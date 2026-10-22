#include "ESP.h"
#include "StatusPanel.h"
#include "BigHead.h"
#include "Chams.h"
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



    struct EntityBones {
        void *headTrans = nullptr;
        void *chestTrans = nullptr;
        void *hipsTrans = nullptr;
        void *leftFootTrans = nullptr;
        void *rightFootTrans = nullptr;
        void *legsTrans = nullptr;
    };

    static EntityBones ResolveEntityBones(void *entityObj, bool isBot, void *netPlayer) {
        EntityBones bones;

        // Collect possible ThirdPersonController pointers to locate the Animator
        void *tpCtrl = nullptr;

        if (isBot) {
            // 1. BotPlayer -> ThirdPersonController (0x58)
            tpCtrl = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::ThirdPersonCtrl);

            // 2. Fallback: BotPlayer -> NetPlayer (0x50) -> DollsManager (0x88)
            if (!tpCtrl || !IsUnityObjectAlive(tpCtrl)) {
                void *botNet = netPlayer;
                if (!botNet || !Memory::IsValidPtr(botNet)) {
                    botNet = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::NetPlayer);
                }
                if (botNet && Memory::IsValidPtr(botNet)) {
                    void *dollsMgr = Memory::ReadField<void*>(botNet, Offsets::NetworkPlayer::DollsManager);
                    if (dollsMgr && Memory::IsValidPtr(dollsMgr)) {
                        tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::BotTPCtrl);
                        if (!tpCtrl || !IsUnityObjectAlive(tpCtrl)) {
                            tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::RemoteTPCtrl);
                        }
                    }
                }
            }
        } else {
            // NetworkPlayer -> DollsManager (0x88)
            void *dollsMgr = Memory::ReadField<void*>(entityObj, Offsets::NetworkPlayer::DollsManager);
            if (dollsMgr && Memory::IsValidPtr(dollsMgr)) {
                tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::RemoteTPCtrl);
                if (!tpCtrl || !IsUnityObjectAlive(tpCtrl)) {
                    tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::BotTPCtrl);
                }
            }
        }

        // 1. Query Animator bones using Mecanim GetBoneTransform (matching BigHead structure)
        if (tpCtrl && IsUnityObjectAlive(tpCtrl) && GetBoneTransform != nullptr) {
            void *animator = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::AnimatorOffset);
            if (animator && IsUnityObjectAlive(animator)) {
                // Head bone (10) / Neck (9)
                void *h = GetBoneTransform(animator, Offsets::Bones::Head);
                if (h && IsUnityObjectAlive(h)) bones.headTrans = h;
                else {
                    void *neck = GetBoneTransform(animator, Offsets::Bones::Neck);
                    if (neck && IsUnityObjectAlive(neck)) bones.headTrans = neck;
                }

                // Chest bone (8) -> UpperChest (54) -> Spine (7)
                void *c = GetBoneTransform(animator, Offsets::Bones::Chest);
                if (c && IsUnityObjectAlive(c)) bones.chestTrans = c;
                else {
                    void *uc = GetBoneTransform(animator, Offsets::Bones::UpperChest);
                    if (uc && IsUnityObjectAlive(uc)) bones.chestTrans = uc;
                    else {
                        void *sp = GetBoneTransform(animator, Offsets::Bones::Spine);
                        if (sp && IsUnityObjectAlive(sp)) bones.chestTrans = sp;
                    }
                }

                // Hips bone (0)
                void *hips = GetBoneTransform(animator, Offsets::Bones::Hips);
                if (hips && IsUnityObjectAlive(hips)) {
                    bones.hipsTrans = hips;
                    if (!bones.chestTrans) bones.chestTrans = hips;
                }

                // Foot bones (5 & 6)
                void *lf = GetBoneTransform(animator, Offsets::Bones::LeftFoot);
                if (lf && IsUnityObjectAlive(lf)) bones.leftFootTrans = lf;

                void *rf = GetBoneTransform(animator, Offsets::Bones::RightFoot);
                if (rf && IsUnityObjectAlive(rf)) bones.rightFootTrans = rf;
            }
        }

        // 2. Query BodyPointsManager from NetworkPlayer (hitboxes: 0=Head, 1=Chest, 2=Legs, matching BigHead)
        void *targetNet = isBot ? netPlayer : entityObj;
        if (isBot && (!targetNet || !Memory::IsValidPtr(targetNet))) {
            targetNet = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::NetPlayer);
        }
        if (targetNet && Memory::IsValidPtr(targetNet) && get_transform != nullptr) {
            void *bpm = Memory::ReadField<void*>(targetNet, Offsets::NetworkPlayer::BodyPointsManager);
            if (bpm && Memory::IsValidPtr(bpm)) {
                void *bpArr = Memory::ReadField<void*>(bpm, Offsets::BodyPoints::ArrayOffset);
                if (bpArr && Memory::IsValidPtr(bpArr)) {
                    uintptr_t length = Memory::ReadField<uintptr_t>(bpArr, Offsets::BodyPoints::ArrayLength);
                    if (length > 0 && length < 32) {
                        void **items = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bpArr) + Offsets::BodyPoints::ArrayItems);
                        for (uintptr_t i = 0; i < length; i++) {
                            void *bp = items[i];
                            if (bp && Memory::IsValidPtr(bp)) {
                                void *bpView = Memory::ReadField<void*>(bp, Offsets::BodyPoints::BPView);
                                if (bpView && Memory::IsValidPtr(bpView)) {
                                    int pType = Memory::ReadField<int>(bpView, Offsets::BodyPoints::BPType);
                                    void *t = get_transform(bpView);
                                    if (t && IsUnityObjectAlive(t)) {
                                        if (pType == Offsets::BodyPoints::Head && !bones.headTrans) {
                                            bones.headTrans = t;
                                        } else if (pType == Offsets::BodyPoints::Chest && !bones.chestTrans) {
                                            bones.chestTrans = t;
                                        } else if (pType == Offsets::BodyPoints::Legs && !bones.legsTrans) {
                                            bones.legsTrans = t;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // 3. Fallbacks for BotPlayer
        if (isBot) {
            // BotPlayerLook at 0x28 -> Head bone at 0x28 (as checked in BigHead)
            if (!bones.headTrans) {
                void *botLook = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::BotLook);
                if (botLook && Memory::IsValidPtr(botLook)) {
                    void *hBone = Memory::ReadField<void*>(botLook, 0x28);
                    if (hBone && IsUnityObjectAlive(hBone)) {
                        bones.headTrans = hBone;
                    }
                }
            }

            // TargetibleObject at 0x60
            void *tObj = Memory::ReadField<void*>(entityObj, Offsets::BotPlayer::TargetibleObject);
            if (tObj && Memory::IsValidPtr(tObj)) {
                if (!bones.headTrans) {
                    void *lookAt = Memory::ReadField<void*>(tObj, Offsets::Targetible::BotLookAtTransform);
                    if (lookAt && IsUnityObjectAlive(lookAt)) bones.headTrans = lookAt;
                }
                if (!bones.legsTrans && !bones.leftFootTrans && !bones.rightFootTrans) {
                    void *foot = Memory::ReadField<void*>(tObj, Offsets::Targetible::FootTransform);
                    if (foot && IsUnityObjectAlive(foot)) bones.legsTrans = foot;
                }
            }
        }

        return bones;
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

            // 1. Teammate & Enemy Filtering (Strictly synchronized with StatusPanel logic)
            bool isEnemy = false;
            if (item.team == 0 || item.team == 1) {
                isEnemy = StatusPanel::IsEnemy(item.team);
            } else {
                isEnemy = StatusPanel::IsEnemyEntity(entityObj, isBot, netPlayer);
            }

            // If enemy-only filter is ON, strictly ignore friendly teammates and local player
            if (enemyOnly && !isEnemy) {
                continue;
            }

            // 2. Resolve Head, Chest, Torso & Feet bone transforms
            EntityBones bones = ResolveEntityBones(entityObj, isBot, netPlayer);
            void *rootTrans = get_transform ? get_transform(entityObj) : nullptr;

            // 3. Obtain 3D Positions in World Space
            // Horizontal Center (X, Z) - strictly centered on the character's chest / torso!
            float centerX = 0.0f;
            float centerZ = 0.0f;
            bool hasCenter = false;

            if (bones.chestTrans && IsUnityObjectAlive(bones.chestTrans) && Transform_get_position != nullptr) {
                Vector3 cp = Transform_get_position(bones.chestTrans);
                if (cp.x != 0.0f || cp.y != 0.0f || cp.z != 0.0f) {
                    centerX = cp.x;
                    centerZ = cp.z;
                    hasCenter = true;
                }
            }

            if (!hasCenter && bones.headTrans && IsUnityObjectAlive(bones.headTrans) && Transform_get_position != nullptr) {
                Vector3 hp = Transform_get_position(bones.headTrans);
                if (hp.x != 0.0f || hp.y != 0.0f || hp.z != 0.0f) {
                    centerX = hp.x;
                    centerZ = hp.z;
                    hasCenter = true;
                }
            }

            if (!hasCenter && bones.hipsTrans && IsUnityObjectAlive(bones.hipsTrans) && Transform_get_position != nullptr) {
                Vector3 hip = Transform_get_position(bones.hipsTrans);
                if (hip.x != 0.0f || hip.y != 0.0f || hip.z != 0.0f) {
                    centerX = hip.x;
                    centerZ = hip.z;
                    hasCenter = true;
                }
            }

            if (!hasCenter) {
                if (!rootTrans || !IsUnityObjectAlive(rootTrans) || Transform_get_position == nullptr) continue;
                Vector3 rp = Transform_get_position(rootTrans);
                if (rp.x == 0.0f && rp.y == 0.0f && rp.z == 0.0f) continue;
                centerX = rp.x;
                centerZ = rp.z;
            }

            // Top vertical coordinate (Top of head / top of box)
            float topY = 0.0f;
            bool hasTop = false;

            if (bones.headTrans && IsUnityObjectAlive(bones.headTrans) && Transform_get_position != nullptr) {
                Vector3 hp = Transform_get_position(bones.headTrans);
                if (hp.y != 0.0f) {
                    float headExtra = BigHead::IsEnabled() ? 0.38f : 0.20f;
                    topY = hp.y + headExtra;
                    hasTop = true;
                }
            }

            if (!hasTop && bones.chestTrans && IsUnityObjectAlive(bones.chestTrans) && Transform_get_position != nullptr) {
                Vector3 cp = Transform_get_position(bones.chestTrans);
                topY = cp.y + 0.45f;
                hasTop = true;
            }

            if (!hasTop && bones.hipsTrans && IsUnityObjectAlive(bones.hipsTrans) && Transform_get_position != nullptr) {
                Vector3 hip = Transform_get_position(bones.hipsTrans);
                topY = hip.y + 0.90f;
                hasTop = true;
            }

            if (!hasTop && rootTrans && IsUnityObjectAlive(rootTrans) && Transform_get_position != nullptr) {
                Vector3 rp = Transform_get_position(rootTrans);
                topY = rp.y + 1.80f;
                hasTop = true;
            }

            if (!hasTop) continue;

            // Bottom vertical coordinate (Feet / Ground / bottom of box)
            float bottomY = 0.0f;
            bool hasBottom = false;

            if (bones.leftFootTrans && IsUnityObjectAlive(bones.leftFootTrans) && Transform_get_position != nullptr) {
                Vector3 lf = Transform_get_position(bones.leftFootTrans);
                float feetY = lf.y - 0.10f;
                if (bones.rightFootTrans && IsUnityObjectAlive(bones.rightFootTrans)) {
                    Vector3 rf = Transform_get_position(bones.rightFootTrans);
                    feetY = std::min(feetY, rf.y - 0.10f);
                }
                if (feetY < (topY - 0.6f) && feetY > (topY - 2.5f)) {
                    bottomY = feetY;
                    hasBottom = true;
                }
            } else if (bones.rightFootTrans && IsUnityObjectAlive(bones.rightFootTrans) && Transform_get_position != nullptr) {
                Vector3 rf = Transform_get_position(bones.rightFootTrans);
                float feetY = rf.y - 0.10f;
                if (feetY < (topY - 0.6f) && feetY > (topY - 2.5f)) {
                    bottomY = feetY;
                    hasBottom = true;
                }
            } else if (bones.legsTrans && IsUnityObjectAlive(bones.legsTrans) && Transform_get_position != nullptr) {
                Vector3 lg = Transform_get_position(bones.legsTrans);
                float feetY = lg.y - 0.40f;
                if (feetY < (topY - 0.6f) && feetY > (topY - 2.5f)) {
                    bottomY = feetY;
                    hasBottom = true;
                }
            }

            if (!hasBottom && bones.hipsTrans && IsUnityObjectAlive(bones.hipsTrans) && Transform_get_position != nullptr) {
                Vector3 hip = Transform_get_position(bones.hipsTrans);
                float headToHips = topY - hip.y;
                if (headToHips > 0.3f && headToHips < 1.2f) {
                    float candidateY = hip.y - (headToHips * 1.05f);
                    if (candidateY < (topY - 0.6f) && candidateY > (topY - 2.5f)) {
                        bottomY = candidateY;
                        hasBottom = true;
                    }
                }
            }

            if (!hasBottom && bones.chestTrans && IsUnityObjectAlive(bones.chestTrans) && Transform_get_position != nullptr) {
                Vector3 cp = Transform_get_position(bones.chestTrans);
                float headToChest = topY - cp.y;
                if (headToChest > 0.15f && headToChest < 0.70f) {
                    float candidateY = topY - (headToChest * 3.8f);
                    if (candidateY < (topY - 0.6f) && candidateY > (topY - 2.5f)) {
                        bottomY = candidateY;
                        hasBottom = true;
                    }
                }
            }

            if (!hasBottom && rootTrans && IsUnityObjectAlive(rootTrans) && Transform_get_position != nullptr) {
                Vector3 rp = Transform_get_position(rootTrans);
                if (rp.y < (topY - 0.6f) && rp.y > (topY - 2.5f)) {
                    bottomY = rp.y;
                    hasBottom = true;
                }
            }

            if (!hasBottom || bottomY >= (topY - 0.6f) || bottomY <= (topY - 2.5f)) {
                bottomY = topY - 1.75f;
            }

            // Both points lie directly on the central vertical axis passing through the chest!
            Vector3 headPos3D(centerX, topY, centerZ);
            Vector3 footPos3D(centerX, bottomY, centerZ);

            // 4. World to Screen projection using Unity Camera.WorldToScreenPoint
            if (!Camera_WorldToScreenPoint) continue;

            Vector3 sHead = Camera_WorldToScreenPoint(cameraObj, headPos3D);
            Vector3 sFoot = Camera_WorldToScreenPoint(cameraObj, footPos3D);

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

    static void SyncTeammatesToChams();

    void OnLocalPlayerUpdate(void *fpc) {
        if (!fpc || !IsUnityObjectAlive(fpc)) return;
        s_localPlayerFPC.store(fpc, std::memory_order_relaxed);
        StatusPanel::OnLocalPlayerUpdate(fpc);

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

        // Sync teammates to Chams so Chams can ignore allies even if ESP is OFF
        SyncTeammatesToChams();
    }

    static void SyncTeammatesToChams() {
        if (!Chams::IsEnabled() || !Chams::IsEnemyOnly()) return;

        static uint64_t s_lastChamsSync = 0;
        uint64_t now = GetCurrentTimeMs();
        if (now - s_lastChamsSync < 50) return; // Sync at ~20Hz
        s_lastChamsSync = now;

        std::vector<Chams::Vector3> teammates;

        void *localFpc = s_localPlayerFPC.load(std::memory_order_relaxed);
        if (localFpc && Memory::IsValidPtr(localFpc) && get_transform != nullptr && Transform_get_position != nullptr) {
            void *lpTrans = get_transform(localFpc);
            if (lpTrans && IsUnityObjectAlive(lpTrans)) {
                Vector3 lpos = Transform_get_position(lpTrans);
                teammates.push_back(Chams::Vector3(lpos.x, lpos.y, lpos.z));
            }
        }

        std::vector<StatusPanel::EntitySnapshot> entities = StatusPanel::GetActiveEntities();
        for (const auto &item : entities) {
            void *entityObj = item.object;
            if (!entityObj || !IsUnityObjectAlive(entityObj)) continue;
            bool isEnemy = false;
            if (item.team == 0 || item.team == 1) {
                isEnemy = StatusPanel::IsEnemy(item.team);
            } else {
                isEnemy = StatusPanel::IsEnemyEntity(entityObj, item.isBot, item.netPlayer);
            }

            if (!isEnemy) { // Confirmed Teammate / Friendly!
                void *trans = get_transform ? get_transform(entityObj) : nullptr;
                if (trans && IsUnityObjectAlive(trans) && Transform_get_position != nullptr) {
                    Vector3 pos = Transform_get_position(trans);
                    teammates.push_back(Chams::Vector3(pos.x, pos.y, pos.z));
                }
            }
        }

        Chams::UpdateTeammatePositions(teammates.data(), teammates.size());
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
