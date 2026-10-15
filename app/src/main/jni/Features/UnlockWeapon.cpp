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

    // Feature activation state
    static std::atomic<bool> s_enabled{false};

    // Active unlocked weapon state
    static std::atomic<int> s_selectedWeaponId{0};
    static std::mutex s_weaponNameMutex;
    static std::string s_selectedWeaponName = "";

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

    // Il2Cpp runtime exports
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

    // Read IL2CPP managed string (UTF-16) safely into std::string
    static std::string ReadIl2CppString(void *il2cppString) {
        if (!Memory::IsValidPtr(il2cppString)) return "";
        // Offset 0x10 is 32-bit int length in 64-bit IL2CPP
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

    // Helper: Extract name from ShopItemBase pointer
    static void UpdateWeaponNameFromShopItem(void *shopItemBase, int weaponId) {
        std::string nameFound;
        if (Memory::IsValidPtr(shopItemBase)) {
            // Try get_TranslatedName() first
            static auto pfnGetTranslatedName = reinterpret_cast<void* (*)(void*)>(
                Memory::GetRVA(Offsets::UnlockWeapon::ShopItemBase_GetTranslatedName)
            );
            if (pfnGetTranslatedName) {
                void *strObj = pfnGetTranslatedName(shopItemBase);
                nameFound = ReadIl2CppString(strObj);
            }

            // Fallback to get_Name() if translated name is empty
            if (nameFound.empty()) {
                static auto pfnGetName = reinterpret_cast<void* (*)(void*)>(
                    Memory::GetRVA(Offsets::UnlockWeapon::ShopItemBase_GetName)
                );
                if (pfnGetName) {
                    void *strObj = pfnGetName(shopItemBase);
                    nameFound = ReadIl2CppString(strObj);
                }
            }
        }

        if (nameFound.empty()) {
            nameFound = "Weapon #" + std::to_string(weaponId);
        }

        std::lock_guard<std::mutex> lock(s_weaponNameMutex);
        s_selectedWeaponName = nameFound;
    }

    // Helper: Query active weapon if not set yet from WeaponCustomizationController singleton
    static void QueryActiveWeaponFromLobby() {
        if (s_selectedWeaponId.load() > 0) return;

        static auto pfnGetInstance = reinterpret_cast<void* (*)()>(
            Memory::GetRVA(Offsets::UnlockWeapon::WeaponCustomization_GetInstance)
        );
        if (!pfnGetInstance) return;

        void *instance = pfnGetInstance();
        if (!instance || !Memory::IsValidPtr(instance)) return;

        int currId = Memory::ReadField<int>(instance, Offsets::UnlockWeapon::WeaponCustomization_WeaponIdField);
        if (currId > 0) {
            s_selectedWeaponId.store(currId);
            void *shopItem = Memory::ReadField<void*>(instance, Offsets::UnlockWeapon::WeaponCustomization_ShopItemField);
            UpdateWeaponNameFromShopItem(shopItem, currId);
            LOG_INFO("[UNLOCK_WEAPON] Auto-detected primary weapon from lobby controller: ID %d ('%s')",
                     currId, s_selectedWeaponName.c_str());
        }
    }

    // -------------------------------------------------------------------------
    // Function Pointer Declarations (Originals)
    // -------------------------------------------------------------------------

    // Shop Item & Profile Unlock Hooks
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

    // Selection Hooks
    static void (*old_WeaponCustomization_SelectWeapon)(void *instance, int weaponId) = nullptr;

    // In-Game Weapon Logic Hooks (Shooting Range Mode & Loadout Override)
    static int  (*old_MatchConfig1_GetWeaponId)(void *instance) = nullptr;
    static int  (*old_MatchConfig2_GetWeaponId)(void *instance) = nullptr;
    static int  (*old_LoadoutManager_GetWeaponId1)(void *instance, int kitIndex, int slot) = nullptr;
    static int  (*old_LoadoutManager_GetWeaponId2)(void *instance, int kitIndex, int slot) = nullptr;
    static int  (*old_LoadoutManager_GetWeaponId3)(void *instance, int slot, int kitIndex, bool unk) = nullptr;
    static int  (*old_LoadoutManager_GetWeaponId4)(void *instance, int slot, int kitIndex, bool unk) = nullptr;
    static void*(*old_LoadoutManager_GetKit)(void *instance, int kitIndex) = nullptr;
    static void (*old_LoadoutManager_EquipWeapon)(void *instance, int itemId, int slot, int kitIndex, bool sendToServer) = nullptr;

    static int  (*old_FPC_GetSlotWeaponId)(void *instance, int slot) = nullptr;
    static void*(*old_UserKit_GetSlot)(void *instance, int slot) = nullptr;
    static int  (*old_UserKitSlot_GetItemId)(void *instance) = nullptr;
    static int  (*old_UserKitSlot_GetItemId2)(void *instance) = nullptr;

    // Equipment Slot UI View Hooks
    static void (*old_SimpleInventorySlot_UpdateView)(void *instance) = nullptr;
    static void (*old_SimpleInventorySlot_Bind)(void *instance, void *item, int kitIndex) = nullptr;

    // -------------------------------------------------------------------------
    // Hook Implementations
    // -------------------------------------------------------------------------

    // 1. ShopItemProfileData::get_IsBought (RVA: 0x4D52074)
    static bool hook_ShopItemProfile_IsBought(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_IsBought ? old_ShopItemProfile_IsBought(instance) : false;
    }

    // 2. ShopItemProfileData::get_IsHave (RVA: 0x4D50F2C)
    static bool hook_ShopItemProfile_IsHave(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_IsHave ? old_ShopItemProfile_IsHave(instance) : false;
    }

    // 3. ShopItemProfileData::get_HasAvailableToEquip (RVA: 0x4D50F50)
    static bool hook_ShopItemProfile_HasAvailableToEquip(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_HasAvailableToEquip ? old_ShopItemProfile_HasAvailableToEquip(instance) : false;
    }

    // 4. ShopItemProfileData::get_IsAvailable (RVA: 0x4D52120)
    static bool hook_ShopItemProfile_IsAvailable(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemProfile_IsAvailable ? old_ShopItemProfile_IsAvailable(instance) : false;
    }

    // 5. ShopItemExtensionProfileData::get_IsBought (RVA: 0x4254B7C)
    static bool hook_ShopItemExtProfile_IsBought(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExtProfile_IsBought ? old_ShopItemExtProfile_IsBought(instance) : false;
    }

    // 6. ShopItemExtensionProfileData::get_HasAvailableToEquip (RVA: 0x4254F58)
    static bool hook_ShopItemExtProfile_HasAvailableToEquip(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExtProfile_HasAvailableToEquip ? old_ShopItemExtProfile_HasAvailableToEquip(instance) : false;
    }

    // 7. ShopItemExtensionProfileData::get_IsHave (RVA: 0x4254F7C)
    static bool hook_ShopItemExtProfile_IsHave(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExtProfile_IsHave ? old_ShopItemExtProfile_IsHave(instance) : false;
    }

    // 8. ShopItemBase::get_IsActualRequiredLevelReached (RVA: 0x4BB780C)
    static bool hook_ShopItemBase_IsLevelReached(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemBase_IsLevelReached ? old_ShopItemBase_IsLevelReached(instance) : false;
    }

    // 9. ShopItem Slot 24 isBought (RVA: 0x4103330)
    static bool hook_ShopItem_IsBoughtSlot(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItem_IsBoughtSlot ? old_ShopItem_IsBoughtSlot(instance) : false;
    }

    // 10. ShopItemExtension Slot 24 isBought (RVA: 0x50CA4BC)
    static bool hook_ShopItemExt_IsBoughtSlot(void *instance) {
        if (s_enabled.load()) return true;
        return old_ShopItemExt_IsBoughtSlot ? old_ShopItemExt_IsBoughtSlot(instance) : false;
    }

    // 11. UiShopEquipButtonView::CanEquip (RVA: 0x4138AE8)
    static bool hook_UiShopEquipButton_CanEquip(void *instance) {
        if (s_enabled.load()) return true;
        return old_UiShopEquipButton_CanEquip ? old_UiShopEquipButton_CanEquip(instance) : false;
    }

    // 12. UiEquipLockView::UpdateView (RVA: 0x3EEF948)
    // Hides the lock overlay container ("15 LEVEL", etc.) when feature is active
    static void hook_UiEquipLockView_UpdateView(void *instance, void *context) {
        if (old_UiEquipLockView_UpdateView) {
            old_UiEquipLockView_UpdateView(instance, context);
        }
        if (s_enabled.load() && instance && Memory::IsValidPtr(instance)) {
            void *lockContainer = Memory::ReadField<void*>(
                instance, Offsets::UnlockWeapon::UiEquipLockView_LockContainer
            );
            if (lockContainer && Memory::IsValidPtr(lockContainer)) {
                UnitySetActive(lockContainer, false);
            }
        }
    }

    // 13. WeaponCustomizationController::SelectWeapon (RVA: 0x41A3CE4)
    // Captures the weapon selected by user clicking a weapon card in the equipment UI
    static void hook_WeaponCustomization_SelectWeapon(void *instance, int weaponId) {
        if (old_WeaponCustomization_SelectWeapon) {
            old_WeaponCustomization_SelectWeapon(instance, weaponId);
        }
        if (s_enabled.load() && weaponId > 0) {
            s_selectedWeaponId.store(weaponId);
            if (instance && Memory::IsValidPtr(instance)) {
                void *shopItem = Memory::ReadField<void*>(
                    instance, Offsets::UnlockWeapon::WeaponCustomization_ShopItemField
                );
                UpdateWeaponNameFromShopItem(shopItem, weaponId);
            }
            std::lock_guard<std::mutex> lock(s_weaponNameMutex);
            LOG_INFO("[UNLOCK_WEAPON] Primary weapon chosen from lobby equipment: ID %d ('%s')",
                     weaponId, s_selectedWeaponName.c_str());
        }
    }

    // 14. MatchConfig1_GetWeaponId (RVA: 0x3E97924)
    // Shooting Range Mode Match Session Weapon Logic
    static int hook_MatchConfig1_GetWeaponId(void *instance) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && targetId > 0) {
            return targetId;
        }
        return old_MatchConfig1_GetWeaponId ? old_MatchConfig1_GetWeaponId(instance) : 0;
    }

    // 15. MatchConfig2_GetWeaponId (RVA: 0x44577FC)
    // Shooting Range Mode Match Session Weapon Logic
    static int hook_MatchConfig2_GetWeaponId(void *instance) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && targetId > 0) {
            return targetId;
        }
        return old_MatchConfig2_GetWeaponId ? old_MatchConfig2_GetWeaponId(instance) : 0;
    }

    // 16. LoadoutManager::GetWeaponId1 (RVA: 0x3F164EC)
    // Direct loadout query for Primary Weapon Slot (0)
    static int hook_LoadoutManager_GetWeaponId1(void *instance, int kitIndex, int slot) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary && targetId > 0) {
            return targetId;
        }
        return old_LoadoutManager_GetWeaponId1 ? old_LoadoutManager_GetWeaponId1(instance, kitIndex, slot) : 0;
    }

    // 17. LoadoutManager::GetWeaponId2 (RVA: 0x3F16DD4)
    // Secondary loadout query for Primary Weapon Slot (0)
    static int hook_LoadoutManager_GetWeaponId2(void *instance, int kitIndex, int slot) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary && targetId > 0) {
            return targetId;
        }
        return old_LoadoutManager_GetWeaponId2 ? old_LoadoutManager_GetWeaponId2(instance, kitIndex, slot) : 0;
    }

    // 18. LoadoutManager::GetWeaponId3 (RVA: 0x3F18008)
    static int hook_LoadoutManager_GetWeaponId3(void *instance, int slot, int kitIndex, bool unk) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary && targetId > 0) {
            return targetId;
        }
        return old_LoadoutManager_GetWeaponId3 ? old_LoadoutManager_GetWeaponId3(instance, slot, kitIndex, unk) : 0;
    }

    // 19. LoadoutManager::GetWeaponId4 (RVA: 0x3F1804C)
    static int hook_LoadoutManager_GetWeaponId4(void *instance, int slot, int kitIndex, bool unk) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary && targetId > 0) {
            return targetId;
        }
        return old_LoadoutManager_GetWeaponId4 ? old_LoadoutManager_GetWeaponId4(instance, slot, kitIndex, unk) : 0;
    }

    // 20. LoadoutManager::GetKit (RVA: 0x3F15B70)
    // Ensures the UserKit slot 0 always contains the unlocked weapon ID
    static void* hook_UserLoadoutManager_GetKit(void *instance, int kitIndex) {
        void *userKit = old_LoadoutManager_GetKit ? old_LoadoutManager_GetKit(instance, kitIndex) : nullptr;
        if (s_enabled.load() && userKit && Memory::IsValidPtr(userKit)) {
            int targetId = s_selectedWeaponId.load();
            if (targetId > 0) {
                static auto pfnGetSlot = reinterpret_cast<void* (*)(void*, int)>(
                    Memory::GetRVA(Offsets::UnlockWeapon::UserKit_GetSlot)
                );
                if (pfnGetSlot) {
                    void *slotObj = pfnGetSlot(userKit, Offsets::UnlockWeapon::EquipmentSlot::Primary);
                    if (slotObj && Memory::IsValidPtr(slotObj)) {
                        Memory::WriteField<int>(slotObj, Offsets::UnlockWeapon::UserKitSlot_ItemIdField, targetId);
                    }
                }
            }
        }
        return userKit;
    }

    // 21. LoadoutManager::EquipWeapon (RVA: 0x3F16258)
    // Enforces client-sided equip without sending rejection RPC to server
    static void hook_LoadoutManager_EquipWeapon(void *instance, int itemId, int slot, int kitIndex, bool sendToServer) {
        if (s_enabled.load() && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
            s_selectedWeaponId.store(itemId);
            LOG_INFO("[UNLOCK_WEAPON] Intercepted EquipWeapon Primary: ID %d, kitIndex %d (forcing client-side equip)", itemId, kitIndex);
            if (old_LoadoutManager_EquipWeapon) {
                old_LoadoutManager_EquipWeapon(instance, itemId, slot, kitIndex, false);
            }
            return;
        }
        if (old_LoadoutManager_EquipWeapon) {
            old_LoadoutManager_EquipWeapon(instance, itemId, slot, kitIndex, sendToServer);
        }
    }

    // 22. FirstPersonController::GetSlotWeaponId (RVA: 0x408DB4C)
    // In-game local player weapon ID query for Primary Weapon Slot (0)
    static int hook_FPC_GetSlotWeaponId(void *instance, int slot) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary && targetId > 0) {
            return targetId;
        }
        return old_FPC_GetSlotWeaponId ? old_FPC_GetSlotWeaponId(instance, slot) : 0;
    }

    // 23. UserKit::GetSlot (RVA: 0x460D710)
    // Overrides UserKitSlot ItemId field (0x10) when primary weapon is queried
    static void* hook_UserKit_GetSlot(void *instance, int slot) {
        void *slotObj = old_UserKit_GetSlot ? old_UserKit_GetSlot(instance, slot) : nullptr;
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && slot == Offsets::UnlockWeapon::EquipmentSlot::Primary && targetId > 0) {
            if (slotObj && Memory::IsValidPtr(slotObj)) {
                Memory::WriteField<int>(slotObj, Offsets::UnlockWeapon::UserKitSlot_ItemIdField, targetId);
            }
        }
        return slotObj;
    }

    // 24. UserKitSlot::GetItemId (RVA: 0x42AADDC)
    static int hook_UserKitSlot_GetItemId(void *instance) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && targetId > 0 && instance && Memory::IsValidPtr(instance)) {
            int slotType = Memory::ReadField<int>(instance, Offsets::UnlockWeapon::UserKitSlot_SlotField);
            if (slotType == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
                return targetId;
            }
        }
        return old_UserKitSlot_GetItemId ? old_UserKitSlot_GetItemId(instance) : 0;
    }

    // 25. UserKitSlot::GetItemId2 (RVA: 0x42AADAC)
    static int hook_UserKitSlot_GetItemId2(void *instance) {
        int targetId = s_selectedWeaponId.load();
        if (s_enabled.load() && targetId > 0 && instance && Memory::IsValidPtr(instance)) {
            int slotType = Memory::ReadField<int>(instance, Offsets::UnlockWeapon::UserKitSlot_SlotField);
            if (slotType == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
                return targetId;
            }
        }
        return old_UserKitSlot_GetItemId2 ? old_UserKitSlot_GetItemId2(instance) : 0;
    }

    // 26. SimpleInventorySlot::UpdateView (RVA: 0x4765F20)
    // Forces the equipment UI card to display the unlocked weapon name and ItemId
    static void hook_SimpleInventorySlot_UpdateView(void *instance) {
        if (old_SimpleInventorySlot_UpdateView) {
            old_SimpleInventorySlot_UpdateView(instance);
        }
        if (s_enabled.load() && instance && Memory::IsValidPtr(instance)) {
            int slot = Memory::ReadField<int>(instance, Offsets::UnlockWeapon::SimpleInventorySlot_SlotField);
            if (slot == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
                int targetId = s_selectedWeaponId.load();
                if (targetId > 0) {
                    Memory::WriteField<int>(instance, Offsets::UnlockWeapon::BaseInventorySlot_ItemIdField, targetId);
                    void *nameText = Memory::ReadField<void*>(instance, Offsets::UnlockWeapon::SimpleInventorySlot_TextNameField);
                    if (nameText && Memory::IsValidPtr(nameText)) {
                        std::string weaponName = GetSelectedWeaponName();
                        if (!weaponName.empty()) {
                            SetUnityText(nameText, weaponName.c_str());
                        }
                    }
                }
            }
        }
    }

    // 27. SimpleInventorySlot::Bind (RVA: 0x47665B0)
    static void hook_SimpleInventorySlot_Bind(void *instance, void *item, int kitIndex) {
        if (old_SimpleInventorySlot_Bind) {
            old_SimpleInventorySlot_Bind(instance, item, kitIndex);
        }
        if (s_enabled.load() && instance && Memory::IsValidPtr(instance)) {
            int slot = Memory::ReadField<int>(instance, Offsets::UnlockWeapon::SimpleInventorySlot_SlotField);
            if (slot == Offsets::UnlockWeapon::EquipmentSlot::Primary) {
                int targetId = s_selectedWeaponId.load();
                if (targetId > 0) {
                    Memory::WriteField<int>(instance, Offsets::UnlockWeapon::BaseInventorySlot_ItemIdField, targetId);
                    void *nameText = Memory::ReadField<void*>(instance, Offsets::UnlockWeapon::SimpleInventorySlot_TextNameField);
                    if (nameText && Memory::IsValidPtr(nameText)) {
                        std::string weaponName = GetSelectedWeaponName();
                        if (!weaponName.empty()) {
                            SetUnityText(nameText, weaponName.c_str());
                        }
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Module Lifecycle & Public API
    // -------------------------------------------------------------------------

    void Init() {
        LOG_INFO("[UNLOCK_WEAPON] Initializing Unlock Weapon modular hooks...");

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

        // 2. Hook Weapon Selection in Lobby Equipment
        Memory::HookRVA(Offsets::UnlockWeapon::WeaponCustomization_SelectWeapon,
                        reinterpret_cast<void*>(hook_WeaponCustomization_SelectWeapon),
                        reinterpret_cast<void**>(&old_WeaponCustomization_SelectWeapon));

        // 3. Hook In-Game Weapon Loadout & Match Config (Shooting Range Logic)
        Memory::HookRVA(Offsets::UnlockWeapon::MatchConfig1_GetWeaponId,
                        reinterpret_cast<void*>(hook_MatchConfig1_GetWeaponId),
                        reinterpret_cast<void**>(&old_MatchConfig1_GetWeaponId));

        Memory::HookRVA(Offsets::UnlockWeapon::MatchConfig2_GetWeaponId,
                        reinterpret_cast<void*>(hook_MatchConfig2_GetWeaponId),
                        reinterpret_cast<void**>(&old_MatchConfig2_GetWeaponId));

        Memory::HookRVA(Offsets::UnlockWeapon::LoadoutManager_GetWeaponId1,
                        reinterpret_cast<void*>(hook_LoadoutManager_GetWeaponId1),
                        reinterpret_cast<void**>(&old_LoadoutManager_GetWeaponId1));

        Memory::HookRVA(Offsets::UnlockWeapon::LoadoutManager_GetWeaponId2,
                        reinterpret_cast<void*>(hook_LoadoutManager_GetWeaponId2),
                        reinterpret_cast<void**>(&old_LoadoutManager_GetWeaponId2));

        Memory::HookRVA(Offsets::UnlockWeapon::LoadoutManager_GetWeaponId3,
                        reinterpret_cast<void*>(hook_LoadoutManager_GetWeaponId3),
                        reinterpret_cast<void**>(&old_LoadoutManager_GetWeaponId3));

        Memory::HookRVA(Offsets::UnlockWeapon::LoadoutManager_GetWeaponId4,
                        reinterpret_cast<void*>(hook_LoadoutManager_GetWeaponId4),
                        reinterpret_cast<void**>(&old_LoadoutManager_GetWeaponId4));

        Memory::HookRVA(Offsets::UnlockWeapon::LoadoutManager_GetKit,
                        reinterpret_cast<void*>(hook_UserLoadoutManager_GetKit),
                        reinterpret_cast<void**>(&old_LoadoutManager_GetKit));

        Memory::HookRVA(Offsets::UnlockWeapon::LoadoutManager_EquipWeapon,
                        reinterpret_cast<void*>(hook_LoadoutManager_EquipWeapon),
                        reinterpret_cast<void**>(&old_LoadoutManager_EquipWeapon));

        Memory::HookRVA(Offsets::UnlockWeapon::FPC_GetSlotWeaponId,
                        reinterpret_cast<void*>(hook_FPC_GetSlotWeaponId),
                        reinterpret_cast<void**>(&old_FPC_GetSlotWeaponId));

        Memory::HookRVA(Offsets::UnlockWeapon::UserKit_GetSlot,
                        reinterpret_cast<void*>(hook_UserKit_GetSlot),
                        reinterpret_cast<void**>(&old_UserKit_GetSlot));

        Memory::HookRVA(Offsets::UnlockWeapon::UserKitSlot_GetItemId,
                        reinterpret_cast<void*>(hook_UserKitSlot_GetItemId),
                        reinterpret_cast<void**>(&old_UserKitSlot_GetItemId));

        Memory::HookRVA(Offsets::UnlockWeapon::UserKitSlot_GetItemId2,
                        reinterpret_cast<void*>(hook_UserKitSlot_GetItemId2),
                        reinterpret_cast<void**>(&old_UserKitSlot_GetItemId2));

        // 4. Hook Equipment Slot UI Views
        Memory::HookRVA(Offsets::UnlockWeapon::SimpleInventorySlot_UpdateView,
                        reinterpret_cast<void*>(hook_SimpleInventorySlot_UpdateView),
                        reinterpret_cast<void**>(&old_SimpleInventorySlot_UpdateView));

        Memory::HookRVA(Offsets::UnlockWeapon::SimpleInventorySlot_Bind,
                        reinterpret_cast<void*>(hook_SimpleInventorySlot_Bind),
                        reinterpret_cast<void**>(&old_SimpleInventorySlot_Bind));

        LOG_INFO("[UNLOCK_WEAPON] All Unlock Weapon hooks installed successfully.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable);
        LOG_INFO("[UNLOCK_WEAPON] Feature %s", enable ? "ENABLED" : "DISABLED");
        if (enable) {
            QueryActiveWeaponFromLobby();
        }
    }

    bool IsEnabled() {
        return s_enabled.load();
    }

    int GetSelectedWeaponId() {
        QueryActiveWeaponFromLobby();
        return s_selectedWeaponId.load();
    }

    std::string GetSelectedWeaponName() {
        QueryActiveWeaponFromLobby();
        std::lock_guard<std::mutex> lock(s_weaponNameMutex);
        return s_selectedWeaponName;
    }

    void SetSelectedWeapon(int weaponId, const std::string &name) {
        s_selectedWeaponId.store(weaponId);
        std::lock_guard<std::mutex> lock(s_weaponNameMutex);
        s_selectedWeaponName = name;
        LOG_INFO("[UNLOCK_WEAPON] Weapon explicitly set to ID: %d ('%s')", weaponId, name.c_str());
    }

    std::string GetOverlayStatus() {
        if (!s_enabled.load()) {
            return "DISABLED";
        }
        int id = GetSelectedWeaponId();
        if (id <= 0) {
            return "Pilih di Equipment";
        }
        std::string name = GetSelectedWeaponName();
        if (name.empty()) {
            name = "ID: " + std::to_string(id);
        }
        return name;
    }

} // namespace UnlockWeapon
