#ifndef GAME_OFFSETS_H
#define GAME_OFFSETS_H

#include <cstdint>

/**
 * ======================================================================================
 * GTA San Andreas (Mobile / IL2CPP 64-bit) - Central Game Offsets & RVAs
 * ======================================================================================
 * File ini berisi seluruh alamat Relative Virtual Address (RVA) dan offset field struct
 * yang diekstrak langsung dari dump.cs.
 * 
 * CARA MENAMBAHKAN FITUR BARU:
 * 1. Buka dump.cs menggunakan text editor / pencari string.
 * 2. Cari Class atau Method yang diinginkan (contoh: "PlayerHealth", "AddAmmo", "SetSpeed").
 * 3. Salin nilai "// RVA: 0xXXXXXXX" atau "// 0xXX" (Field offset).
 * 4. Tambahkan ke namespace yang sesuai di bawah ini.
 * 5. Gunakan helper Memory::GetRVA() atau Memory::ReadField() / Memory::WriteField()
 *    pada modul fitur baru di folder Features/.
 * ======================================================================================
 */

namespace Offsets {

    // Target binary library name
    namespace TargetLib {
        constexpr const char *Name = "libil2cpp.so";
    }

    // ----------------------------------------------------------------------------------
    // Unity Engine Core Native Functions (dump.cs / UnityEngine.CoreModule)
    // ----------------------------------------------------------------------------------
    namespace Unity {
        // Component.get_transform() -> Transform
        // dump.cs RVA: 0x8597C20
        constexpr uintptr_t GetTransform = 0x8597C20;

        // Transform.set_localScale_Injected(Transform, ref Vector3)
        // dump.cs RVA: 0x85B224C
        constexpr uintptr_t SetLocalScaleInjected = 0x85B224C;

        // Animator.GetBoneTransform(HumanBodyBones humanBoneId) -> Transform
        // dump.cs RVA: 0x84EBAB0 (Head bone ID = 10)
        constexpr uintptr_t GetBoneTransform = 0x84EBAB0;

        // Transform cached C++ pointer offset in managed Component/Transform
        constexpr uintptr_t Transform_m_CachedPtr = 0x10;
    }

    // ----------------------------------------------------------------------------------
    // FirstPersonController (Local Player Character Controller)
    // ----------------------------------------------------------------------------------
    namespace FPC {
        // Methods (RVAs)
        constexpr uintptr_t Update                  = 0x40A1BF4; // Update()
        constexpr uintptr_t GetCurrentWeapon        = 0x408FC44; // GetCurrentWeapon() -> WeaponShooterBehaviour
        constexpr uintptr_t GetShooterBehaviour     = 0x409188C; // GetShooterBehaviour(item) -> WeaponShooterBehaviour
        constexpr uintptr_t GetShotInterval1        = 0x40906CC; // GetShotInterval1() -> ObscuredFloat
        constexpr uintptr_t GetShotInterval2        = 0x409F2F8; // GetShotInterval2() -> ObscuredFloat
        constexpr uintptr_t RecoilShift             = 0x40A4420; // RecoilShift(Vector2, float, float, float)

        // Headshot override patch points in FirstPersonController
        constexpr uintptr_t HitBodyPartStore        = 0x40961B4; // Store calculated bodyPart
        constexpr uintptr_t DamageDataBodyPart      = 0x409657C; // DamageData.ctor bodyPart argument
        constexpr uintptr_t ReportHitBodyPart       = 0x4096690; // Network report bodyPart argument
        constexpr uintptr_t HeadshotMultiplierBranch= 0x40968B8; // Damage multiplier branch

        // Fields (Offsets)
        constexpr uintptr_t CurrentWeapon           = 0x50;      // Equipped Weapon pointer
        constexpr uintptr_t AnimController          = 0xB8;      // Animation Controller pointer
    }

    // ----------------------------------------------------------------------------------
    // WeaponShooterBehaviour (Weapon Mechanics, Cooldown & Ammo)
    // ----------------------------------------------------------------------------------
    namespace WeaponShooter {
        // Methods (RVAs)
        constexpr uintptr_t CanShoot                = 0x405BBF0; // CanShoot() -> bool
        constexpr uintptr_t SetCooldown             = 0x405D084; // SetCooldown() -> void
        constexpr uintptr_t SetClipAmmo             = 0x405A39C; // SetClipAmmo(int) -> void

        // Fields (Offsets)
        constexpr uintptr_t ShootAction             = 0x150;     // 0 = Auto, 1 = Bolt Action
        constexpr uintptr_t Cooldown1               = 0x18C;     // Cooldown ObscuredFloat 1
        constexpr uintptr_t Cooldown2               = 0x1B0;     // Cooldown ObscuredFloat 2
    }

    // ----------------------------------------------------------------------------------
    // Weapon Parameters & Profiles
    // ----------------------------------------------------------------------------------
    namespace Weapon {
        // Methods (RVAs)
        constexpr uintptr_t Params_GetFireRate      = 0x49D0E90; // WeaponParameters.GetFireRate() -> float
        constexpr uintptr_t Profile_GetFireRate     = 0x4A451C0; // WeaponProfile.GetFireRate() -> float
        constexpr uintptr_t ShootCoroutine_MoveNext = 0x40B93A0; // ShootCoroutine.MoveNext() -> bool

        // Fields on ItemBehaviour / Weapon (Offsets)
        constexpr uintptr_t ProfileOffset           = 0x80;      // WeaponProfile pointer
        constexpr uintptr_t ParamsOffset            = 0x88;      // WeaponParameters pointer
    }

    // ----------------------------------------------------------------------------------
    // AimingControl (Camera Recoil & Kick)
    // ----------------------------------------------------------------------------------
    namespace AimingControl {
        constexpr uintptr_t RecoilShift1            = 0x4D27AB8; // RecoilShift1()
        constexpr uintptr_t RecoilShift2            = 0x4D27E20; // RecoilShift2()
        constexpr uintptr_t RecoilShift3            = 0x4D284B0; // RecoilShift3()
    }

    // ----------------------------------------------------------------------------------
    // NetworkPlayer (Remote & Local Player Network Entity)
    // ----------------------------------------------------------------------------------
    namespace NetworkPlayer {
        // Methods (RVAs)
        constexpr uintptr_t Update                  = 0x42CC8C8; // NetworkPlayer.Update()
        constexpr uintptr_t OnDestroy               = 0x42C9540; // NetworkPlayer.OnDestroy()
        constexpr uintptr_t Damage                  = 0x42CF248; // NetworkPlayer.Damage() patch point

        // Fields (Offsets)
        constexpr uintptr_t DollsManager            = 0x88;      // DollsManager pointer
        constexpr uintptr_t BodyPointsManager       = 0xC8;      // BodyPointsManager pointer
    }

    // ----------------------------------------------------------------------------------
    // BotPlayer (AI Bot Entity)
    // ----------------------------------------------------------------------------------
    namespace BotPlayer {
        // Methods (RVAs)
        constexpr uintptr_t Start                   = 0x44493F4; // BotPlayer.Start()
        constexpr uintptr_t Update                  = 0x444A310; // BotPlayer.Update()
        constexpr uintptr_t OnDestroy               = 0x4449EB0; // BotPlayer.OnDestroy()

        // Fields (Offsets)
        constexpr uintptr_t BotLook                 = 0x28;      // BotPlayerLook pointer (Head Look Bone at +0x28)
        constexpr uintptr_t NetPlayer               = 0x50;      // NetworkPlayer reference
        constexpr uintptr_t ThirdPersonCtrl         = 0x58;      // ThirdPersonController pointer
    }

    // ----------------------------------------------------------------------------------
    // DollsManager & ThirdPersonController (Mesh / Bone Hierarchy)
    // ----------------------------------------------------------------------------------
    namespace Dolls {
        constexpr uintptr_t FirstPersonDollView     = 0x38;      // FP Doll Transform
        constexpr uintptr_t ThirdPersonDollView     = 0x48;      // TP Doll Transform
        constexpr uintptr_t RemoteTPCtrl            = 0x50;      // Remote player ThirdPersonController
        constexpr uintptr_t BotTPCtrl               = 0x60;      // Bot player ThirdPersonController
        constexpr uintptr_t AnimatorOffset          = 0x78;      // Animator in ThirdPersonController
    }

    // ----------------------------------------------------------------------------------
    // Damage & BodyPoints (Hitbox and Body Part Resolution)
    // ----------------------------------------------------------------------------------
    namespace Damage {
        constexpr uintptr_t VehicleWeapon_DamageData= 0x4293794; // VehicleWeaponBehaviour.DamageData
        constexpr uintptr_t DamageData_get_BodyPart = 0x4FC8668; // DamageData.get_BodyPart() -> int
        constexpr uintptr_t DamageData_ctor         = 0x4FC8630; // DamageData..ctor()
        constexpr uintptr_t BodyPoint_get_Types     = 0x3F2FB54; // BodyPoint.get_BodyPointTypes() -> int
    }

    // ----------------------------------------------------------------------------------
    // Anti-Cheat Toolkit (ACTk) Obscured Types
    // ----------------------------------------------------------------------------------
    namespace ACTk {
        constexpr uintptr_t ObscuredFloat_Implicit  = 0x3D6E4DC; // ObscuredFloat.op_Implicit(float)
    }

} // namespace Offsets

#endif // GAME_OFFSETS_H
