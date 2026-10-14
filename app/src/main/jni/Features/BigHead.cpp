#include "BigHead.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>

namespace BigHead {

    struct Vector3 {
        float x, y, z;
        Vector3(float _x = 0.0f, float _y = 0.0f, float _z = 0.0f) : x(_x), y(_y), z(_z) {}
    };

    static const Vector3 BIG_HEAD_SCALE(3.0f, 3.0f, 3.0f);
    static const Vector3 NORMAL_HEAD_SCALE(1.0f, 1.0f, 1.0f);

    static std::atomic<bool> s_enabled{false};
    static std::atomic<bool> s_needsReset{false};
    static std::atomic<int>  s_resetFrames{0};

    // Unity native pointers
    static void *(*get_transform)(void *) = nullptr;
    static void  (*set_localScale_Injected)(void *, const Vector3 *) = nullptr;
    static void *(*GetBoneTransform)(void *, int) = nullptr;

    static void SafeSetLocalScale(void *transformObj, const Vector3 &scale) {
        if (!Memory::IsValidPtr(transformObj) || set_localScale_Injected == nullptr) return;

        // In Unity, managed Transform has native C++ pointer at offset 0x10 (m_CachedPtr)
        void *nativePtr = Memory::ReadField<void*>(transformObj, Offsets::Unity::Transform_m_CachedPtr);
        if (Memory::IsValidPtr(nativePtr)) {
            set_localScale_Injected(nativePtr, &scale);
        }
    }

    static void ApplyScaleToNetPlayer(void *netPlayer, const Vector3 &scale) {
        if (!Memory::IsValidPtr(netPlayer)) return;

        // 1. DollsManager at offset 0x88
        void *dollsMgr = Memory::ReadField<void*>(netPlayer, Offsets::NetworkPlayer::DollsManager);
        if (Memory::IsValidPtr(dollsMgr)) {
            // Remote player: ThirdPersonController at 0x50
            void *tpCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::RemoteTPCtrl);
            if (Memory::IsValidPtr(tpCtrl) && GetBoneTransform != nullptr) {
                void *animator = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::AnimatorOffset);
                if (Memory::IsValidPtr(animator)) {
                    void *headBone = GetBoneTransform(animator, 10); // 10 = HumanBodyBones.Head
                    SafeSetLocalScale(headBone, scale);
                }
            }

            // Bot doll: ThirdPersonController at 0x60
            void *tpBotCtrl = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::BotTPCtrl);
            if (Memory::IsValidPtr(tpBotCtrl) && GetBoneTransform != nullptr) {
                void *animator = Memory::ReadField<void*>(tpBotCtrl, Offsets::Dolls::AnimatorOffset);
                if (Memory::IsValidPtr(animator)) {
                    void *headBone = GetBoneTransform(animator, 10);
                    SafeSetLocalScale(headBone, scale);
                }
            }

            // Doll views
            if (get_transform != nullptr) {
                void *fpDoll = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::FirstPersonDollView);
                if (Memory::IsValidPtr(fpDoll)) {
                    SafeSetLocalScale(get_transform(fpDoll), scale);
                }
                void *tpDoll = Memory::ReadField<void*>(dollsMgr, Offsets::Dolls::ThirdPersonDollView);
                if (Memory::IsValidPtr(tpDoll)) {
                    SafeSetLocalScale(get_transform(tpDoll), scale);
                }
            }
        }

        // 2. Head Hitbox in BodyPointsManager at offset 0xC8
        void *bpm = Memory::ReadField<void*>(netPlayer, Offsets::NetworkPlayer::BodyPointsManager);
        if (Memory::IsValidPtr(bpm) && get_transform != nullptr) {
            void *bodyPointsArr = Memory::ReadField<void*>(bpm, 0x20);
            if (Memory::IsValidPtr(bodyPointsArr)) {
                uintptr_t length = Memory::ReadField<uintptr_t>(bodyPointsArr, 0x18);
                if (length > 0 && length < 32) {
                    void **items = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bodyPointsArr) + 0x20);
                    for (uintptr_t i = 0; i < length; i++) {
                        void *bodyPoint = items[i];
                        if (Memory::IsValidPtr(bodyPoint)) {
                            void *bpView = Memory::ReadField<void*>(bodyPoint, 0x10);
                            if (Memory::IsValidPtr(bpView)) {
                                int pointType = Memory::ReadField<int>(bpView, 0x20);
                                if (pointType == 0) { // Head hitbox
                                    SafeSetLocalScale(get_transform(bpView), scale);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    static void ApplyScaleToBotPlayer(void *botPlayer, const Vector3 &scale) {
        if (!Memory::IsValidPtr(botPlayer)) return;

        // 1. ThirdPersonController at offset 0x58
        void *tpCtrl = Memory::ReadField<void*>(botPlayer, Offsets::BotPlayer::ThirdPersonCtrl);
        if (Memory::IsValidPtr(tpCtrl) && GetBoneTransform != nullptr) {
            void *animator = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::AnimatorOffset);
            if (Memory::IsValidPtr(animator)) {
                void *headBone = GetBoneTransform(animator, 10);
                SafeSetLocalScale(headBone, scale);
            }
        }

        // 2. BotPlayerLook at offset 0x28 -> Head bone at 0x28
        void *botLook = Memory::ReadField<void*>(botPlayer, Offsets::BotPlayer::BotLook);
        if (Memory::IsValidPtr(botLook)) {
            void *headBone = Memory::ReadField<void*>(botLook, 0x28);
            SafeSetLocalScale(headBone, scale);
        }

        // 3. NetworkPlayer at offset 0x50
        void *netPlayer = Memory::ReadField<void*>(botPlayer, Offsets::BotPlayer::NetPlayer);
        if (Memory::IsValidPtr(netPlayer)) {
            ApplyScaleToNetPlayer(netPlayer, scale);
        }
    }

    void OnNetworkPlayerUpdate(void *netPlayer) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            ApplyScaleToNetPlayer(netPlayer, BIG_HEAD_SCALE);
        } else if (s_needsReset.load(std::memory_order_relaxed)) {
            ApplyScaleToNetPlayer(netPlayer, NORMAL_HEAD_SCALE);
            int remaining = s_resetFrames.fetch_sub(1);
            if (remaining <= 1) {
                s_needsReset.store(false, std::memory_order_relaxed);
            }
        }
    }

    void OnBotPlayerUpdate(void *botPlayer) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            ApplyScaleToBotPlayer(botPlayer, BIG_HEAD_SCALE);
        } else if (s_needsReset.load(std::memory_order_relaxed)) {
            ApplyScaleToBotPlayer(botPlayer, NORMAL_HEAD_SCALE);
        }
    }

    void Init() {
        get_transform           = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::Unity::GetTransform));
        set_localScale_Injected = reinterpret_cast<void(*)(void*, const Vector3*)>(Memory::GetRVA(Offsets::Unity::SetLocalScaleInjected));
        GetBoneTransform        = reinterpret_cast<void*(*)(void*, int)>(Memory::GetRVA(Offsets::Unity::GetBoneTransform));

        LOG_INFO("[BIG_HEAD] Unity pointers resolved: get_transform=%p, set_localScale=%p, GetBoneTransform=%p",
                 get_transform, set_localScale_Injected, GetBoneTransform);
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        if (!enable) {
            s_needsReset.store(true, std::memory_order_relaxed);
            s_resetFrames.store(60, std::memory_order_relaxed); // Restore scale over 60 frames
        }
        LOG_INFO("[BIG_HEAD] Big Head toggle: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace BigHead
