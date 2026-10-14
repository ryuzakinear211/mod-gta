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
