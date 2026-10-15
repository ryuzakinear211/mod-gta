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

        // Application.set_targetFrameRate(int)
        // dump.cs RVA: 0x85137C0
        constexpr uintptr_t SetTargetFrameRate = 0x85137C0;

        // Application.get_targetFrameRate() -> int
        // dump.cs RVA: 0x851378C
        constexpr uintptr_t GetTargetFrameRate = 0x851378C;

        // QualitySettings.set_vSyncCount(int)
        // dump.cs RVA: 0x8536E1C
        constexpr uintptr_t SetVSyncCount = 0x8536E1C;

        // Transform.get_position() -> Vector3
        // dump.cs RVA: 0x85B12FC
        constexpr uintptr_t TransformGetPosition = 0x85B12FC;

        // Camera.get_main() -> Camera
        // dump.cs RVA: 0x851ED38
        constexpr uintptr_t CameraGetMain = 0x851ED38;

        // Camera.WorldToScreenPoint(Vector3 position) -> Vector3
        // dump.cs RVA: 0x851E1B0
        constexpr uintptr_t CameraWorldToScreenPoint = 0x851E1B0;

        // Behaviour.get_isActiveAndEnabled() -> bool
        // dump.cs RVA: 0x85974E0
        constexpr uintptr_t BehaviourGetIsActive = 0x85974E0;

        // GameObject.SetActive(bool)
        // dump.cs RVA: 0x859D140
        constexpr uintptr_t GameObject_SetActive = 0x859D140;

        // Component.get_gameObject() -> GameObject
        // dump.cs RVA: 0x8597D00
        constexpr uintptr_t Component_GetGameObject = 0x8597D00;

        // Selectable.set_interactable(bool)
        // dump.cs RVA: 0x88B7998
        constexpr uintptr_t Selectable_SetInteractable = 0x88B7998;
    }

    // ----------------------------------------------------------------------------------
    // Humanoid Bone Hierarchy (Unity HumanBodyBones enum for Animator.GetBoneTransform)
    // ----------------------------------------------------------------------------------
    namespace Bones {
        enum HumanBodyBones {
            Hips          = 0,
            LeftUpperLeg  = 1,
            RightUpperLeg = 2,
            LeftLowerLeg  = 3,
            RightLowerLeg = 4,
            LeftFoot      = 5,
            RightFoot     = 6,
            Spine         = 7,
            Chest         = 8,
            Neck          = 9,
            Head          = 10,
            LeftShoulder  = 11,
            RightShoulder = 12,
            UpperChest    = 54
        };
    }

    // ----------------------------------------------------------------------------------
    // BodyPoints (BodyPointsManager Hitboxes from dump.cs: Head=0, Chest=1, Legs=2)
    // ----------------------------------------------------------------------------------
    namespace BodyPoints {
        enum PointType {
            Head  = 0,
            Chest = 1,
            Legs  = 2
        };
        constexpr uintptr_t ArrayOffset = 0x20; // BodyPointsManager._bodyPoints array
        constexpr uintptr_t ArrayLength = 0x18; // Il2Cpp array length
        constexpr uintptr_t ArrayItems  = 0x20; // Il2Cpp array elements start
        constexpr uintptr_t BPView      = 0x10; // BodyPoint.bpView
        constexpr uintptr_t BPType      = 0x20; // BodyPointView.BodyPointType
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
        constexpr uintptr_t AimingControl           = 0xF0;      // AimingControl pointer
        constexpr uintptr_t TargetibleObject        = 0x120;     // Local TargetibleObject pointer
        constexpr uintptr_t FPSCamera               = 0x180;     // Camera pointer
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
        constexpr uintptr_t Update                  = 0x4D27294; // AimingControl.Update()
        constexpr uintptr_t RecoilShift1            = 0x4D27AB8; // RecoilShift1()
        constexpr uintptr_t RecoilShift2            = 0x4D27E20; // RecoilShift2()
        constexpr uintptr_t RecoilShift3            = 0x4D284B0; // RecoilShift3()

        // Fields (Offsets)
        constexpr uintptr_t Camera                  = 0x20;      // Camera pointer
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
        constexpr uintptr_t TargetInfo              = 0xC0;      // TargetInfo pointer
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
        constexpr uintptr_t GetTeam                 = 0x4449610; // BotPlayer.GetTeam() -> int (0 = TeamA, 1 = TeamB)

        // Fields (Offsets)
        constexpr uintptr_t BotLook                 = 0x28;      // BotPlayerLook pointer (Head Look Bone at +0x28)
        constexpr uintptr_t BotHealth               = 0x30;      // BotPlayerHealth pointer (Health at +0x2C, MaxHealth at +0x30)
        constexpr uintptr_t NetPlayer               = 0x50;      // NetworkPlayer reference
        constexpr uintptr_t ThirdPersonCtrl         = 0x58;      // ThirdPersonController pointer
        constexpr uintptr_t TargetibleObject        = 0x60;      // TargetibleObject pointer
    }

    // ----------------------------------------------------------------------------------
    // TargetibleObject & CustomSettings (Team Affiliation & Target Types)
    // ----------------------------------------------------------------------------------
    namespace Targetible {
        constexpr uintptr_t TargetPlace             = 0x20;      // Vector3 TargetPlace
        constexpr uintptr_t BotLookAtTransform      = 0x38;      // Transform pointer (Head Look Bone)
        constexpr uintptr_t HeadCollider            = 0x40;      // Collider pointer
        constexpr uintptr_t BodyCollider            = 0x48;      // Collider pointer
        constexpr uintptr_t FootTransform           = 0x80;      // Transform pointer (Feet/Ground)
        constexpr uintptr_t CustomSettings          = 0x90;      // TargetibleObjectCustomSettings pointer
    }

    namespace CustomSettings {
        // Methods (RVAs)
        constexpr uintptr_t GetIsAutoAimAllowed     = 0x4DED124; // TargetibleObjectCustomSettings.get_IsAutoAimAllowed() -> bool
        constexpr uintptr_t GetAllyObjectToggle     = 0x4DED164; // TargetibleObjectCustomSettings.get_AllyObjectToogle() -> bool

        // Fields (Offsets)
        constexpr uintptr_t IsAllyToggle            = 0x20;      // bool AllyObjectToogle
        constexpr uintptr_t EnemyData               = 0x28;      // TargetibleObjectCustomSettingsData*
        constexpr uintptr_t AllyData                = 0x30;      // TargetibleObjectCustomSettingsData*
        constexpr uintptr_t IsAutoAimAllowed        = 0x11;      // bool on TargetibleObjectCustomSettingsData
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

    // ----------------------------------------------------------------------------------
    // Reward & Currency Management (Claim Reward Booster)
    // ----------------------------------------------------------------------------------
    namespace Reward {
        // Enums from dump.cs (LastLevel.Legion.Dom.Currency)
        namespace CurrencyType {
            constexpr int None  = 0;
            constexpr int Soft  = 1; // Credits / Money
            constexpr int Hard  = 2; // Gold / Gems
        }

        // RewardType enum from dump.cs (TypeDefIndex: 11421)
        namespace RewardType {
            constexpr int None            = -1;
            constexpr int ArmorPlate      = 4;
            constexpr int Grenade         = 5;
            constexpr int UserWeapon      = 6;
            constexpr int ItemEquip       = 7;
            constexpr int Experience      = 8;
            constexpr int Chest           = 9;
            constexpr int Gadget          = 14;
            constexpr int Camouflage      = 15;
            constexpr int MultiplyReward  = 16;
            constexpr int DailyChestReset = 17;
            constexpr int BattlePass      = 18;
            constexpr int Blueprint       = 19;
            constexpr int Miner           = 20;
            constexpr int ClanPoints      = 21;
            constexpr int GameMoney       = 22; // Currency (Soft/Hard)
            constexpr int ClanExperience  = 23;
            constexpr int DivisionPoints  = 24;
            constexpr int CharacterExp    = 25;
            constexpr int MasterPoints    = 26;
            constexpr int Character       = 27;
            constexpr int Skill           = 28;
            constexpr int PlusAccess      = 29;
            constexpr int RatingStagePoints = 30;
            constexpr int QuestPoints     = 999;
        }

        // Domain Model: SerializableUserReward (dump.cs class: LastLevel.Legion.Dom.SerializableUserReward)
        // Fields (Offsets)
        constexpr uintptr_t SerialReward_Type      = 0x10; // RewardType enum
        constexpr uintptr_t SerialReward_Value     = 0x14; // Currency enum ID
        constexpr uintptr_t SerialReward_Count     = 0x18; // Amount int
        // Methods (RVAs)
        constexpr uintptr_t SerialReward_ctor      = 0x4B5447C; // SerializableUserReward..ctor(type, value, count, comp, meta)

        // Domain Model: SerializableUserRewardList (dump.cs class: LastLevel.Legion.Dom.SerializableUserRewardList)
        constexpr uintptr_t SerialList_AddReward   = 0x45262A0; // SerializableUserRewardList.Add(reward)
        constexpr uintptr_t SerialList_GetList1    = 0x4525494; // -> List<RewardItem>
        constexpr uintptr_t SerialList_GetList2    = 0x45255C0; // -> List<RewardItem>
        constexpr uintptr_t SerialList_GetList3    = 0x45257F8; // -> List<RewardItem>
        constexpr uintptr_t SerialList_GetList4    = 0x4525918; // -> List<RewardItem>
        constexpr uintptr_t SerialList_GetList5    = 0x4525A38; // -> List<RewardItem>
        constexpr uintptr_t SerialList_GetList6    = 0x4525C88; // -> List<RewardItem>

        // Reward Item (dump.cs class: abbbbabbbaaabababbbbbbbbaabbbbaababababaababbbb)
        // Fields (Offsets)
        constexpr uintptr_t RewardItem_Type        = 0x10; // RewardType enum
        constexpr uintptr_t RewardItem_CurrencyId  = 0x14; // Currency enum ID
        constexpr uintptr_t RewardItem_Amount      = 0x18; // Amount int

        // Methods (RVAs)
        constexpr uintptr_t RewardItem_ctor        = 0x497D240; // RewardItem..ctor(type, id, amount, p3, p4)
        constexpr uintptr_t RewardItem_GetAmount   = 0x497D228; // RewardItem.GetAmount() -> int
        constexpr uintptr_t RewardItem_SetAmount   = 0x497D098; // RewardItem.SetAmount(int) -> void

        // Reward Popup Data (dump.cs class: baabbabbbabaabaaaaaabbbabbbbbbbbbbbbabaaabbaaaa)
        constexpr uintptr_t PopupData_ctor         = 0x41D8E50; // .ctor(p1, List<RewardItem>, p3)

        // UI View Setup (dump.cs: BaseRewardElementView.Setup)
        constexpr uintptr_t RewardView_Setup       = 0x49F9E48; // BaseRewardElementView.Setup(...)
    }

    // ----------------------------------------------------------------------------------
    // Push Notification Permission & One-Time Reward Re-Claim
    // ----------------------------------------------------------------------------------
    namespace Notification {
        // SettingKey enum from dump.cs (TypeDefIndex: 7618)
        namespace SettingKey {
            constexpr int PushNotificationReward     = 23;
            constexpr int PushNotificationTimechest  = 24;
            constexpr int PushNotificationSocial     = 25;
        }

        // Settings Provider (dump.cs class: abaaabbaaabababaabbbaaabababaaabaababbabbbaaaab)
        constexpr uintptr_t Settings_GetSetting      = 0x41C8788; // aaaaabaaabbbbababbaabbabaaabbaabbaaabaabbaaaabb(SettingKey) -> Setting<T>
        constexpr uintptr_t Settings_GetInstance     = 0x41C8830; // bbbaabaaaabababbbaabbaabbaaabbbaababbbbaaabbbaa() -> SettingsProvider

        // Generic Reactive Setting (dump.cs class: aaaaaaabbbbabaaaabbabbbaabbbbaaaaabaabbabbaaaba<bool>)
        constexpr uintptr_t SettingBool_GetValue     = 0x6EF91D0; // aaaaaaabbbbabaaaabbabbbaabbbbaaaaabaabbabbaaaba<bool>.get_Value() -> bool
        constexpr uintptr_t SettingBool_SetValue     = 0x6EF9410; // aaaaaaabbbbabaaaabbabbbaabbbbaaaaabaabbabbaaaba<bool>.bbbbbabbbabbaababaaababaabbbaababbababbababbaba(bool)
        constexpr uintptr_t SettingBool_Save         = 0x6EF9214; // aaaaaaabbbbabaaaabbabbbaabbbbaaaaabaabbabbaaaba<bool>.babbaaaaaaaabbaaababbaababbbbaaaabaaaababababbb()

        // Permission Controller (dump.cs class: baaabaaabaabbabbbabbaabaaabaabbbbaaaabbaabbabbb)
        constexpr uintptr_t PermissionController_ctor       = 0x3F8A84C; // .ctor(PermissionRequestScreen)
        constexpr uintptr_t PermissionController_Check      = 0x3F89344; // abbbaabaababaabbaabbaabaabbaaabaaabaabaaaaaaaaa() -> bool
        constexpr uintptr_t PermissionController_Show       = 0x3F8B5E0; // aabbabbbababbaaababaabaabbbbbabbabaaaaabbaaaabb() -> void
        constexpr uintptr_t PermissionController_OnGranted1 = 0x3F89DC8; // bbabbbaabaabbababbabaabaabbbaabbaaabaaaaabbabba(bool)
        constexpr uintptr_t PermissionController_OnGranted2 = 0x3F8A914; // bbaabbaaaaabaaabbbaaababbbabbbbaaabababaaaaabbb(bool)
        constexpr uintptr_t PermissionController_OnResult   = 0x3F8A73C; // baaabbaaaabaabbaaaababbabbbbabababababaaaaaabab(bool)

        // Permission Request Screen (dump.cs class: PermissionRequestScreen)
        constexpr uintptr_t PermissionScreen_ctor           = 0x445E34C; // .ctor()
        constexpr uintptr_t PermissionScreen_OnClickYes     = 0x445E3E0; // OnClickYes()
        constexpr uintptr_t PermissionScreen_OnClickNo      = 0x445E404; // OnClickNo()
        constexpr uintptr_t PermissionScreen_SetState       = 0x445E290; // babbbbabbabbabbabbaaabaabaabaabbabbaaaaabbbabbb(bool, bool)

        // WindowsUi (dump.cs class: UI.Windows.WindowsUi, TypeDefIndex: 6087)
        constexpr uintptr_t WindowsUi_GetInstance           = 0x3FECAA0; // bbbabbabababaabaaabbbbbbababbbbbbaabbbaabbaabaa() -> WindowsUi
        constexpr uintptr_t WindowsUi_ListOffset            = 0x30;      // List<BaseWindowUi>
        constexpr uintptr_t WindowsUi_DictOffset            = 0x28;      // Dictionary<Type, BaseWindowUi>

        // PushPopUpWindowUi (dump.cs: UI.Windows.Push.PushPopUpWindowUi, TypeDefIndex: 2761)
        constexpr uintptr_t PushPopUpWindowUi_ctor          = 0x4E07DB0; // .ctor()
        constexpr uintptr_t PushPopUpWindowUi_Awake         = 0x4E073E4; // Slot 11: OnAwake()
        constexpr uintptr_t PushPopUpWindowUi_ShowPopup     = 0x4E07D2C; // baaaaaaaabbbaabababbababbaabbbabbaaababaaaaabaa() -> void
        constexpr uintptr_t PushPopUpWindowUi_OnShow        = 0x4E08A68; // Slot 13: OnShow(int transitionType) -> void
        constexpr uintptr_t PushPopUpWindowUi_AcceptEnable  = 0x4E080A0; // AcceptEnablePush() -> void
        constexpr uintptr_t PushPopUpWindowUi_ClosePopUp    = 0x4E08B24; // ClosePopUp() -> void
        constexpr uintptr_t PushPopUpWindowUi_OnGranted1    = 0x4E083E4; // bbabaabababbbaabbbbaaaaaabbbabbbbbbbabbbabababb(bool)
        constexpr uintptr_t PushPopUpWindowUi_OnGranted2    = 0x4E08680; // babbbbabbbabaaaaaabababbbbbbbbababaaabbaaabaaab(bool)

        // PushPopUpWindowUi Component Fields
        constexpr uintptr_t PushPopUp_FirstPopUp            = 0x28;      // GameObject _firstPopUp (dialog with female soldier & gold)
        constexpr uintptr_t PushPopUp_AnyPopUp              = 0x30;      // GameObject _anyPopUp
        constexpr uintptr_t PushPopUp_Reward                = 0x38;      // QuestRewardUiElement _reward
        constexpr uintptr_t PushPopUp_Placeholder           = 0x40;      // Transform _rewardsPlaceholder
        constexpr uintptr_t PushPopUp_Description           = 0x48;      // Text _description

        // UnityEngine.PlayerPrefs RVAs from dump.cs (TypeDefIndex: 15451)
        constexpr uintptr_t PlayerPrefs_SetInt              = 0x858FE70; // SetInt(string, int)
        constexpr uintptr_t PlayerPrefs_GetInt              = 0x858FED0; // GetInt(string, int)
        constexpr uintptr_t PlayerPrefs_DeleteKey           = 0x8590870; // DeleteKey(string)
        constexpr uintptr_t PlayerPrefs_Save                = 0x8590A58; // Save()
    }

    // ----------------------------------------------------------------------------------
    // Quest & Reward Claim State (Freeze Claim / Infinite Repeat Claim)
    // ----------------------------------------------------------------------------------
    namespace Quest {
        // QuestState enum from dump.cs (TypeDefIndex: 7402, abbabbbbababbababbaaaabaaaaabbabbbababbaaaaabab)
        namespace QuestState {
            constexpr int Active    = 0;
            constexpr int Complete  = 1; // Completed, reward available to claim
            constexpr int Finished  = 2; // Claimed / Finished
            constexpr int Expired   = 3;
            constexpr int Removed   = 4;
            constexpr int Available = 5;
        }

        // Generic RewardState enum from dump.cs (TypeDefIndex: 3657, aaabababaaaababbbaaabbbbaaaaaabbababaababaaabba)
        namespace RewardState {
            constexpr int Locked    = 0;
            constexpr int Available = 1; // Ready to claim
            constexpr int Claimed   = 2; // Already claimed
        }

        // QuestData (dump.cs class: aabbbaababaababbababbabbaaabaabaaabaabbabbbbbab, TypeDefIndex: 3865)
        constexpr uintptr_t QuestData_StateField             = 0x14;      // QuestState backing field
        constexpr uintptr_t QuestData_GetState               = 0x50169A0; // baaaaaababaabbbaaaabbaaababbbabbbbabbbbbabbbabb() -> QuestState
        constexpr uintptr_t QuestData_SetState1              = 0x50169C4; // aabababbbbabbaababbbbaaabbbbbaaabaababaabbbbaaa(QuestState)
        constexpr uintptr_t QuestData_SetState2              = 0x501697C; // abaabaaabbaaaabaaabaabbabaaaababbbabaaabababbbb(QuestState)

        // Quest Presenter / Wrapper (dump.cs class: bbaaaaaaaabbababaaaababbaababbabaabbbbbbbabbbab, TypeDefIndex: 2229)
        constexpr uintptr_t QuestPresenter_GetState          = 0x4CE8A4C; // baaaaaababaabbbaaaabbaaababbbabbbbabbbbbabbbabb() -> QuestState

        // QuestItemView (dump.cs class: UI.Quests.View.QuestItemView, TypeDefIndex: 8883)
        constexpr uintptr_t QuestItemView_RewardAvailable    = 0x50;      // List<GameObject> _rewardAvailableState
        constexpr uintptr_t QuestItemView_FinishedState      = 0x58;      // List<GameObject> _finishedState
        constexpr uintptr_t QuestItemView_ClaimButton        = 0x98;      // Button _claimRewardButton
        constexpr uintptr_t QuestItemView_QuestField         = 0xB0;      // abbbabbabaaabbaaabaababaaabbabaabbaabbbbabaaaab
        constexpr uintptr_t QuestItemView_StateField         = 0xC8;      // QuestState
        constexpr uintptr_t QuestItemView_UpdateState        = 0x4556AA8; // UpdateState() -> void

        // DailyRepeatingQuestView (dump.cs class: UI.Quests.View.DailyRepeatingQuestView, TypeDefIndex: 8061)
        constexpr uintptr_t DailyRepeatingQuestView_NotFinished     = 0x40; // List<GameObject> _notFinishedState
        constexpr uintptr_t DailyRepeatingQuestView_RewardAvailable = 0x48; // List<GameObject> _rewardAvailableState
        constexpr uintptr_t DailyRepeatingQuestView_FinishedState   = 0x50; // List<GameObject> _finishedState
        constexpr uintptr_t DailyRepeatingQuestView_ClaimButton     = 0xA0; // Button _claimRewardButton
        constexpr uintptr_t DailyRepeatingQuestView_QuestField      = 0xC8; // Quest
        constexpr uintptr_t DailyRepeatingQuestView_StateField      = 0xD0; // QuestState
        constexpr uintptr_t DailyRepeatingQuestView_UpdateState     = 0x43C3114; // UpdateState() -> void

        // DailyQuestGroupRewardsView (dump.cs class: UI.Quests.View.DailyQuestGroupRewardsView, TypeDefIndex: 4109)
        constexpr uintptr_t DailyQuestGroupRewards_GetState  = 0x5074A08; // aabaaaabaabbaabbababbbababbbabaaabbababbbbbbabb() -> RewardState

        // GroupRewardItemView (dump.cs class: UI.Quests.View.GroupRewardItemView, TypeDefIndex: 10370)
        constexpr uintptr_t GroupRewardItemView_ClaimButton  = 0x60;      // Button _claimRewardButton
        constexpr uintptr_t GroupRewardItemView_CanClaim     = 0x4809AC4; // babaababbbbbaaaababbbaaaaaabbabababbbaabbbaaaba() -> bool

        // DailyRewardContainer (dump.cs class: UI.Reward.View.DailyRewardContainer, TypeDefIndex: 9170)
        constexpr uintptr_t DailyReward_ItemToggle           = 0x28;      // Toggle _itemToggle
        constexpr uintptr_t DailyReward_ClaimedBg            = 0x48;      // GameObject _claimedBgGo
        constexpr uintptr_t DailyReward_ClaimedFrame         = 0x50;      // GameObject _claimedFrameGo
        constexpr uintptr_t DailyRewardContainer_Setup       = 0x45CB45C; // Setup(...) -> void
    }

    // ----------------------------------------------------------------------------------
    // Unlock Weapon (Primary Lobby Equipment & In-Game Shooting Range Logic)
    // ----------------------------------------------------------------------------------
    namespace UnlockWeapon {
        // Equipment Slot Enum: 0 = Primary, 1 = Secondary, 2 = Throwable, 3 = Melee
        namespace EquipmentSlot {
            constexpr int Primary   = 0;
            constexpr int Secondary = 1;
            constexpr int Throwable = 2;
            constexpr int Melee     = 3;
        }

        // ShopItemProfileData (dump.cs TypeDefIndex: 2517)
        constexpr uintptr_t ShopItemProfile_IsBought            = 0x4D52074; // get_IsBought() -> bool
        constexpr uintptr_t ShopItemProfile_IsHave              = 0x4D50F2C; // get_IsHave() -> bool
        constexpr uintptr_t ShopItemProfile_HasAvailableToEquip = 0x4D50F50; // get_HasAvailableToEquip() -> bool
        constexpr uintptr_t ShopItemProfile_IsAvailable         = 0x4D52120; // get_IsAvailable() -> bool

        // ShopItemExtensionProfileData (dump.cs TypeDefIndex: 4255)
        constexpr uintptr_t ShopItemExtProfile_IsBought         = 0x4254B7C; // get_IsBought() -> bool
        constexpr uintptr_t ShopItemExtProfile_HasAvailableToEquip = 0x4254F58; // get_HasAvailableToEquip() -> bool
        constexpr uintptr_t ShopItemExtProfile_IsHave           = 0x4254F7C; // get_IsHave() -> bool

        // ShopItemBase (dump.cs TypeDefIndex: 185)
        constexpr uintptr_t ShopItemBase_IsLevelReached         = 0x4BB780C; // get_IsActualRequiredLevelReached() -> bool
        constexpr uintptr_t ShopItemBase_GetKey                 = 0x4BB7C3C; // get_Key() -> int
        constexpr uintptr_t ShopItemBase_GetName                = 0x4BB7C14; // get_Name() -> string
        constexpr uintptr_t ShopItemBase_GetTranslatedName      = 0x4BB6740; // get_TranslatedName() -> string

        // Slot 24 virtual isBought implementations
        constexpr uintptr_t ShopItem_IsBoughtSlot               = 0x4103330; // ShopItem Slot 24
        constexpr uintptr_t ShopItemExt_IsBoughtSlot            = 0x50CA4BC; // ShopItemExtension Slot 24

        // UiShopEquipButtonView (dump.cs TypeDefIndex: 6732)
        constexpr uintptr_t UiShopEquipButton_CanEquip          = 0x4138AE8; // babbbbbbaaabaaaaaabbabababbbbbabbaabbabbaaaaaaa() -> bool

        // UiEquipLockView (dump.cs TypeDefIndex: 5546)
        constexpr uintptr_t UiEquipLockView_UpdateView          = 0x3EEF948; // UpdateView(context) -> void
        constexpr uintptr_t UiEquipLockView_LockContainer       = 0x20;      // GameObject _customizationLockContainer

        // WeaponCustomizationController (dump.cs TypeDefIndex: 6961)
        constexpr uintptr_t WeaponCustomization_SelectWeapon    = 0x41A3CE4; // abbbbababaaabaaaabbababbaababbbaaaabbabaaaaaaaa(int id)
        constexpr uintptr_t WeaponCustomization_GetInstance     = 0x41A0470; // bbbaabaaaabababbbaabbaabbaaabbbaababbbbaaabbbaa() -> instance
        constexpr uintptr_t WeaponCustomization_ShopItemField   = 0xC8;      // ShopItemBase
        constexpr uintptr_t WeaponCustomization_WeaponIdField   = 0xD4;      // int weaponId

        // UiShopGoToShootingRangeButtonView (dump.cs TypeDefIndex: 2966)
        constexpr uintptr_t ShootingRangeButton_WeaponIdField   = 0x38;      // int weaponId

        // Match Weapon Config (Shooting Range Mode Weapon Override Logic)
        constexpr uintptr_t MatchConfig1_GetWeaponId            = 0x3E97924; // TypeDefIndex: 28 -> int
        constexpr uintptr_t MatchConfig2_GetWeaponId            = 0x44577FC; // TypeDefIndex: 8313 -> int

        // Loadout & Inventory Weapon Resolution
        constexpr uintptr_t LoadoutManager_GetWeaponId1         = 0x3F164EC; // aabbbabaaabaabaaaabaababbabbaaaaaabaaaabbbbbbba(int, Slot) -> int
        constexpr uintptr_t LoadoutManager_GetWeaponId2         = 0x3F16DD4; // babaaababbaaababbbaaababbabaabaabbabbabbbbbabab(int, Slot) -> int
        constexpr uintptr_t LoadoutManager_EquipWeapon          = 0x3F16258; // aabababbabbabaaaaaaaaabbbbaaaaaaaabbabbbabbaabb(int, int, int, bool)
        constexpr uintptr_t FPC_GetSlotWeaponId                 = 0x408DB4C; // bbaaababbbbbaaababbbabbabbbabaabaaabbaaabababab(Slot) -> int
        constexpr uintptr_t UserKit_GetSlot                     = 0x460D710; // bbbbaabbabbbaaaabbabbbbbbbbababaabbaabbbbbaaaba(Slot) -> UserKitSlot*
        constexpr uintptr_t UserKitSlot_GetItemId               = 0x42AADDC; // aabbabaabbaabbabbbaababbaabaaabaaabaabbbbabbbba() -> int
        constexpr uintptr_t UserKitSlot_ItemIdField             = 0x10;      // int ItemId
        constexpr uintptr_t UserKitSlot_SlotField               = 0x28;      // EquipmentSlot
    }

} // namespace Offsets

#endif // GAME_OFFSETS_H
