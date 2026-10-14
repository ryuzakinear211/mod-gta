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
    static std::atomic<bool> s_enemyOnly{true}; // Default: strictly filter teammates!
    static std::atomic<bool> s_boxEnabled{true};
    static std::atomic<bool> s_lineEnabled{true};
    static std::atomic<bool> s_distanceEnabled{true};
    static std::atomic<bool> s_healthEnabled{true};
    static std::atomic<bool> s_nameEnabled{true};

    static std::atomic<int> s_screenWidth{0};
    static std::atomic<int> s_screenHeight{0};
    static std::atomic<float> s_maxDistance{400.0f};

    // Tracking for local player, team & camera
    static std::atomic<void*> s_localPlayerFPC{nullptr};
    static std::atomic<void*> s_localPlayerNetPlayer{nullptr};
    static std::atomic<void*> s_localPlayerTargetibleObject{nullptr};
    static std::atomic<int>   s_localPlayerTeam{-1}; // -1 = uncalibrated, 0 = team 0, 1 = team 1
    static std::atomic<void*> s_activeCamera{nullptr};

    // Data buffer protected by mutex
    static std::mutex s_espMutex;
    static std::vector<float> s_espDrawData;
    static std::atomic<uint64_t> s_lastEspUpdateMs{0};

    // Engine function pointers
    static void* (*get_transform)(void *) = nullptr;
    static void (*get_position_Injected)(void *, Vector3 *) = nullptr;
    static void* (*Camera_get_main)() = nullptr;
    static void (*WorldToViewportPoint_Injected)(void *, const Vector3 *, int, Vector3 *) = nullptr;
    static void* (*GetBoneTransform)(void *, int) = nullptr;
    static bool (*Behaviour_get_isActiveAndEnabled)(void *) = nullptr;
    static void* (*NetworkPlayer_GetTargetibleObject)(void *) = nullptr;
    static void* (*BotPlayer_GetTargetibleObject)(void *) = nullptr;
    static int (*BotPlayer_GetTeam)(void *) = nullptr;
    static float (*BotPlayerHealth_GetHealth)(void *) = nullptr;
    static bool (*Targetible_GetAllyObjectToggle)(void *) = nullptr;
    static bool (*Targetible_GetIsAutoAimAllowed)(void *) = nullptr;
    static void* (*FPC_GetNetworkPlayer)(void *) = nullptr;

    static inline uint64_t GetCurrentTimeMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
    }

    // Safely dereference Unity's internal native pointer (m_CachedPtr at offset 0x10)
    static inline void* GetNativeUnityPointer(void *unityObj) {
        if (!unityObj || !Memory::IsValidPtr(unityObj)) return nullptr;
        uintptr_t addr = reinterpret_cast<uintptr_t>(unityObj);
        if ((addr & 0x7) != 0) return nullptr;
        void *klass = *reinterpret_cast<void**>(addr);
        if (!klass || !Memory::IsValidPtr(klass)) return nullptr;
        if (!Memory::IsValidPtr(reinterpret_cast<const void*>(addr + Offsets::Unity::Transform_m_CachedPtr))) return nullptr;
        void *cachedPtr = *reinterpret_cast<void**>(addr + Offsets::Unity::Transform_m_CachedPtr);
        if (!cachedPtr || !Memory::IsValidPtr(cachedPtr)) return nullptr;
        return cachedPtr;
    }

    static inline bool IsUnityObjectAlive(void *unityObj) {
        return GetNativeUnityPointer(unityObj) != nullptr;
    }

    static inline Vector3 GetTransformPosition(void *transformObj) {
        Vector3 pos(0.0f, 0.0f, 0.0f);
        if (!transformObj || !get_position_Injected) return pos;
        void *nativeTrans = GetNativeUnityPointer(transformObj);
        if (!nativeTrans) return pos;
        get_position_Injected(nativeTrans, &pos);
        return pos;
    }

    static inline bool WorldToViewport(void *cameraObj, const Vector3 &worldPos, Vector3 &outVp) {
        if (!cameraObj || !WorldToViewportPoint_Injected) return false;
        void *nativeCam = GetNativeUnityPointer(cameraObj);
        if (!nativeCam) return false;
        WorldToViewportPoint_Injected(nativeCam, &worldPos, 2, &outVp);
        return (outVp.z > 0.1f && !std::isnan(outVp.x) && !std::isnan(outVp.y) && !std::isnan(outVp.z));
    }

    // Read bot team via field offsets or getter
    static inline int GetBotTeamDirect(void *botPlayer) {
        if (!botPlayer || !Memory::IsValidPtr(botPlayer)) return -1;
        // 1. Direct field read at offset 0xE0
        if (Memory::IsValidPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(botPlayer) + 0xE0))) {
            int tE0 = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(botPlayer) + 0xE0);
            if (tE0 == 0 || tE0 == 1) return tE0;
        }
        // 2. Direct field read at offset 0xE4
        if (Memory::IsValidPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(botPlayer) + 0xE4))) {
            int tE4 = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(botPlayer) + 0xE4);
            if (tE4 == 0 || tE4 == 1) return tE4;
        }
        // 3. Getter
        if (BotPlayer_GetTeam) {
            int t = BotPlayer_GetTeam(botPlayer);
            if (t == 0 || t == 1) return t;
        }
        return -1;
    }

    static inline bool IsValidTargetType(int val) {
        return (val > 0 && (val & 0xFF) != 0 && (val & ~0xFF) == 0);
    }

    static inline int ExtractTargetType(void *targetInfo) {
        if (!targetInfo || !Memory::IsValidPtr(targetInfo)) return 0;
        // Remote TargetInfo at offset 0x34
        if (Memory::IsValidPtr(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(targetInfo) + 0x34))) {
            int val34 = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(targetInfo) + 0x34);
            if (IsValidTargetType(val34)) return val34;
        }
        // Local TargetInfo at offset 0x30
        if (Memory::IsValidPtr(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30))) {
            int val30 = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30);
            if (IsValidTargetType(val30)) return val30;
        }
        return 0;
    }

    // Classify entity affiliation:
    //  1: Confirmed Enemy
    // -1: Confirmed Ally / Teammate / Self
    //  0: Inconclusive
    static int ClassifyAffiliation(void *netPlayer, void *targetibleObj, void *botPlayer) {
        // Cross-resolve pointers if missing
        if (botPlayer && Memory::IsValidPtr(botPlayer)) {
            if (!netPlayer) {
                netPlayer = Memory::ReadField<void*>(botPlayer, Offsets::BotPlayer::NetPlayer);
            }
            if (!targetibleObj) {
                targetibleObj = Memory::ReadField<void*>(botPlayer, Offsets::BotPlayer::TargetibleObject);
                if (!targetibleObj && BotPlayer_GetTargetibleObject) {
                    targetibleObj = BotPlayer_GetTargetibleObject(botPlayer);
                }
            }
        }
        if (netPlayer && Memory::IsValidPtr(netPlayer) && !targetibleObj) {
            if (NetworkPlayer_GetTargetibleObject) {
                targetibleObj = NetworkPlayer_GetTargetibleObject(netPlayer);
            }
        }

        // Strict Self / Local Player Exclusion
        void *localNet = s_localPlayerNetPlayer.load();
        if (localNet && netPlayer && netPlayer == localNet) return -1;

        void *localTObj = s_localPlayerTargetibleObject.load();
        if (localTObj && targetibleObj && targetibleObj == localTObj) return -1;

        void *localFpc = s_localPlayerFPC.load();
        if (localFpc && botPlayer && botPlayer == localFpc) return -1;

        // Bot Team vs My Team
        int targetTeam = (botPlayer) ? GetBotTeamDirect(botPlayer) : -1;
        int myTeam = s_localPlayerTeam.load();
        if (myTeam >= 0 && targetTeam >= 0) {
            if (targetTeam == myTeam) return -1; // Teammate
            else return 1;                       // Enemy
        }

        // TargetInfo bitmask check
        if (netPlayer && Memory::IsValidPtr(netPlayer)) {
            void *targetInfo = Memory::ReadField<void*>(netPlayer, Offsets::NetworkPlayer::TargetInfo);
            if (targetInfo && Memory::IsValidPtr(targetInfo)) {
                int tType = ExtractTargetType(targetInfo);
                if (tType != 0) {
                    if (tType == 1) { // LocalPlayer
                        s_localPlayerNetPlayer.store(netPlayer);
                        return -1;
                    }
                    if ((tType & (2 | 8 | 32)) != 0) { // Ally
                        if (targetTeam >= 0) s_localPlayerTeam.store(targetTeam);
                        return -1;
                    }
                    if ((tType & (4 | 16 | 64 | 128)) != 0) { // Enemy
                        if (targetTeam >= 0) s_localPlayerTeam.store(1 - targetTeam);
                        return 1;
                    }
                }
            }
        }

        // TargetibleObject CustomSettings check
        if (targetibleObj && Memory::IsValidPtr(targetibleObj)) {
            void *customSettings = Memory::ReadField<void*>(targetibleObj, Offsets::Targetible::CustomSettings);
            if (customSettings && Memory::IsValidPtr(customSettings)) {
                if (Targetible_GetAllyObjectToggle && Targetible_GetAllyObjectToggle(customSettings)) {
                    if (targetTeam >= 0) s_localPlayerTeam.store(targetTeam);
                    return -1;
                }
                if (Targetible_GetIsAutoAimAllowed && Targetible_GetIsAutoAimAllowed(customSettings)) {
                    if (targetTeam >= 0) s_localPlayerTeam.store(1 - targetTeam);
                    return 1;
                }
            }
        }

        if (targetTeam >= 0 && myTeam >= 0) {
            return (targetTeam == myTeam) ? -1 : 1;
        }

        return 0; // Inconclusive
    }

    // Core ESP collection executed synchronously on Unity Main Render Thread
    static void UpdateESPFrameData(void *cameraObj) {
        if (!s_enabled.load(std::memory_order_relaxed)) return;
        if (!cameraObj || !IsUnityObjectAlive(cameraObj)) return;

        int screenWidth = s_screenWidth.load(std::memory_order_relaxed);
        int screenHeight = s_screenHeight.load(std::memory_order_relaxed);
        if (screenWidth <= 0 || screenHeight <= 0) return;

        // Fetch active entities from StatusPanel snapshot
        std::vector<StatusPanel::EntitySnapshot> entities = StatusPanel::GetActiveEntities();
        if (entities.empty()) {
            std::lock_guard<std::mutex> lock(s_espMutex);
            s_espDrawData.clear();
            s_lastEspUpdateMs.store(GetCurrentTimeMs());
            return;
        }

        // Proactive Team Calibration if not yet calibrated
        if (s_localPlayerTeam.load() == -1) {
            for (const auto &item : entities) {
                void *botObj = item.isBot ? item.object : nullptr;
                void *netObj = !item.isBot ? item.object : nullptr;
                void *tObj = nullptr;
                if (botObj) {
                    tObj = Memory::ReadField<void*>(botObj, Offsets::BotPlayer::TargetibleObject);
                    if (!tObj && BotPlayer_GetTargetibleObject) tObj = BotPlayer_GetTargetibleObject(botObj);
                }
                if (netObj && NetworkPlayer_GetTargetibleObject) {
                    tObj = NetworkPlayer_GetTargetibleObject(netObj);
                }
                ClassifyAffiliation(netObj, tObj, botObj);
                if (s_localPlayerTeam.load() != -1) break;
            }
        }

        bool enemyOnly = s_enemyOnly.load(std::memory_order_relaxed);
        float maxDist = s_maxDistance.load(std::memory_order_relaxed);

        std::vector<float> buffer;
        buffer.reserve(entities.size() * 9);

        for (const auto &item : entities) {
            void *entityObj = item.object;
            if (!entityObj || !IsUnityObjectAlive(entityObj)) continue;

            bool isBot = item.isBot;
            void *netPlayer = item.netPlayer;
            void *targetibleObj = nullptr;

            if (isBot) {
                targetibleObj = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::TargetibleObject);
                if (!targetibleObj && BotPlayer_GetTargetibleObject) targetibleObj = BotPlayer_GetTargetibleObject(entityObj);
            } else {
                if (NetworkPlayer_GetTargetibleObject) targetibleObj = NetworkPlayer_GetTargetibleObject(entityObj);
            }

            // 1. Team & Enemy Filtering
            int affil = ClassifyAffiliation(isBot ? netPlayer : entityObj, targetibleObj, isBot ? entityObj : nullptr);

            // If enemy-only is enabled, skip self and teammates
            if (enemyOnly) {
                if (affil == -1) continue; // Filter out teammate / self!
            } else {
                if (affil == -1) {
                    // Check if self
                    void *localNet = s_localPlayerNetPlayer.load();
                    if (localNet && (entityObj == localNet || netPlayer == localNet)) continue;
                }
            }

            bool isEnemy = (affil == 1);

            // 2. Dead / Corpse Check
            if (Behaviour_get_isActiveAndEnabled && !Behaviour_get_isActiveAndEnabled(entityObj)) {
                continue; // Entity inactive or dead
            }

            int currentHp = 100;
            int maxHp = 100;
            if (isBot) {
                void *botHealth = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::BotHealth);
                if (botHealth && Memory::IsValidPtr(botHealth)) {
                    float hp = 0.0f;
                    if (BotPlayerHealth_GetHealth) {
                        hp = BotPlayerHealth_GetHealth(botHealth);
                    } else if (Memory::IsValidPtr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C))) {
                        hp = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C);
                    }
                    if (hp <= 0.0f) continue; // Dead bot!
                    currentHp = static_cast<int>(hp);
                }
            }

            // 3. Resolve Root & Head Transform
            void *rootTrans = nullptr;
            if (get_transform) {
                rootTrans = get_transform(entityObj);
                if ((!rootTrans || !IsUnityObjectAlive(rootTrans)) && netPlayer && IsUnityObjectAlive(netPlayer)) {
                    rootTrans = get_transform(netPlayer);
                }
            }
            if (!rootTrans || !IsUnityObjectAlive(rootTrans)) continue;

            Vector3 charRoot = GetTransformPosition(rootTrans);
            if (charRoot.x == 0.0f && charRoot.y == 0.0f && charRoot.z == 0.0f) continue;

            void *headBone = nullptr;
            if (isBot) {
                void *botLook = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::BotLook);
                if (botLook && Memory::IsValidPtr(botLook)) {
                    headBone = Memory::ReadField<void*>(botLook, 0x28);
                }
            } else {
                void *dollsMgr = Memory::ReadField<void*>(entityObj, Offsets::NetworkPlayer::DollsManager);
                if (dollsMgr && Memory::IsValidPtr(dollsMgr)) {
                    void *tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::RemoteTPCtrl);
                    if (!tpCtrl) tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::BotTPCtrl);
                    if (tpCtrl && Memory::IsValidPtr(tpCtrl)) {
                        void *animator = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::AnimatorOffset);
                        if (animator && Memory::IsValidPtr(animator) && GetBoneTransform) {
                            headBone = GetBoneTransform(animator, 10); // Head bone = 10
                        }
                    }
                }
            }

            Vector3 headPos = (headBone && IsUnityObjectAlive(headBone)) ? GetTransformPosition(headBone)
                                                                        : Vector3(charRoot.x, charRoot.y + 1.65f, charRoot.z);
            if (headPos.x == 0.0f && headPos.y == 0.0f && headPos.z == 0.0f) {
                headPos = Vector3(charRoot.x, charRoot.y + 1.65f, charRoot.z);
            }

            float groundY = charRoot.y;
            if (groundY > headPos.y - 0.5f || groundY < headPos.y - 2.5f) {
                groundY = headPos.y - 1.70f;
            }

            Vector3 worldHead(headPos.x, headPos.y + 0.15f, headPos.z);
            Vector3 worldFeet(headPos.x, groundY, headPos.z);

            Vector3 vpHead(0.0f, 0.0f, 0.0f);
            Vector3 vpFeet(0.0f, 0.0f, 0.0f);
            bool headValid = WorldToViewport(cameraObj, worldHead, vpHead);
            bool feetValid = WorldToViewport(cameraObj, worldFeet, vpFeet);

            if ((!headValid && !feetValid) || (vpHead.z < 0.2f && vpFeet.z < 0.2f)) {
                continue; // Behind camera
            }

            float dist = (vpFeet.z > 0.2f) ? vpFeet.z : vpHead.z;
            if (dist < 0.2f || dist > maxDist) continue;

            float screenHeadX = vpHead.x * static_cast<float>(screenWidth);
            float screenHeadY = (1.0f - vpHead.y) * static_cast<float>(screenHeight);
            float screenRootX = vpFeet.x * static_cast<float>(screenWidth);
            float screenRootY = (1.0f - vpFeet.y) * static_cast<float>(screenHeight);

            float approxH = (static_cast<float>(screenHeight) * 1.70f) / std::max(0.5f, dist);
            if (!headValid || vpHead.z < 0.2f) {
                screenHeadX = screenRootX;
                screenHeadY = screenRootY - approxH;
            } else if (!feetValid || vpFeet.z < 0.2f) {
                screenRootX = screenHeadX;
                screenRootY = screenHeadY + approxH;
            }

            float offMargin = static_cast<float>(screenWidth) * 0.4f;
            if (screenHeadX < -offMargin || screenHeadX > static_cast<float>(screenWidth) + offMargin ||
                screenHeadY < -offMargin || screenHeadY > static_cast<float>(screenHeight) + offMargin) {
                continue;
            }

            // Push 9 serialized float values for JNI
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
        if (!aimingControl) return;

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
        if (!fpc) return;
        s_localPlayerFPC.store(fpc, std::memory_order_relaxed);

        if (FPC_GetNetworkPlayer) {
            void *net = FPC_GetNetworkPlayer(fpc);
            if (net && IsUnityObjectAlive(net)) {
                s_localPlayerNetPlayer.store(net, std::memory_order_relaxed);
            }
        }

        void *tObj = Memory::ReadField<void*>(fpc, Offsets::FPC::TargetibleObject);
        if (tObj && IsUnityObjectAlive(tObj)) {
            s_localPlayerTargetibleObject.store(tObj, std::memory_order_relaxed);
        }
    }

    void Init() {
        LOG_INFO("Initializing ESP Module...");

        // Resolve function pointers
        get_transform = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::Unity::GetTransform));
        get_position_Injected = reinterpret_cast<void(*)(void*, Vector3*)>(Memory::GetRVA(Offsets::Unity::GetPositionInjected));
        Camera_get_main = reinterpret_cast<void*(*)()>(Memory::GetRVA(Offsets::Unity::CameraGetMain));
        WorldToViewportPoint_Injected = reinterpret_cast<void(*)(void*, const Vector3*, int, Vector3*)>(Memory::GetRVA(Offsets::Unity::WorldToViewportPointInjected));
        GetBoneTransform = reinterpret_cast<void*(*)(void*, int)>(Memory::GetRVA(Offsets::Unity::GetBoneTransform));
        Behaviour_get_isActiveAndEnabled = reinterpret_cast<bool(*)(void*)>(Memory::GetRVA(Offsets::Unity::BehaviourGetIsActive));
        NetworkPlayer_GetTargetibleObject = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::NetworkPlayer::GetTargetibleObject));
        BotPlayer_GetTargetibleObject = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::BotPlayer::GetTargetibleObject));
        BotPlayer_GetTeam = reinterpret_cast<int(*)(void*)>(Memory::GetRVA(Offsets::BotPlayer::GetTeam));
        BotPlayerHealth_GetHealth = reinterpret_cast<float(*)(void*)>(Memory::GetRVA(Offsets::BotPlayerHealth::GetHealth));
        Targetible_GetAllyObjectToggle = reinterpret_cast<bool(*)(void*)>(Memory::GetRVA(Offsets::Targetible::GetAllyObjectToggle));
        Targetible_GetIsAutoAimAllowed = reinterpret_cast<bool(*)(void*)>(Memory::GetRVA(Offsets::Targetible::GetIsAutoAimAllowed));
        FPC_GetNetworkPlayer = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::FPC::GetNetworkPlayer));

        // Hook AimingControl.Update
        Memory::HookRVA(Offsets::AimingControl::Update, reinterpret_cast<void*>(hook_AimingControl_Update), reinterpret_cast<void**>(&old_AimingControl_Update));

        LOG_INFO("[ESP] ESP module initialized and AimingControl.Update hooked.");
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

    // JNI Native consumer called on Android UI thread (Zero Unity engine calls)
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
