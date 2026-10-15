#include "ESP.h"
#include "StatusPanel.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"

#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>
#include <chrono>
#include <algorithm>

namespace ESP {

    struct Vector3 {
        float x, y, z;
        Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
        Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    };

    // Atomic settings
    static std::atomic<bool> s_enabled{false};
    static std::atomic<bool> s_enemyOnly{true}; // Default: filter out teammates!
    static std::atomic<bool> s_boxEnabled{true};
    static std::atomic<bool> s_lineEnabled{true};
    static std::atomic<bool> s_distanceEnabled{true};
    static std::atomic<bool> s_healthEnabled{true};
    static std::atomic<bool> s_nameEnabled{true};

    static std::atomic<int> s_screenWidth{0};
    static std::atomic<int> s_screenHeight{0};
    static std::atomic<float> s_maxDistance{400.0f};

    // Tracking for local player & camera
    static std::atomic<void*> s_localPlayerFPC{nullptr};
    static std::atomic<void*> s_localPlayerTargetibleObject{nullptr};
    static std::atomic<void*> s_activeCamera{nullptr};

    // Frame data buffer protected by mutex
    static std::mutex s_espMutex;
    static std::vector<float> s_espDrawData;
    static std::atomic<uint64_t> s_lastEspUpdateMs{0};

    // Verified Unity engine function pointers from dump.cs
    static void*   (*get_transform)(void *) = nullptr;
    static Vector3 (*Transform_get_position)(void *) = nullptr;
    static void*   (*Camera_get_main)() = nullptr;
    static Vector3 (*Camera_WorldToScreenPoint)(void *, Vector3) = nullptr;
    static void*   (*GetBoneTransform)(void *, int) = nullptr;
    static bool    (*Behaviour_get_isActiveAndEnabled)(void *) = nullptr;
    static bool    (*TargetibleCustomSettings_get_IsAutoAimAllowed)(void *) = nullptr;
    static bool    (*TargetibleCustomSettings_get_AllyObjectToogle)(void *) = nullptr;
    static int     (*BotPlayer_GetTeam)(void *) = nullptr;

    static inline uint64_t GetCurrentTimeMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
    }

    // Safely check if a Unity C# managed object has a valid, non-null internal C++ pointer (m_CachedPtr at +0x10)
    static inline bool IsUnityObjectAlive(void *unityObj) {
        if (!unityObj || !Memory::IsValidPtr(unityObj)) return false;
        uintptr_t addr = reinterpret_cast<uintptr_t>(unityObj);
        if ((addr & 0x7) != 0) return false;
        void *klass = *reinterpret_cast<void**>(addr);
        if (!klass || !Memory::IsValidPtr(klass)) return false;
        void *cachedPtr = *reinterpret_cast<void**>(addr + Offsets::Unity::Transform_m_CachedPtr);
        return (cachedPtr != nullptr && Memory::IsValidPtr(cachedPtr));
    }

    static std::atomic<int> s_localPlayerTeam{-1};

    // Helper: Safely read bot's team enum (-1 = Unknown, 0 = TeamA, 1 = TeamB)
    static int GetBotTeam(void *botPlayer) {
        if (!botPlayer || !Memory::IsValidPtr(botPlayer)) return -1;

        // 1. Native method getter call if resolved
        if (BotPlayer_GetTeam) {
            int t = BotPlayer_GetTeam(botPlayer);
            if (t == 0 || t == 1) return t;
        }

        // 2. Direct team backing field at 0xE0 (<abbabaabbabbaabaaaabbbaababbaaaabaabbbaabbbaaaa>k__BackingField)
        int t0 = Memory::ReadField<int>(botPlayer, 0xE0);
        if (t0 == 0 || t0 == 1) return t0;

        // 3. Secondary team field at 0xE4
        int t1 = Memory::ReadField<int>(botPlayer, 0xE4);
        if (t1 == 0 || t1 == 1) return t1;

        // 4. PlayerSpawnPoint at 0xB0 -> _team at 0x38
        void *spawnPoint = Memory::ReadField<void*>(botPlayer, 0xB0);
        if (spawnPoint && Memory::IsValidPtr(spawnPoint)) {
            int t2 = Memory::ReadField<int>(spawnPoint, 0x38);
            if (t2 == 0 || t2 == 1) return t2;
        }

        return -1;
    }

    // Helper: Check if an entity's CustomSettings designates it as an ally or enemy
    // Returns:
    //  1: Confirmed Enemy (Auto-aim allowed)
    // -1: Confirmed Ally (Ally toggle active)
    //  0: Inconclusive
    static int CheckCustomSettingsAffiliation(void *tObj) {
        if (!tObj || !Memory::IsValidPtr(tObj)) return 0;
        void *customSettings = Memory::ReadField<void*>(tObj, Offsets::Targetible::CustomSettings);
        if (!customSettings || !Memory::IsValidPtr(customSettings)) return 0;

        bool isAutoAim = false;
        if (TargetibleCustomSettings_get_IsAutoAimAllowed) {
            isAutoAim = TargetibleCustomSettings_get_IsAutoAimAllowed(customSettings);
        }

        bool isAlly = false;
        if (TargetibleCustomSettings_get_AllyObjectToogle) {
            isAlly = TargetibleCustomSettings_get_AllyObjectToogle(customSettings);
        } else {
            isAlly = Memory::ReadField<bool>(customSettings, Offsets::CustomSettings::IsAllyToggle);
        }

        if (isAutoAim && !isAlly) {
            return 1; // Confirmed Enemy!
        }
        if (isAlly && !isAutoAim) {
            return -1; // Confirmed Ally/Teammate!
        }
        if (isAlly) {
            return -1; // Ally Toggle on
        }

        return 0;
    }

    // Helper: Resolve local player's team (-1 if unresolved, 0 for TeamA, 1 for TeamB)
    static int ResolveLocalPlayerTeam(void *localFpc) {
        int current = s_localPlayerTeam.load(std::memory_order_relaxed);
        if (current == 0 || current == 1) return current;
        return current;
    }

    // Classify entity affiliation:
    //  1: Confirmed Enemy
    // -1: Confirmed Teammate / Ally / Self
    //  0: Inconclusive / Ignore
    static int ClassifyAffiliation(void *entityObj, bool isBot, void *netPlayer) {
        if (!entityObj || !IsUnityObjectAlive(entityObj)) return 0;

        // 1. Local Player Self Check
        void *localFpc = s_localPlayerFPC.load(std::memory_order_relaxed);
        if (localFpc && entityObj == localFpc) return -1;

        if (isBot) {
            // Check TargetibleObject on the BotPlayer
            void *tObj = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::TargetibleObject);
            int settingAffil = CheckCustomSettingsAffiliation(tObj);
            int botTeam = GetBotTeam(entityObj);

            if (settingAffil == 1) {
                // Confirmed enemy! Local player team is the opposing team
                if (botTeam == 0 || botTeam == 1) {
                    s_localPlayerTeam.store(1 - botTeam, std::memory_order_relaxed);
                }
                return 1; // Enemy!
            } else if (settingAffil == -1) {
                // Confirmed teammate! Local player team is the same team
                if (botTeam == 0 || botTeam == 1) {
                    s_localPlayerTeam.store(botTeam, std::memory_order_relaxed);
                }
                return -1; // Teammate!
            }

            // Fallback: If CustomSettings was inconclusive, use team enum comparison
            int myTeam = s_localPlayerTeam.load(std::memory_order_relaxed);
            if (myTeam < 0) {
                myTeam = ResolveLocalPlayerTeam(localFpc);
            }

            if ((botTeam == 0 || botTeam == 1) && (myTeam == 0 || myTeam == 1)) {
                return (botTeam == myTeam) ? -1 : 1;
            }

            // Inconclusive fallback for bots: Enemy
            return 1;
        } else {
            // NetworkPlayer (Human Player)
            void *netObj = entityObj;
            if (netPlayer && Memory::IsValidPtr(netPlayer)) {
                netObj = netPlayer;
            }

            int myTeam = s_localPlayerTeam.load(std::memory_order_relaxed);
            if (myTeam < 0) {
                myTeam = ResolveLocalPlayerTeam(localFpc);
            }

            void *targetInfo = Memory::ReadField<void*>(netObj, Offsets::NetworkPlayer::TargetInfo);
            if (targetInfo && Memory::IsValidPtr(targetInfo)) {
                int teamVal = Memory::ReadField<int>(targetInfo, 0x30);
                if ((teamVal == 0 || teamVal == 1) && (myTeam == 0 || myTeam == 1)) {
                    return (teamVal == myTeam) ? -1 : 1;
                }
            }

            return 1;
        }
    }

    // Core ESP frame update executed on Unity Render Thread
    static void UpdateESPFrameData(void *cameraObj) {
        if (!s_enabled.load(std::memory_order_relaxed)) return;
        if (!cameraObj || !IsUnityObjectAlive(cameraObj)) return;

        int screenWidth = s_screenWidth.load(std::memory_order_relaxed);
        int screenHeight = s_screenHeight.load(std::memory_order_relaxed);
        if (screenWidth <= 0 || screenHeight <= 0) return;

        // Fetch active entity snapshot from StatusPanel
        std::vector<StatusPanel::EntitySnapshot> entities = StatusPanel::GetActiveEntities();
        if (entities.empty()) {
            std::lock_guard<std::mutex> lock(s_espMutex);
            s_espDrawData.clear();
            s_lastEspUpdateMs.store(GetCurrentTimeMs());
            return;
        }

        // Calibrate local player team if uncalibrated by scanning bots with confirmed settings
        if (s_localPlayerTeam.load(std::memory_order_relaxed) < 0) {
            void *localFpc = s_localPlayerFPC.load(std::memory_order_relaxed);
            ResolveLocalPlayerTeam(localFpc);

            if (s_localPlayerTeam.load(std::memory_order_relaxed) < 0) {
                for (const auto &item : entities) {
                    if (item.isBot && item.object && Memory::IsValidPtr(item.object)) {
                        void *tObj = Memory::ReadField<void*>(item.object, Offsets::BotPlayer::TargetibleObject);
                        int affil = CheckCustomSettingsAffiliation(tObj);
                        int bTeam = GetBotTeam(item.object);
                        if (bTeam == 0 || bTeam == 1) {
                            if (affil == 1) {
                                // Confirmed enemy! Local player team is the opposing team
                                s_localPlayerTeam.store(1 - bTeam, std::memory_order_relaxed);
                                break;
                            } else if (affil == -1) {
                                // Confirmed teammate! Local player team is this team
                                s_localPlayerTeam.store(bTeam, std::memory_order_relaxed);
                                break;
                            }
                        }
                    }
                }
            }
        }

        bool enemyOnly = s_enemyOnly.load(std::memory_order_relaxed);
        float maxDist = s_maxDistance.load(std::memory_order_relaxed);

        std::vector<float> buffer;
        buffer.reserve(entities.size() * 9);

        for (const auto &item : entities) {
            void *entityObj = item.object;
            if (!entityObj || !IsUnityObjectAlive(entityObj)) continue;

            // Alive / Active check in Unity engine
            if (Behaviour_get_isActiveAndEnabled && !Behaviour_get_isActiveAndEnabled(entityObj)) {
                continue;
            }

            bool isBot = item.isBot;
            void *netPlayer = item.netPlayer;

            // Health check for bots
            int currentHp = 100;
            int maxHp = 100;
            if (isBot) {
                void *botHealth = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::BotHealth);
                if (botHealth && Memory::IsValidPtr(botHealth)) {
                    float hp = Memory::ReadField<float>(botHealth, 0x2C);
                    float mHp = Memory::ReadField<float>(botHealth, 0x30);
                    if (hp <= 0.0f) continue; // Dead entity, skip!
                    currentHp = static_cast<int>(hp);
                    maxHp = (mHp > 0.0f) ? static_cast<int>(mHp) : 100;
                }
            }

            // 1. Teammate & Enemy Filtering
            int affil = ClassifyAffiliation(entityObj, isBot, netPlayer);
            if (affil == -1 && enemyOnly) {
                continue; // Strictly filter out teammates and self!
            }
            bool isEnemy = (affil == 1);

            // 2. Resolve Head & Root Transforms
            // Root Transform is ALWAYS the entity's base GameObject transform (floor / center capsule)
            void *rootTrans = get_transform ? get_transform(entityObj) : nullptr;
            if (!rootTrans || !IsUnityObjectAlive(rootTrans)) continue;

            void *headTrans = nullptr;
            if (isBot) {
                // ThirdPersonController at offset 0x58 -> Animator at 0x78 -> HumanBodyBones.Head (10)
                void *tpCtrl = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::ThirdPersonCtrl);
                if (tpCtrl && IsUnityObjectAlive(tpCtrl)) {
                    void *animator = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::AnimatorOffset);
                    if (animator && IsUnityObjectAlive(animator) && GetBoneTransform) {
                        headTrans = GetBoneTransform(animator, 10);
                    }
                }
            } else {
                // NetworkPlayer: DollsManager at offset 0x88 -> TPCtrl -> Animator -> Bone 10
                void *dollsMgr = Memory::ReadField<void*>(entityObj, Offsets::NetworkPlayer::DollsManager);
                if (dollsMgr && Memory::IsValidPtr(dollsMgr)) {
                    void *tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::RemoteTPCtrl);
                    if (!tpCtrl) tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::BotTPCtrl);
                    if (tpCtrl && IsUnityObjectAlive(tpCtrl)) {
                        void *animator = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::AnimatorOffset);
                        if (animator && IsUnityObjectAlive(animator) && GetBoneTransform) {
                            headTrans = GetBoneTransform(animator, 10);
                        }
                    }
                }
            }

            // 3. Obtain 3D Positions in World Space
            Vector3 footPos = Transform_get_position ? Transform_get_position(rootTrans) : Vector3();
            if (footPos.x == 0.0f && footPos.y == 0.0f && footPos.z == 0.0f) continue;

            Vector3 headPos;
            bool validBone = false;
            if (headTrans && IsUnityObjectAlive(headTrans) && Transform_get_position) {
                Vector3 bonePos = Transform_get_position(headTrans);
                float dx = bonePos.x - footPos.x;
                float dz = bonePos.z - footPos.z;
                float distSq = dx * dx + dz * dz;

                // Validate bone: must be vertically above feet and horizontally within 0.55m radius of capsule center
                if (bonePos.y > (footPos.y + 0.8f) && bonePos.y < (footPos.y + 2.4f) && distSq < 0.30f) {
                    // Keep horizontal X & Z strictly centered on footPos capsule, use bone Y for dynamic height/crouching!
                    headPos = Vector3(footPos.x, bonePos.y + 0.18f, footPos.z);
                    validBone = true;
                }
            }

            if (!validBone) {
                // Default standing character height: 1.80m above feet
                headPos = Vector3(footPos.x, footPos.y + 1.80f, footPos.z);
            }

            // 4. World to Screen projection using Unity Camera.WorldToScreenPoint
            if (!Camera_WorldToScreenPoint) continue;

            Vector3 sHead = Camera_WorldToScreenPoint(cameraObj, headPos);
            Vector3 sFoot = Camera_WorldToScreenPoint(cameraObj, footPos);

            // In Unity WorldToScreenPoint: z is distance in front of the camera plane
            if (sHead.z < 0.2f && sFoot.z < 0.2f) {
                continue; // Behind the camera view!
            }

            float dist = (sFoot.z > 0.2f) ? sFoot.z : sHead.z;
            if (dist < 0.2f || dist > maxDist) continue;

            // Convert Unity Screen coordinates (0,0 is bottom-left) to Android Canvas (0,0 is top-left)
            float screenHeadX = sHead.x;
            float screenHeadY = static_cast<float>(screenHeight) - sHead.y;
            float screenRootX = sFoot.x;
            float screenRootY = static_cast<float>(screenHeight) - sFoot.y;

            // Offscreen bounds check (skip entities far outside viewport)
            float off = static_cast<float>(screenWidth) * 0.4f;
            if (screenHeadX < -off || screenHeadX > static_cast<float>(screenWidth) + off ||
                screenHeadY < -off || screenHeadY > static_cast<float>(screenHeight) + off) {
                continue;
            }

            // Push 9 serialized float values for high-speed Android JNI consumption
            buffer.push_back(screenRootX);
            buffer.push_back(screenRootY);
            buffer.push_back(screenHeadX);
            buffer.push_back(screenHeadY);
            buffer.push_back(dist);
            buffer.push_back(static_cast<float>(currentHp));
            buffer.push_back(static_cast<float>(maxHp));
            buffer.push_back(isBot ? 1.0f : 0.0f);
            buffer.push_back(isEnemy ? 1.0f : 0.0f);
        }

        // Thread-safe swap of pre-calculated draw buffer
        {
            std::lock_guard<std::mutex> lock(s_espMutex);
            s_espDrawData = std::move(buffer);
        }
        s_lastEspUpdateMs.store(GetCurrentTimeMs());
    }

    // AimingControl.Update hook: RVA 0x4D27294
    static void (*old_AimingControl_Update)(void *instance) = nullptr;
    static void hook_AimingControl_Update(void *instance) {
        if (instance) {
            OnAimingControlUpdate(instance);
        }
        if (old_AimingControl_Update) old_AimingControl_Update(instance);
    }

    void OnAimingControlUpdate(void *aimingControl) {
        if (!aimingControl || !IsUnityObjectAlive(aimingControl)) return;

        void *cam = Memory::ReadField<void*>(aimingControl, Offsets::AimingControl::Camera);
        if (cam && IsUnityObjectAlive(cam)) {
            s_activeCamera.store(cam, std::memory_order_relaxed);
        } else if (Camera_get_main) {
            void *mainCam = Camera_get_main();
            if (mainCam && IsUnityObjectAlive(mainCam)) {
                s_activeCamera.store(mainCam, std::memory_order_relaxed);
                cam = mainCam;
            }
        }

        if (s_enabled.load(std::memory_order_relaxed) && cam) {
            UpdateESPFrameData(cam);
        }
    }

    void OnLocalPlayerUpdate(void *fpc) {
        if (!fpc || !IsUnityObjectAlive(fpc)) return;
        void *oldFpc = s_localPlayerFPC.exchange(fpc, std::memory_order_relaxed);
        if (oldFpc != fpc) {
            s_localPlayerTeam.store(-1, std::memory_order_relaxed);
        }

        ResolveLocalPlayerTeam(fpc);

        void *tObj = Memory::ReadField<void*>(fpc, Offsets::FPC::TargetibleObject);
        if (tObj && IsUnityObjectAlive(tObj)) {
            s_localPlayerTargetibleObject.store(tObj, std::memory_order_relaxed);
        }

        // Fallback camera resolution if AimingControl hook is not yet active
        void *cam = s_activeCamera.load(std::memory_order_relaxed);
        if (!cam || !IsUnityObjectAlive(cam)) {
            cam = Memory::ReadField<void*>(fpc, Offsets::FPC::FPSCamera);
            if (cam && IsUnityObjectAlive(cam)) {
                s_activeCamera.store(cam, std::memory_order_relaxed);
            }
        }

        if (s_enabled.load(std::memory_order_relaxed) && cam) {
            static uint64_t s_lastFpcEspUpdate = 0;
            uint64_t now = GetCurrentTimeMs();
            // Update at ~60 FPS rate if AimingControl isn't firing
            if (now - s_lastFpcEspUpdate >= 16) {
                s_lastFpcEspUpdate = now;
                UpdateESPFrameData(cam);
            }
        }
    }

    void Init() {
        LOG_INFO("Initializing ESP Module with verified dump.cs RVAs...");

        // Resolve verified Unity functions from dump.cs
        get_transform = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::Unity::GetTransform));
        Transform_get_position = reinterpret_cast<Vector3(*)(void*)>(Memory::GetRVA(Offsets::Unity::TransformGetPosition));
        Camera_get_main = reinterpret_cast<void*(*)()>(Memory::GetRVA(Offsets::Unity::CameraGetMain));
        Camera_WorldToScreenPoint = reinterpret_cast<Vector3(*)(void*, Vector3)>(Memory::GetRVA(Offsets::Unity::CameraWorldToScreenPoint));
        GetBoneTransform = reinterpret_cast<void*(*)(void*, int)>(Memory::GetRVA(Offsets::Unity::GetBoneTransform));
        Behaviour_get_isActiveAndEnabled = reinterpret_cast<bool(*)(void*)>(Memory::GetRVA(Offsets::Unity::BehaviourGetIsActive));

        // Resolve TargetibleCustomSettings & BotPlayer team functions from dump.cs
        TargetibleCustomSettings_get_IsAutoAimAllowed = reinterpret_cast<bool(*)(void*)>(Memory::GetRVA(Offsets::CustomSettings::GetIsAutoAimAllowed));
        TargetibleCustomSettings_get_AllyObjectToogle = reinterpret_cast<bool(*)(void*)>(Memory::GetRVA(Offsets::CustomSettings::GetAllyObjectToggle));
        BotPlayer_GetTeam = reinterpret_cast<int(*)(void*)>(Memory::GetRVA(Offsets::BotPlayer::GetTeam));

        // Hook AimingControl.Update
        Memory::HookRVA(Offsets::AimingControl::Update, reinterpret_cast<void*>(hook_AimingControl_Update), reinterpret_cast<void**>(&old_AimingControl_Update));

        LOG_INFO("[ESP] ESP module initialized: AimingControl.Update hooked and Unity functions resolved.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        if (!enable) {
            std::lock_guard<std::mutex> lock(s_espMutex);
            s_espDrawData.clear();
        }
        LOG_INFO("[ESP] Master toggle: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

    void SetEnemyOnly(bool enemyOnly) {
        s_enemyOnly.store(enemyOnly, std::memory_order_relaxed);
        LOG_INFO("[ESP] Enemy-only filter: %s", enemyOnly ? "ON (Enemies Only)" : "OFF (All)");
    }

    bool IsEnemyOnly() {
        return s_enemyOnly.load(std::memory_order_relaxed);
    }

    void SetBoxEnabled(bool enable) {
        s_boxEnabled.store(enable, std::memory_order_relaxed);
    }

    bool IsBoxEnabled() {
        return s_boxEnabled.load(std::memory_order_relaxed);
    }

    void SetLineEnabled(bool enable) {
        s_lineEnabled.store(enable, std::memory_order_relaxed);
    }

    bool IsLineEnabled() {
        return s_lineEnabled.load(std::memory_order_relaxed);
    }

    void SetDistanceEnabled(bool enable) {
        s_distanceEnabled.store(enable, std::memory_order_relaxed);
    }

    bool IsDistanceEnabled() {
        return s_distanceEnabled.load(std::memory_order_relaxed);
    }

    void SetHealthEnabled(bool enable) {
        s_healthEnabled.store(enable, std::memory_order_relaxed);
    }

    bool IsHealthEnabled() {
        return s_healthEnabled.load(std::memory_order_relaxed);
    }

    void SetNameEnabled(bool enable) {
        s_nameEnabled.store(enable, std::memory_order_relaxed);
    }

    bool IsNameEnabled() {
        return s_nameEnabled.load(std::memory_order_relaxed);
    }

    // JNI Native consumer called on Android UI thread (Zero Unity calls, non-blocking)
    jfloatArray GetESPData(JNIEnv *env, jint screenWidth, jint screenHeight) {
        if (screenWidth > 0 && screenHeight > 0) {
            s_screenWidth.store(screenWidth, std::memory_order_relaxed);
            s_screenHeight.store(screenHeight, std::memory_order_relaxed);
        }

        if (!s_enabled.load(std::memory_order_relaxed)) {
            return env->NewFloatArray(0);
        }

        uint64_t now = GetCurrentTimeMs();
        uint64_t last = s_lastEspUpdateMs.load(std::memory_order_relaxed);
        if (now - last > 500) {
            // Data is older than 500ms (game paused or match ended)
            return env->NewFloatArray(0);
        }

        std::vector<float> snapshot;
        {
            std::lock_guard<std::mutex> lock(s_espMutex);
            if (s_espDrawData.empty()) {
                return env->NewFloatArray(0);
            }
            snapshot = s_espDrawData;
        }

        jfloatArray result = env->NewFloatArray(static_cast<jsize>(snapshot.size()));
        if (result && !snapshot.empty()) {
            env->SetFloatArrayRegion(result, 0, static_cast<jsize>(snapshot.size()), snapshot.data());
        }
        return result;
    }

    jboolean GetESPSetting(jint settingId) {
        switch (settingId) {
            case 0: return static_cast<jboolean>(s_enabled.load(std::memory_order_relaxed));
            case 1: return static_cast<jboolean>(s_boxEnabled.load(std::memory_order_relaxed));
            case 2: return static_cast<jboolean>(s_lineEnabled.load(std::memory_order_relaxed));
            case 3: return static_cast<jboolean>(s_distanceEnabled.load(std::memory_order_relaxed));
            case 4: return static_cast<jboolean>(s_healthEnabled.load(std::memory_order_relaxed));
            case 5: return static_cast<jboolean>(s_nameEnabled.load(std::memory_order_relaxed));
            case 6: return static_cast<jboolean>(s_enemyOnly.load(std::memory_order_relaxed));
            default: return JNI_FALSE;
        }
    }

} // namespace ESP
