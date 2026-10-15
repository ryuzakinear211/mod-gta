#include "UnlockWeapon.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include "../Includes/obfuscate.h"

#include <dlfcn.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdint>

namespace UnlockWeapon {

    // -------------------------------------------------------------------------
    // Weapon Choice Definitions & State
    // -------------------------------------------------------------------------
    struct WeaponEntry {
        int id;
        const char *name;
    };

    static const WeaponEntry k_weaponChoices[] = {
        { 0,  "Default (Senjata Asli)" }, // Index 0: No replacement
        { 7,  "Type 99" },                // Index 1: Type 99 (from user screenshot)
        { 1,  "Mosin-Nagant" },           // Index 2: Mosin
        { 2,  "FG42" },                   // Index 3: FG42
        { 3,  "Kar98k" },                 // Index 4: Karabiner 98k
        { 4,  "M1 Carbine" },             // Index 5: M1 Carbine
        { 5,  "M1 Garand" },              // Index 6: M1 Garand
        { 6,  "SKS Legendary" },          // Index 7: SKS
        { 8,  "Winchester 1895" },        // Index 8: .405 Winchester
        { 9,  "MG34" },                   // Index 9: MG34
        { 10, "Super Bazooka" },          // Index 10: Super Bazooka
        { 11, "M3 Grease Gun" },          // Index 11: M3
        { 12, "Lee Enfield" },            // Index 12: Lee Enfield
        { 13, "MP44 (StG 44)" },          // Index 13: StG 44
        { 14, "Prototype 206" }           // Index 14: Prototype 206 Rifle
    };
    static const int k_totalWeaponChoices = sizeof(k_weaponChoices) / sizeof(k_weaponChoices[0]);

    // Active weapon choice (Spinner selection)
    static std::atomic<int> s_weaponChoiceIndex{0};

    // Feature activation state (enabled whenever a specific weapon is chosen)
    static std::atomic<bool> s_enabled{false};

    // Active selected weapon details
    static std::atomic<int> s_selectedWeaponId{0};
    static std::mutex s_weaponNameMutex;
    static std::string s_selectedWeaponName = "Default (Drop)";

    // Captured TemplateProvider singleton instance (TypeDefIndex: 5465)
    static void *s_templateProviderInstance = nullptr;

    // -------------------------------------------------------------------------
    // Unity Engine Native Helpers
    // -------------------------------------------------------------------------
    static void UnitySetActive(void *go, bool active) {
        if (!Memory::IsValidPtr(go)) return;
        static auto pfnSetActive = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Unity::GameObject_SetActive)
        );
        if (pfnSetActive) pfnSetActive(go, active);
    }

    typedef void* (*il2cpp_string_new_t)(const char *str);
    static il2cpp_string_new_t s_pfnStringNew = nullptr;

    static void SetUnityText(void *textComponent, const char *str) {
        if (!Memory::IsValidPtr(textComponent) || !str) return;
        if (!s_pfnStringNew) {
            void *hLib = dlopen(Offsets::TargetLib::Name, RTLD_NOLOAD);
            if (hLib) {
                s_pfnStringNew = reinterpret_cast<il2cpp_string_new_t>(dlsym(hLib, "il2cpp_string_new"));
            }
        }
        if (!s_pfnStringNew) return;
        void *managedStr = s_pfnStringNew(str);
        if (!managedStr) return;
        static auto pfnSetText = reinterpret_cast<void (*)(void*, void*)>(
            Memory::GetRVA(Offsets::Unity::Text_SetText)
        );
        if (pfnSetText) pfnSetText(textComponent, managedStr);
    }

    static std::string ReadIl2CppString(void *il2cppString) {
        if (!Memory::IsValidPtr(il2cppString)) return "";
        int32_t length = Memory::ReadField<int32_t>(il2cppString, 0x10);
        if (length <= 0 || length > 256) return "";

        const uint16_t *chars = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<uintptr_t>(il2cppString) + 0x14
        );
        if (!Memory::IsValidPtr(const_cast<uint16_t*>(chars))) return "";

        std::string result;
        result.reserve(length);
        for (int i = 0; i < length; ++i) {
            uint16_t c = chars[i];
            if (c < 128) {
                result.push_back(static_cast<char>(c));
            } else {
                result.push_back(' ');
            }
        }
        return result;
    }

    // Helper: Retrieve WeaponTemplate pointer for a given weapon itemId from TemplateProvider
    static void* GetWeaponTemplate(int weaponId) {
        if (!s_templateProviderInstance || !Memory::IsValidPtr(s_templateProviderInstance) || weaponId <= 0) {
            return nullptr;
        }
        static auto pfnGetTemplate = reinterpret_cast<void* (*)(void*, int)>(
            Memory::GetRVA(Offsets::UnlockWeapon::TemplateProvider_GetWeaponTemplate)
        );
        if (!pfnGetTemplate) return nullptr;
        return pfnGetTemplate(s_templateProviderInstance, weaponId);
    }

    // Helper: Safely mutate WeaponProfile ItemId and ObscuredInt WeaponId
    static void ModifyWeaponProfile(void *profile, int targetId) {
        if (!profile || !Memory::IsValidPtr(profile) || targetId <= 0) return;

        // Write plaintext ItemId at offset 0x10
        Memory::WriteField<int>(profile, Offsets::UnlockWeapon::WeaponProfile_ItemIdField, targetId);

        // Update CodeStage ObscuredInt WeaponId at offset 0x38 (hash=0x0, hidden=0x4, key=0x8, fake=0xC)
        uintptr_t obscuredAddr = reinterpret_cast<uintptr_t>(profile) + Offsets::UnlockWeapon::WeaponProfile_WeaponIdField;
        if (Memory::IsValidPtr(reinterpret_cast<void*>(obscuredAddr))) {
            int cryptoKey = *reinterpret_cast<int*>(obscuredAddr + 0x8);
            *reinterpret_cast<int*>(obscuredAddr + 0x4) = targetId ^ cryptoKey;
            *reinterpret_cast<int*>(obscuredAddr + 0xC) = targetId;
        }
    }

    // Helper: Modify a DropObject's container and drop data to represent the chosen weapon
    static void ModifyDropObjectWeapon(void *dropObject, int targetId) {
        if (!dropObject || !Memory::IsValidPtr(dropObject) || targetId <= 0) return;

        // 1. Container at offset 0x98 (baabbbbabbaabaabaabbbbbbbaaababbbabbbaabbaaaabb)
        void *container = Memory::ReadField<void*>(dropObject, Offsets::UnlockWeapon::DropObject_ContainerField);
        if (container && Memory::IsValidPtr(container)) {
            // WeaponProfile at offset 0x60
            void *profile = Memory::ReadField<void*>(container, Offsets::UnlockWeapon::DropWeaponContainer_ProfileField);
            ModifyWeaponProfile(profile, targetId);

            // WeaponTemplate at offset 0x50
            void *newTemplate = GetWeaponTemplate(targetId);
            if (newTemplate && Memory::IsValidPtr(newTemplate)) {
                Memory::WriteField<void*>(container, Offsets::UnlockWeapon::DropWeaponContainer_TemplateField, newTemplate);
            } else {
                void *oldTemplate = Memory::ReadField<void*>(container, Offsets::UnlockWeapon::DropWeaponContainer_TemplateField);
                if (oldTemplate && Memory::IsValidPtr(oldTemplate)) {
                    Memory::WriteField<int>(oldTemplate, 0x10, targetId);
                }
            }
        }

        // 2. DropData at offset 0x90 (babaabbbbbbaabaaabbababbabbbbaaabbbabbabbabaaab)
        void *dropData = Memory::ReadField<void*>(dropObject, Offsets::UnlockWeapon::DropObject_DropDataField);
        if (dropData && Memory::IsValidPtr(dropData)) {
            // Payload at offset 0x30 (baaabababbabbbbabbbabaaaaaaabaabaaabbbbaaabbabb)
            void *payload = Memory::ReadField<void*>(dropData, 0x30);
            if (payload && Memory::IsValidPtr(payload)) {
                // Profile at offset 0x10
                void *profile = Memory::ReadField<void*>(payload, 0x10);
                ModifyWeaponProfile(profile, targetId);
            }
        }

        LOG_INFO("[WEAPON_PICKUP] Modified DropObject %p to chosen weapon ID %d", dropObject, targetId);
    }

    // -------------------------------------------------------------------------
    // Function Pointer Declarations (Originals)
    // -------------------------------------------------------------------------

    // Shop Item & Profile Unlock Hooks (Lobby overview inspection)
    static bool (*old_ShopItemProfile_IsBought)(void *instance) = nullptr;
    static bool (*old_ShopItemProfile_IsHave)(void *instance) = nullptr;
    static bool (*old_ShopItemProfile_HasAvailableToEquip)(void *instance) = nullptr;
    static bool (*old_ShopItemProfile_IsAvailable)(void *instance) = nullptr;

    static bool (*old_ShopItemExtProfile_IsBought)(void *instance) = nullptr;
    static bool (*old_ShopItemExtProfile_HasAvailableToEquip)(void *instance) = nullptr;
    static bool (*old_ShopItemExtProfile_IsHave)(void *instance) = nullptr;

    static bool (*old_ShopItemBase_IsLevelReached)(void *instance) = nullptr;
    static bool (*old_ShopItem_IsBoughtSlot)(void *instance) = nullptr;
    static bool (*old_ShopItemExt_IsBoughtSlot)(void *instance) = nullptr;

    static bool (*old_UiShopEquipButton_CanEquip)(void *instance) = nullptr;
    static void (*old_UiEquipLockView_UpdateView)(void *instance, void *context) = nullptr;

    // Pick-Up Swap Hooks
    static void (*old_DropObject_OnPickup)(void *instance, int64_t userId) = nullptr;
    static int  (*old_DropObject_GetItemId)(void *instance) = nullptr;
    static int  (*old_DropWeaponContainer_GetItemId)(void *instance) = nullptr;

    static void (*old_DropManager_OnPickup)(void *pickable, int64_t userId) = nullptr;
    static void (*old_DropPickupHandler_PickupDropObj1)(void *instance, void *dropObj, int64_t userId) = nullptr;
    static void (*old_DropPickupHandler_PickupDropObj2)(void *instance, void *dropObj, int64_t userId) = nullptr;

    static void (*old_TemplateProvider_Ctor)(void *instance) = nullptr;
    static void*(*old_InventoryBehaviour_AddWeapon)(void *instance, void *templateObj, void *profile, int slot, void *callback) = nullptr;
    static void*(*old_InventoryBehaviour_CreateWeapon)(void *instance, void *profile, int slot) = nullptr;
    static void*(*old_FPC_WieldWeapon)(void *instance, int slot, void *templateObj, void *profile, int wieldReason) = nullptr;
    static int  (*old_FPC_GetSlotWeaponId)(void *instance, int slot) = nullptr;

    // -------------------------------------------------------------------------
    // Hook Implementations
    // -------------------------------------------------------------------------

    // 1. Shop Item & Profile Unlock Hooks
    static bool hook_ShopItemProfile_IsBought(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_IsBought ? old_ShopItemProfile_IsBought(instance) : false;
    }

    static bool hook_ShopItemProfile_IsHave(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_IsHave ? old_ShopItemProfile_IsHave(instance) : false;
    }

    static bool hook_ShopItemProfile_HasAvailableToEquip(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_HasAvailableToEquip ? old_ShopItemProfile_HasAvailableToEquip(instance) : false;
    }

    static bool hook_ShopItemProfile_IsAvailable(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_IsAvailable ? old_ShopItemProfile_IsAvailable(instance) : false;
    }

    static bool hook_ShopItemExtProfile_IsBought(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExtProfile_IsBought ? old_ShopItemExtProfile_IsBought(instance) : false;
    }

    static bool hook_ShopItemExtProfile_HasAvailableToEquip(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExtProfile_HasAvailableToEquip ? old_ShopItemExtProfile_HasAvailableToEquip(instance) : false;
    }

    static bool hook_ShopItemExtProfile_IsHave(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExtProfile_IsHave ? old_ShopItemExtProfile_IsHave(instance) : false;
    }

    static bool hook_ShopItemBase_IsLevelReached(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemBase_IsLevelReached ? old_ShopItemBase_IsLevelReached(instance) : false;
    }

    static bool hook_ShopItem_IsBoughtSlot(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItem_IsBoughtSlot ? old_ShopItem_IsBoughtSlot(instance) : false;
    }

    static bool hook_ShopItemExt_IsBoughtSlot(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExt_IsBoughtSlot ? old_ShopItemExt_IsBoughtSlot(instance) : false;
    }

    static bool hook_UiShopEquipButton_CanEquip(void *instance) {
        if (s_enabled.load()) return true;
        return old_UiShopEquipButton_CanEquip ? old_UiShopEquipButton_CanEquip(instance) : false;
    }

    static void hook_UiEquipLockView_UpdateView(void *instance, void *context) {
        if (old_UiEquipLockView_UpdateView) {
            old_UiEquipLockView_UpdateView(instance, context);
        }
        if (s_enabled.load() && instance && Memory::IsValidPtr(instance)) {
            void *lockGo = Memory::ReadField<void*>(instance, Offsets::UnlockWeapon::UiEquipLockView_LockContainer);
            if (lockGo && Memory::IsValidPtr(lockGo)) {
                UnitySetActive(lockGo, false);
            }
        }
    }

    // 2. TemplateProvider constructor hook: capture instance singleton
    static void hook_TemplateProvider_Ctor(void *instance) {
        if (old_TemplateProvider_Ctor) {
            old_TemplateProvider_Ctor(instance);
        }
        s_templateProviderInstance = instance;
        LOG_INFO("[WEAPON_PICKUP] Captured TemplateProvider instance: %p", instance);
    }

    // 3. DropObject::OnPickup (RVA: 0x4EA50C8, Slot 39)
    // Called when the local player triggers pickup on a dropped weapon
    static void hook_DropObject_OnPickup(void *instance, int64_t userId) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && instance && Memory::IsValidPtr(instance)) {
            ModifyDropObjectWeapon(instance, chosenId);
        }
        if (old_DropObject_OnPickup) {
            old_DropObject_OnPickup(instance, userId);
        }
    }

    // 4. DropManager::OnPickup (RVA: 0x4DBEB14)
    // Static pickup event listener registered to PickableObjectBehaviour
    static void hook_DropManager_OnPickup(void *pickable, int64_t userId) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && pickable && Memory::IsValidPtr(pickable)) {
            ModifyDropObjectWeapon(pickable, chosenId);
        }
        if (old_DropManager_OnPickup) {
            old_DropManager_OnPickup(pickable, userId);
        }
    }

    // 5. DropPickupHandler drop pickup calls (RVA: 0x51C72E0 & 0x51C7C5C)
    static void hook_DropPickupHandler_PickupDropObj1(void *instance, void *dropObj, int64_t userId) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && dropObj && Memory::IsValidPtr(dropObj)) {
            ModifyDropObjectWeapon(dropObj, chosenId);
        }
        if (old_DropPickupHandler_PickupDropObj1) {
            old_DropPickupHandler_PickupDropObj1(instance, dropObj, userId);
        }
    }

    static void hook_DropPickupHandler_PickupDropObj2(void *instance, void *dropObj, int64_t userId) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && dropObj && Memory::IsValidPtr(dropObj)) {
            ModifyDropObjectWeapon(dropObj, chosenId);
        }
        if (old_DropPickupHandler_PickupDropObj2) {
            old_DropPickupHandler_PickupDropObj2(instance, dropObj, userId);
        }
    }

    // 6. DropObject::GetItemId (RVA: 0x4EA6360, Slot 31)
    // Returns the item ID of the dropped weapon
    static int hook_DropObject_GetItemId(void *instance) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0) {
            return chosenId;
        }
        return old_DropObject_GetItemId ? old_DropObject_GetItemId(instance) : 0;
    }

    // 7. DropWeaponContainer::GetItemId (RVA: 0x51F6598, Slot 19)
    static int hook_DropWeaponContainer_GetItemId(void *instance) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0) {
            return chosenId;
        }
        return old_DropWeaponContainer_GetItemId ? old_DropWeaponContainer_GetItemId(instance) : 0;
    }

    // 8. InventoryBehaviour::AddWeapon (RVA: 0x52B6090)
    // Equips the weapon into the player's inventory slot (Slot 0 = Primary)
    static void* hook_InventoryBehaviour_AddWeapon(void *instance, void *templateObj, void *profile, int slot, void *callback) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
            ModifyWeaponProfile(profile, chosenId);
            void *newTemplate = GetWeaponTemplate(chosenId);
            if (newTemplate && Memory::IsValidPtr(newTemplate)) {
                templateObj = newTemplate;
            } else if (templateObj && Memory::IsValidPtr(templateObj)) {
                Memory::WriteField<int>(templateObj, 0x10, chosenId);
            }
            LOG_INFO("[WEAPON_PICKUP] InventoryBehaviour::AddWeapon overridden with weapon ID %d", chosenId);
        }
        return old_InventoryBehaviour_AddWeapon ?
            old_InventoryBehaviour_AddWeapon(instance, templateObj, profile, slot, callback) : nullptr;
    }

    // 9. InventoryBehaviour::CreateWeapon (RVA: 0x52B6568)
    static void* hook_InventoryBehaviour_CreateWeapon(void *instance, void *profile, int slot) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
            ModifyWeaponProfile(profile, chosenId);
            LOG_INFO("[WEAPON_PICKUP] InventoryBehaviour::CreateWeapon overridden with weapon ID %d", chosenId);
        }
        return old_InventoryBehaviour_CreateWeapon ?
            old_InventoryBehaviour_CreateWeapon(instance, profile, slot) : nullptr;
    }

    // 10. FirstPersonController::WieldWeapon (RVA: 0x4097C90)
    // Instantiates weapon view and begins wielding in-game
    static void* hook_FPC_WieldWeapon(void *instance, int slot, void *templateObj, void *profile, int wieldReason) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
            ModifyWeaponProfile(profile, chosenId);
            void *newTemplate = GetWeaponTemplate(chosenId);
            if (newTemplate && Memory::IsValidPtr(newTemplate)) {
                templateObj = newTemplate;
            } else if (templateObj && Memory::IsValidPtr(templateObj)) {
                Memory::WriteField<int>(templateObj, 0x10, chosenId);
            }
            LOG_INFO("[WEAPON_PICKUP] FPC::WieldWeapon overridden with weapon ID %d", chosenId);
        }
        return old_FPC_WieldWeapon ?
            old_FPC_WieldWeapon(instance, slot, templateObj, profile, wieldReason) : nullptr;
    }

    // 11. FirstPersonController::GetSlotWeaponId (RVA: 0x408DB4C)
    static int hook_FPC_GetSlotWeaponId(void *instance, int slot) {
        int chosenId = GetSelectedWeaponId();
        if (chosenId > 0 && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
            return chosenId;
        }
        return old_FPC_GetSlotWeaponId ? old_FPC_GetSlotWeaponId(instance, slot) : 0;
    }

    // -------------------------------------------------------------------------
    // Module Lifecycle & Public API
    // -------------------------------------------------------------------------

    void Init() {
        LOG_INFO("[WEAPON_PICKUP] Initializing Pick-Up Weapon Swap hooks...");

        // 1. Hook Lobby Profile & Store Unlock
        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemProfile_IsBought,
                        reinterpret_cast<void*>(hook_ShopItemProfile_IsBought),
                        reinterpret_cast<void**>(&old_ShopItemProfile_IsBought));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemProfile_IsHave,
                        reinterpret_cast<void*>(hook_ShopItemProfile_IsHave),
                        reinterpret_cast<void**>(&old_ShopItemProfile_IsHave));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemProfile_HasAvailableToEquip,
                        reinterpret_cast<void*>(hook_ShopItemProfile_HasAvailableToEquip),
                        reinterpret_cast<void**>(&old_ShopItemProfile_HasAvailableToEquip));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemProfile_IsAvailable,
                        reinterpret_cast<void*>(hook_ShopItemProfile_IsAvailable),
                        reinterpret_cast<void**>(&old_ShopItemProfile_IsAvailable));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemExtProfile_IsBought,
                        reinterpret_cast<void*>(hook_ShopItemExtProfile_IsBought),
                        reinterpret_cast<void**>(&old_ShopItemExtProfile_IsBought));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemExtProfile_HasAvailableToEquip,
                        reinterpret_cast<void*>(hook_ShopItemExtProfile_HasAvailableToEquip),
                        reinterpret_cast<void**>(&old_ShopItemExtProfile_HasAvailableToEquip));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemExtProfile_IsHave,
                        reinterpret_cast<void*>(hook_ShopItemExtProfile_IsHave),
                        reinterpret_cast<void**>(&old_ShopItemExtProfile_IsHave));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemBase_IsLevelReached,
                        reinterpret_cast<void*>(hook_ShopItemBase_IsLevelReached),
                        reinterpret_cast<void**>(&old_ShopItemBase_IsLevelReached));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItem_IsBoughtSlot,
                        reinterpret_cast<void*>(hook_ShopItem_IsBoughtSlot),
                        reinterpret_cast<void**>(&old_ShopItem_IsBoughtSlot));

        Memory::HookRVA(Offsets::UnlockWeapon::ShopItemExt_IsBoughtSlot,
                        reinterpret_cast<void*>(hook_ShopItemExt_IsBoughtSlot),
                        reinterpret_cast<void**>(&old_ShopItemExt_IsBoughtSlot));

        Memory::HookRVA(Offsets::UnlockWeapon::UiShopEquipButton_CanEquip,
                        reinterpret_cast<void*>(hook_UiShopEquipButton_CanEquip),
                        reinterpret_cast<void**>(&old_UiShopEquipButton_CanEquip));

        Memory::HookRVA(Offsets::UnlockWeapon::UiEquipLockView_UpdateView,
                        reinterpret_cast<void*>(hook_UiEquipLockView_UpdateView),
                        reinterpret_cast<void**>(&old_UiEquipLockView_UpdateView));

        // 2. Hook In-Game Weapon Pick-Up & Ground Drops
        Memory::HookRVA(Offsets::UnlockWeapon::DropObject_OnPickup,
                        reinterpret_cast<void*>(hook_DropObject_OnPickup),
                        reinterpret_cast<void**>(&old_DropObject_OnPickup));

        Memory::HookRVA(Offsets::UnlockWeapon::DropObject_GetItemId,
                        reinterpret_cast<void*>(hook_DropObject_GetItemId),
                        reinterpret_cast<void**>(&old_DropObject_GetItemId));

        Memory::HookRVA(Offsets::UnlockWeapon::DropWeaponContainer_GetItemId,
                        reinterpret_cast<void*>(hook_DropWeaponContainer_GetItemId),
                        reinterpret_cast<void**>(&old_DropWeaponContainer_GetItemId));

        Memory::HookRVA(Offsets::UnlockWeapon::DropManager_OnPickup,
                        reinterpret_cast<void*>(hook_DropManager_OnPickup),
                        reinterpret_cast<void**>(&old_DropManager_OnPickup));

        Memory::HookRVA(Offsets::UnlockWeapon::DropPickupHandler_PickupDropObj1,
                        reinterpret_cast<void*>(hook_DropPickupHandler_PickupDropObj1),
                        reinterpret_cast<void**>(&old_DropPickupHandler_PickupDropObj1));

        Memory::HookRVA(Offsets::UnlockWeapon::DropPickupHandler_PickupDropObj2,
                        reinterpret_cast<void*>(hook_DropPickupHandler_PickupDropObj2),
                        reinterpret_cast<void**>(&old_DropPickupHandler_PickupDropObj2));

        // 3. Hook Template Provider & Inventory Instantiation
        Memory::HookRVA(Offsets::UnlockWeapon::TemplateProvider_Ctor,
                        reinterpret_cast<void*>(hook_TemplateProvider_Ctor),
                        reinterpret_cast<void**>(&old_TemplateProvider_Ctor));

        Memory::HookRVA(Offsets::UnlockWeapon::InventoryBehaviour_AddWeapon,
                        reinterpret_cast<void*>(hook_InventoryBehaviour_AddWeapon),
                        reinterpret_cast<void**>(&old_InventoryBehaviour_AddWeapon));

        Memory::HookRVA(Offsets::UnlockWeapon::InventoryBehaviour_CreateWeapon,
                        reinterpret_cast<void*>(hook_InventoryBehaviour_CreateWeapon),
                        reinterpret_cast<void**>(&old_InventoryBehaviour_CreateWeapon));

        Memory::HookRVA(Offsets::UnlockWeapon::FPC_WieldWeapon,
                        reinterpret_cast<void*>(hook_FPC_WieldWeapon),
                        reinterpret_cast<void**>(&old_FPC_WieldWeapon));

        Memory::HookRVA(Offsets::UnlockWeapon::FPC_GetSlotWeaponId,
                        reinterpret_cast<void*>(hook_FPC_GetSlotWeaponId),
                        reinterpret_cast<void**>(&old_FPC_GetSlotWeaponId));

        LOG_INFO("[WEAPON_PICKUP] All Pick-Up Weapon Swap hooks installed successfully.");
    }

    void SetWeaponChoice(int choiceIndex) {
        if (choiceIndex < 0 || choiceIndex >= k_totalWeaponChoices) {
            choiceIndex = 0;
        }
        s_weaponChoiceIndex.store(choiceIndex);

        int weaponId = k_weaponChoices[choiceIndex].id;
        const char *name = k_weaponChoices[choiceIndex].name;

        s_selectedWeaponId.store(weaponId);
        {
            std::lock_guard<std::mutex> lock(s_weaponNameMutex);
            s_selectedWeaponName = name;
        }
        s_enabled.store(choiceIndex > 0);

        LOG_INFO("[WEAPON_PICKUP] Weapon choice selected: index %d -> ID %d ('%s'), enabled=%d",
                 choiceIndex, weaponId, name, s_enabled.load() ? 1 : 0);
    }

    int GetWeaponChoice() {
        return s_weaponChoiceIndex.load();
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable);
        if (!enable) {
            s_weaponChoiceIndex.store(0);
            s_selectedWeaponId.store(0);
            std::lock_guard<std::mutex> lock(s_weaponNameMutex);
            s_selectedWeaponName = "Default (Drop)";
        }
        LOG_INFO("[WEAPON_PICKUP] Feature %s", enable ? "ENABLED" : "DISABLED");
    }

    bool IsEnabled() {
        return s_enabled.load();
    }

    int GetSelectedWeaponId() {
        int idx = s_weaponChoiceIndex.load();
        if (idx > 0 && idx < k_totalWeaponChoices) {
            return k_weaponChoices[idx].id;
        }
        return s_selectedWeaponId.load();
    }

    std::string GetSelectedWeaponName() {
        int idx = s_weaponChoiceIndex.load();
        if (idx > 0 && idx < k_totalWeaponChoices) {
            return k_weaponChoices[idx].name;
        }
        std::lock_guard<std::mutex> lock(s_weaponNameMutex);
        return s_selectedWeaponName;
    }

    void SetSelectedWeapon(int weaponId, const std::string &name) {
        s_selectedWeaponId.store(weaponId);
        {
            std::lock_guard<std::mutex> lock(s_weaponNameMutex);
            s_selectedWeaponName = name;
        }
        s_enabled.store(weaponId > 0);
        LOG_INFO("[WEAPON_PICKUP] Weapon explicitly set to ID: %d ('%s')", weaponId, name.c_str());
    }

    std::string GetOverlayStatus() {
        int idx = s_weaponChoiceIndex.load();
        if (idx <= 0 || !s_enabled.load()) {
            return "Default (Drop)";
        }
        return GetSelectedWeaponName();
    }

} // namespace UnlockWeapon
