#include "NotificationReward.h"
#include "../Includes/obfuscate.h"
#include "../Menu/Setup.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"

#include <dlfcn.h>
#include <atomic>
#include <cstdio>

namespace NotificationReward {

    // Atomic claim counter
    static std::atomic<int> s_claimCount{0};

    // Captured pointers from Il2Cpp runtime
    static void *s_pPushSetting     = nullptr;
    static void *s_pPermController  = nullptr;
    static void *s_pPermScreen      = nullptr;

    // Original function pointers
    static void* (*orig_Settings_GetSetting)(int key) = nullptr;
    static void  (*orig_SettingBool_SetValue)(void *instance, bool val) = nullptr;
    static void  (*orig_SettingBool_Save)(void *instance) = nullptr;
    static bool  (*orig_SettingBool_GetValue)(void *instance) = nullptr;

    static void  (*orig_PermCtrl_ctor)(void *instance, void *screen) = nullptr;
    static bool  (*orig_PermCtrl_Check)(void *instance) = nullptr;
    static void  (*orig_PermCtrl_Show)(void *instance) = nullptr;
    static void  (*orig_PermCtrl_OnGranted1)(void *instance, bool granted) = nullptr;
    static void  (*orig_PermCtrl_OnGranted2)(void *instance, bool granted) = nullptr;
    static void  (*orig_PermCtrl_OnResult)(void *instance, bool granted) = nullptr;

    static void  (*orig_PermScreen_ctor)(void *instance) = nullptr;
    static void  (*orig_PermScreen_OnClickYes)(void *instance) = nullptr;

    static void  (*orig_PlayerPrefs_SetInt)(void *key, int val) = nullptr;
    static void  (*orig_PlayerPrefs_DeleteKey)(void *key) = nullptr;
    static void  (*orig_PlayerPrefs_Save)() = nullptr;

    // Hook: SettingBool_GetValue (0x6EF91D0)
    // Ensures that any system querying PushNotificationReward (key 23)
    // receives 'false' (unclaimed), making the event repeatable.
    static bool hook_SettingBool_GetValue(void *instance) {
        if (instance != nullptr && instance == s_pPushSetting) {
            return false;
        }
        return orig_SettingBool_GetValue ? orig_SettingBool_GetValue(instance) : false;
    }

    // Hook: PermissionController.Check (0x3F89344)
    // Always returns true so game checks recognize permission as eligible to request.
    static bool hook_PermCtrl_Check(void *instance) {
        return true;
    }

    // Hook: PermissionController..ctor (0x3F8A84C)
    // Automatically captures active PermissionController and PermissionRequestScreen instances.
    static void hook_PermCtrl_ctor(void *instance, void *screen) {
        s_pPermController = instance;
        if (screen != nullptr) {
            s_pPermScreen = screen;
        }
        LOG_INFO("[NOTIF_REWARD] Hooked PermController ctor: controller=%p, screen=%p", instance, screen);
        if (orig_PermCtrl_ctor) orig_PermCtrl_ctor(instance, screen);
    }

    // Hook: PermissionRequestScreen..ctor (0x445E34C)
    static void hook_PermScreen_ctor(void *instance) {
        s_pPermScreen = instance;
        LOG_INFO("[NOTIF_REWARD] Hooked PermScreen ctor: screen=%p", instance);
        if (orig_PermScreen_ctor) orig_PermScreen_ctor(instance);
    }

    void Init() {
        LOG_INFO("[NOTIF_REWARD] Initializing Notification Permission Reward module...");

        // Resolve Settings functions
        orig_Settings_GetSetting = reinterpret_cast<void* (*)(int)>(
            Memory::GetRVA(Offsets::Notification::Settings_GetSetting)
        );
        orig_SettingBool_SetValue = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::SettingBool_SetValue)
        );
        orig_SettingBool_Save = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::SettingBool_Save)
        );

        // Resolve Permission Controller functions
        orig_PermCtrl_Show = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_Show)
        );
        orig_PermCtrl_OnGranted1 = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_OnGranted1)
        );
        orig_PermCtrl_OnGranted2 = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_OnGranted2)
        );
        orig_PermCtrl_OnResult = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_OnResult)
        );

        // Resolve Permission Screen functions
        orig_PermScreen_OnClickYes = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PermissionScreen_OnClickYes)
        );

        // Resolve PlayerPrefs functions
        orig_PlayerPrefs_SetInt = reinterpret_cast<void (*)(void*, int)>(
            Memory::GetRVA(Offsets::Notification::PlayerPrefs_SetInt)
        );
        orig_PlayerPrefs_DeleteKey = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PlayerPrefs_DeleteKey)
        );
        orig_PlayerPrefs_Save = reinterpret_cast<void (*)()>(
            Memory::GetRVA(Offsets::Notification::PlayerPrefs_Save)
        );

        // Install hooks
        Memory::HookRVA(Offsets::Notification::SettingBool_GetValue,
                        reinterpret_cast<void*>(hook_SettingBool_GetValue),
                        reinterpret_cast<void**>(&orig_SettingBool_GetValue));

        Memory::HookRVA(Offsets::Notification::PermissionController_Check,
                        reinterpret_cast<void*>(hook_PermCtrl_Check),
                        reinterpret_cast<void**>(&orig_PermCtrl_Check));

        Memory::HookRVA(Offsets::Notification::PermissionController_ctor,
                        reinterpret_cast<void*>(hook_PermCtrl_ctor),
                        reinterpret_cast<void**>(&orig_PermCtrl_ctor));

        Memory::HookRVA(Offsets::Notification::PermissionScreen_ctor,
                        reinterpret_cast<void*>(hook_PermScreen_ctor),
                        reinterpret_cast<void**>(&orig_PermScreen_ctor));

        LOG_INFO("[NOTIF_REWARD] Module initialized and hooks installed successfully.");
    }

    bool TriggerReward(JNIEnv *env, jobject ctx) {
        LOG_INFO("[NOTIF_REWARD] TriggerReward() invoked by user menu button.");

        bool success = false;

        // 1. Reset SettingKey.PushNotificationReward (key 23) in game memory
        if (orig_Settings_GetSetting != nullptr) {
            void *setting = orig_Settings_GetSetting(Offsets::Notification::SettingKey::PushNotificationReward);
            if (Memory::IsValidPtr(setting)) {
                s_pPushSetting = setting;
                if (orig_SettingBool_SetValue) {
                    orig_SettingBool_SetValue(setting, false);
                }
                if (orig_SettingBool_Save) {
                    orig_SettingBool_Save(setting);
                }
                LOG_INFO("[NOTIF_REWARD] Successfully reset PushNotificationReward setting (ptr=%p) to false.", setting);
                success = true;
            }
        }

        // 2. Clear PlayerPrefs push notification flags
        typedef void* (*il2cpp_string_new_t)(const char *str);
        static il2cpp_string_new_t fn_string_new = nullptr;
        if (!fn_string_new) {
            fn_string_new = reinterpret_cast<il2cpp_string_new_t>(dlsym(RTLD_DEFAULT, "il2cpp_string_new"));
        }

        if (fn_string_new != nullptr && orig_PlayerPrefs_DeleteKey != nullptr && orig_PlayerPrefs_SetInt != nullptr) {
            void *k1 = fn_string_new("SETTINGS_PUSH_REWARD");
            void *k2 = fn_string_new("SETTINGS_PUSH_REWARD_LOCAL_DISABLED");
            void *k3 = fn_string_new("settings_notification_available");

            if (k1) {
                orig_PlayerPrefs_DeleteKey(k1);
                orig_PlayerPrefs_SetInt(k1, 0);
            }
            if (k2) {
                orig_PlayerPrefs_DeleteKey(k2);
                orig_PlayerPrefs_SetInt(k2, 0);
            }
            if (k3) {
                orig_PlayerPrefs_SetInt(k3, 1);
            }
            if (orig_PlayerPrefs_Save) {
                orig_PlayerPrefs_Save();
            }
            LOG_INFO("[NOTIF_REWARD] Cleared PlayerPrefs SETTINGS_PUSH_REWARD keys.");
            success = true;
        }

        // 3. Re-invoke permission granted / reward callbacks
        bool callbackCalled = false;
        if (Memory::IsValidPtr(s_pPermController)) {
            if (orig_PermCtrl_OnGranted1) {
                orig_PermCtrl_OnGranted1(s_pPermController, true);
                callbackCalled = true;
            }
            if (orig_PermCtrl_OnGranted2) {
                orig_PermCtrl_OnGranted2(s_pPermController, true);
                callbackCalled = true;
            }
            if (orig_PermCtrl_OnResult) {
                orig_PermCtrl_OnResult(s_pPermController, true);
                callbackCalled = true;
            }
            if (orig_PermCtrl_Show) {
                orig_PermCtrl_Show(s_pPermController);
                callbackCalled = true;
            }
            LOG_INFO("[NOTIF_REWARD] Dispatched permission callbacks on PermissionController (%p).", s_pPermController);
        }

        if (Memory::IsValidPtr(s_pPermScreen)) {
            if (orig_PermScreen_OnClickYes) {
                orig_PermScreen_OnClickYes(s_pPermScreen);
                callbackCalled = true;
                LOG_INFO("[NOTIF_REWARD] Dispatched PermissionRequestScreen::OnClickYes() on (%p).", s_pPermScreen);
            }
        }

        s_claimCount++;

        // 4. User feedback
        if (env != nullptr && ctx != nullptr) {
            char toastMsg[128];
            if (callbackCalled) {
                snprintf(toastMsg, sizeof(toastMsg), "🎁 Reward Notifikasi Dipanggil (%dx)! Cek saldo / popup reward", s_claimCount.load());
            } else {
                snprintf(toastMsg, sizeof(toastMsg), "🎁 Izin Notifikasi Di-Reset (%dx)! Siap diklaim kembali", s_claimCount.load());
            }
            Toast(env, ctx, toastMsg, ToastLength::LENGTH_SHORT);
        }

        return success || callbackCalled;
    }

    int GetClaimCount() {
        return s_claimCount.load();
    }

} // namespace NotificationReward
