#include "FastFireRate.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>
#include <chrono>

namespace FastFireRate {

    struct ObscuredFloat {
        int hash;
        int hiddenValue;
        int currentCryptoKey;
        float fakeValue;
        uint32_t hiddenValueOldByte4;
    };

    static std::atomic<bool> s_enabled{false};

    // Client-side local player and weapon pointers
    static void *s_localPlayerFPC = nullptr;
    static void *s_localPlayerWeapon = nullptr;
    static void *s_localPlayerShooter = nullptr;
    static void *s_localWeaponProfile = nullptr;
    static void *s_localWeaponParams = nullptr;

    static int s_origShootAction = -1;
    static bool s_hasOrigShootAction = false;

    // Resolved function pointers from dump.cs
    static void* (*get_CurrentWeapon)(void *fpc) = nullptr;
    static void* (*get_ShooterBehaviour)(void *fpc, void *item) = nullptr;
    static void  (*set_ClipAmmo)(void *shooter, int ammo) = nullptr;
    static ObscuredFloat (*actk_op_Implicit_Float)(float val) = nullptr;

    static inline bool IsClientWeapon(void *instance) {
        if (!instance) return false;
        if (instance == s_localPlayerShooter || instance == s_localPlayerWeapon) return true;
        if (s_localPlayerFPC && Memory::IsValidPtr(s_localPlayerFPC)) {
            void *equipped = Memory::ReadField<void*>(s_localPlayerFPC, Offsets::FPC::CurrentWeapon);
            if (equipped == instance) return true;
        }
        return false;
    }

    static inline bool IsClientWeaponProfile(void *instance) {
        return (instance != nullptr && instance == s_localWeaponProfile);
    }

    static inline bool IsClientWeaponParams(void *instance) {
        return (instance != nullptr && instance == s_localWeaponParams);
    }

    void OnLocalPlayerUpdate(void *fpc) {
        if (!Memory::IsValidPtr(fpc)) return;
        s_localPlayerFPC = fpc;

        // Resolve current equipped weapon
        void *currentWeapon = nullptr;
        if (get_CurrentWeapon != nullptr) {
            currentWeapon = get_CurrentWeapon(fpc);
        }
        if (!currentWeapon) {
            currentWeapon = Memory::ReadField<void*>(fpc, Offsets::FPC::CurrentWeapon);
        }

        if (currentWeapon && Memory::IsValidPtr(currentWeapon)) {
            s_localPlayerWeapon = currentWeapon;

            if (get_ShooterBehaviour != nullptr) {
                s_localPlayerShooter = get_ShooterBehaviour(fpc, currentWeapon);
            }

            s_localWeaponProfile = Memory::ReadField<void*>(currentWeapon, Offsets::Weapon::ProfileOffset);
            s_localWeaponParams  = Memory::ReadField<void*>(currentWeapon, Offsets::Weapon::ParamsOffset);
        }

        // Apply weapon memory edits if feature is active
        if (s_enabled.load(std::memory_order_relaxed) && Memory::IsValidPtr(s_localPlayerShooter)) {
            // 1. Force Full-Auto mode (_shootAction = 0)
            int *shootActionPtr = reinterpret_cast<int*>(
                reinterpret_cast<uintptr_t>(s_localPlayerShooter) + Offsets::WeaponShooter::ShootAction);
            if (Memory::IsValidPtr(shootActionPtr)) {
                if (!s_hasOrigShootAction) {
                    s_origShootAction = *shootActionPtr;
                    s_hasOrigShootAction = true;
                }
                *shootActionPtr = 0; // AUTO
            }

            // 2. Keep clip ammo filled
            if (set_ClipAmmo != nullptr) {
                set_ClipAmmo(s_localPlayerShooter, 999);
            }
        } else if (s_hasOrigShootAction && Memory::IsValidPtr(s_localPlayerShooter)) {
            // Restore original fire mode safely
            int *shootActionPtr = reinterpret_cast<int*>(
                reinterpret_cast<uintptr_t>(s_localPlayerShooter) + Offsets::WeaponShooter::ShootAction);
            if (Memory::IsValidPtr(shootActionPtr)) {
                *shootActionPtr = s_origShootAction;
                s_hasOrigShootAction = false;
            }
        }
    }

    // =========================================================================
    // Hooks
    // =========================================================================

    // 1. WeaponShooterBehaviour.CanShoot (RVA: 0x405BBF0)
    static bool (*old_CanShoot)(void *instance) = nullptr;
    static bool hook_CanShoot(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed) && IsClientWeapon(instance)) {
            return true;
        }
        return old_CanShoot ? old_CanShoot(instance) : true;
    }

    // 2. WeaponShooterBehaviour.SetCooldown (RVA: 0x405D084)
    static void (*old_SetCooldown)(void *instance) = nullptr;
    static void hook_SetCooldown(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed) && IsClientWeapon(instance)) {
            return; // Zero cooldown for client weapon
        }
        if (old_SetCooldown) old_SetCooldown(instance);
    }

    // 3. FirstPersonController.GetShotInterval1 (RVA: 0x40906CC)
    static ObscuredFloat (*old_GetShotInterval1)(void *fpc, void *weapon) = nullptr;
    static ObscuredFloat hook_GetShotInterval1(void *fpc, void *weapon) {
        if (s_enabled.load(std::memory_order_relaxed) && (fpc == s_localPlayerFPC || IsClientWeapon(weapon))) {
            if (actk_op_Implicit_Float) return actk_op_Implicit_Float(0.001f);
        }
        if (old_GetShotInterval1) return old_GetShotInterval1(fpc, weapon);
        return actk_op_Implicit_Float ? actk_op_Implicit_Float(0.1f) : ObscuredFloat{};
    }

    // 4. FirstPersonController.GetShotInterval2 (RVA: 0x409F2F8)
    static ObscuredFloat (*old_GetShotInterval2)(void *fpc, void *weapon) = nullptr;
    static ObscuredFloat hook_GetShotInterval2(void *fpc, void *weapon) {
        if (s_enabled.load(std::memory_order_relaxed) && (fpc == s_localPlayerFPC || IsClientWeapon(weapon))) {
            if (actk_op_Implicit_Float) return actk_op_Implicit_Float(0.001f);
        }
        if (old_GetShotInterval2) return old_GetShotInterval2(fpc, weapon);
        return actk_op_Implicit_Float ? actk_op_Implicit_Float(0.1f) : ObscuredFloat{};
    }

    // 5. ShootCoroutine.MoveNext (RVA: 0x40B93A0)
    static bool (*old_ShootCoroutine_MoveNext)(void *instance) = nullptr;
    static bool hook_ShootCoroutine_MoveNext(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed) && Memory::IsValidPtr(instance)) {
            void *fpc = Memory::ReadField<void*>(instance, 0x20);
            if (fpc == s_localPlayerFPC && fpc != nullptr) {
                Memory::WriteField<bool>(instance, 0x28, true);
                Memory::WriteField<float>(instance, 0x2C, 100.0f);
            }
        }
        return old_ShootCoroutine_MoveNext ? old_ShootCoroutine_MoveNext(instance) : false;
    }

    // 6. WeaponParameters.GetFireRate (RVA: 0x49D0E90)
    static float (*old_WeaponParams_GetFireRate)(void *instance) = nullptr;
    static float hook_WeaponParams_GetFireRate(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed) && IsClientWeaponParams(instance)) {
            return 3000.0f; // High speed firerate for client weapon
        }
        return old_WeaponParams_GetFireRate ? old_WeaponParams_GetFireRate(instance) : 600.0f;
    }

    // 7. WeaponProfile.GetFireRate (RVA: 0x4A451C0)
    static float (*old_WeaponProfile_GetFireRate)(void *instance) = nullptr;
    static float hook_WeaponProfile_GetFireRate(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed) && IsClientWeaponProfile(instance)) {
            return 3000.0f;
        }
        return old_WeaponProfile_GetFireRate ? old_WeaponProfile_GetFireRate(instance) : 600.0f;
    }

    void Init() {
        get_CurrentWeapon      = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::FPC::GetCurrentWeapon));
        get_ShooterBehaviour   = reinterpret_cast<void*(*)(void*, void*)>(Memory::GetRVA(Offsets::FPC::GetShooterBehaviour));
        set_ClipAmmo           = reinterpret_cast<void(*)(void*, int)>(Memory::GetRVA(Offsets::WeaponShooter::SetClipAmmo));
        actk_op_Implicit_Float = reinterpret_cast<ObscuredFloat(*)(float)>(Memory::GetRVA(Offsets::ACTk::ObscuredFloat_Implicit));

        Memory::HookRVA(Offsets::WeaponShooter::CanShoot,        reinterpret_cast<void*>(hook_CanShoot), reinterpret_cast<void**>(&old_CanShoot));
        Memory::HookRVA(Offsets::WeaponShooter::SetCooldown,     reinterpret_cast<void*>(hook_SetCooldown), reinterpret_cast<void**>(&old_SetCooldown));
        Memory::HookRVA(Offsets::FPC::GetShotInterval1,         reinterpret_cast<void*>(hook_GetShotInterval1), reinterpret_cast<void**>(&old_GetShotInterval1));
        Memory::HookRVA(Offsets::FPC::GetShotInterval2,         reinterpret_cast<void*>(hook_GetShotInterval2), reinterpret_cast<void**>(&old_GetShotInterval2));
        Memory::HookRVA(Offsets::Weapon::ShootCoroutine_MoveNext,reinterpret_cast<void*>(hook_ShootCoroutine_MoveNext), reinterpret_cast<void**>(&old_ShootCoroutine_MoveNext));
        Memory::HookRVA(Offsets::Weapon::Params_GetFireRate,     reinterpret_cast<void*>(hook_WeaponParams_GetFireRate), reinterpret_cast<void**>(&old_WeaponParams_GetFireRate));
        Memory::HookRVA(Offsets::Weapon::Profile_GetFireRate,    reinterpret_cast<void*>(hook_WeaponProfile_GetFireRate), reinterpret_cast<void**>(&old_WeaponProfile_GetFireRate));

        LOG_INFO("[FIRERATE] Fast FireRate hooks installed successfully.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[FIRERATE] Fast FireRate toggle: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace FastFireRate
