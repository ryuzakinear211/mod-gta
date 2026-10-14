#include "NoRecoil.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>

namespace NoRecoil {

    struct Vector2 {
        float x;
        float y;
        Vector2(float _x = 0.0f, float _y = 0.0f) : x(_x), y(_y) {}
    };

    static std::atomic<bool> s_enabled{false};

    // 1. FirstPersonController.RecoilShift: RVA 0x40A4420
    static void (*old_FPC_Recoil)(void *fpc, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
    static void hook_FPC_Recoil(void *fpc, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            Vector2 zero(0.0f, 0.0f);
            if (old_FPC_Recoil) old_FPC_Recoil(fpc, zero, smooth, returnSpeed, resistance);
            return;
        }
        if (old_FPC_Recoil) old_FPC_Recoil(fpc, recoil, smooth, returnSpeed, resistance);
    }

    // 2. AimingControl.RecoilShift1: RVA 0x4D27AB8
    static void (*old_Aiming_Recoil1)(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
    static void hook_Aiming_Recoil1(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            Vector2 zero(0.0f, 0.0f);
            if (old_Aiming_Recoil1) old_Aiming_Recoil1(instance, zero, smooth, returnSpeed, resistance);
            return;
        }
        if (old_Aiming_Recoil1) old_Aiming_Recoil1(instance, recoil, smooth, returnSpeed, resistance);
    }

    // 3. AimingControl.RecoilShift2: RVA 0x4D27E20
    static void (*old_Aiming_Recoil2)(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
    static void hook_Aiming_Recoil2(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            Vector2 zero(0.0f, 0.0f);
            if (old_Aiming_Recoil2) old_Aiming_Recoil2(instance, zero, smooth, returnSpeed, resistance);
            return;
        }
        if (old_Aiming_Recoil2) old_Aiming_Recoil2(instance, recoil, smooth, returnSpeed, resistance);
    }

    // 4. AimingControl.RecoilShift3: RVA 0x4D284B0
    static void (*old_Aiming_Recoil3)(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
    static void hook_Aiming_Recoil3(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            Vector2 zero(0.0f, 0.0f);
            if (old_Aiming_Recoil3) old_Aiming_Recoil3(instance, zero, smooth, returnSpeed, resistance);
            return;
        }
        if (old_Aiming_Recoil3) old_Aiming_Recoil3(instance, recoil, smooth, returnSpeed, resistance);
    }

    void OnLocalPlayerUpdate(void *fpc) {
        if (!s_enabled.load(std::memory_order_relaxed) || !Memory::IsValidPtr(fpc)) return;

        // 1. AnimController at offset 0xB8
        void *animCtrl = Memory::ReadField<void*>(fpc, Offsets::FPC::AnimController);
        if (!Memory::IsValidPtr(animCtrl)) return;

        // 2. AnimEffectList at offset 0x30
        void *animList = Memory::ReadField<void*>(animCtrl, 0x30);
        if (!Memory::IsValidPtr(animList)) return;

        // 3. AttackAnimEffect at offset 0x38
        void *attackAnim = Memory::ReadField<void*>(animList, 0x38);
        if (!Memory::IsValidPtr(attackAnim)) return;

        // Zero current weapon/camera recoil vectors safely (0x1F0 to 0x22C)
        void *recoilVecStart = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(attackAnim) + 0x1F0);
        void *recoilVecEnd   = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(attackAnim) + 0x228);
        if (Memory::IsValidPtr(recoilVecStart) && Memory::IsValidPtr(recoilVecEnd)) {
            memset(recoilVecStart, 0, 0x22C - 0x1F0);
        }

        // Zero _sumShift (Vector2) at 0x244 & 0x248
        void *sumShiftPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(attackAnim) + 0x244);
        if (Memory::IsValidPtr(sumShiftPtr)) {
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(attackAnim) + 0x244) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(attackAnim) + 0x248) = 0.0f;
        }
    }

    void Init() {
        Memory::HookRVA(Offsets::FPC::RecoilShift,            reinterpret_cast<void*>(hook_FPC_Recoil),     reinterpret_cast<void**>(&old_FPC_Recoil));
        Memory::HookRVA(Offsets::AimingControl::RecoilShift1, reinterpret_cast<void*>(hook_Aiming_Recoil1), reinterpret_cast<void**>(&old_Aiming_Recoil1));
        Memory::HookRVA(Offsets::AimingControl::RecoilShift2, reinterpret_cast<void*>(hook_Aiming_Recoil2), reinterpret_cast<void**>(&old_Aiming_Recoil2));
        Memory::HookRVA(Offsets::AimingControl::RecoilShift3, reinterpret_cast<void*>(hook_Aiming_Recoil3), reinterpret_cast<void**>(&old_Aiming_Recoil3));

        LOG_INFO("[RECOIL] No Recoil hooks installed successfully.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[RECOIL] No Recoil toggle: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace NoRecoil
