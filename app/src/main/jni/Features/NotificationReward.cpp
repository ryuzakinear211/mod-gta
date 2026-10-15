#include "NotificationReward.h"
#include "../Includes/obfuscate.h"
#include "../Menu/Setup.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"

#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace NotificationReward {

    // Atomic claim counter and trigger queue flag
    static std::atomic<int>  s_claimCount{0};
    static std::atomic<bool> s_pendingTrigger{false};

    // Captured active PushPopUpWindowUi instance
    static void *s_pPushPopUp = nullptr;

    // Helper: Check if a managed Unity Object is alive with a valid native C++ peer
    static bool IsUnityObjectAlive(void *uObject) {
        if (!Memory::IsValidPtr(uObject)) return false;
        // In Unity IL2CPP, UnityEngine.Object has IntPtr m_CachedPtr at offset 0x10.
        // When destroyed by Unity (e.g. scene unloaded), m_CachedPtr is set to nullptr.
        void *cachedPtr = Memory::ReadField<void*>(uObject, Offsets::Unity::Transform_m_CachedPtr);
        return Memory::IsValidPtr(cachedPtr);
    }

    // Unity native engine helpers
    static void UnitySetActive(void *go, bool active) {
        if (!Memory::IsValidPtr(go)) return;
        static auto pfnSetActive = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Unity::GameObject_SetActive)
        );
        if (pfnSetActive) pfnSetActive(go, active);
    }

    static void* UnityGetGameObject(void *comp) {
        if (!Memory::IsValidPtr(comp)) return nullptr;
        static auto pfnGetGo = reinterpret_cast<void* (*)(void*)>(
            Memory::GetRVA(Offsets::Unity::Component_GetGameObject)
        );
        return pfnGetGo ? pfnGetGo(comp) : nullptr;
    }

    static bool UnityIsActiveAndEnabled(void *comp) {
        if (!Memory::IsValidPtr(comp)) return false;
        static auto pfnIsActive = reinterpret_cast<bool (*)(void*)>(
            Memory::GetRVA(Offsets::Unity::BehaviourGetIsActive)
        );
        return pfnIsActive ? pfnIsActive(comp) : false;
    }

    // Il2Cpp runtime exports
    typedef void* (*il2cpp_string_new_t)(const char *str);
    typedef void* (*il2cpp_domain_get_t)();
    typedef void* (*il2cpp_thread_attach_t)(void *domain);

    static il2cpp_string_new_t     s_pfnStringNew    = nullptr;
    static il2cpp_domain_get_t     s_pfnDomainGet    = nullptr;
    static il2cpp_thread_attach_t s_pfnThreadAttach = nullptr;

    // Original function pointers for PushPopUpWindowUi
    static void (*orig_PushPopUp_ctor)(void *instance) = nullptr;
    static void (*orig_PushPopUp_Awake)(void *instance) = nullptr;
    static void (*orig_PushPopUp_OnShow)(void *instance, int transitionType) = nullptr;
    static void (*orig_PushPopUp_ShowPopup)(void *instance) = nullptr;
    static void (*orig_PushPopUp_AcceptEnablePush)(void *instance) = nullptr;
    static void (*orig_PushPopUp_ClosePopUp)(void *instance) = nullptr;
    static void (*orig_PushPopUp_OnGranted1)(void *instance, bool isGranted) = nullptr;
    static void (*orig_PushPopUp_OnGranted2)(void *instance, bool isGranted) = nullptr;

    // WindowsUi instance getter
    static void* (*orig_WindowsUi_GetInstance)() = nullptr;

    // Settings Provider & Reactive Settings
    static void* (*orig_Settings_GetSetting)(int key) = nullptr;
    static void  (*orig_SettingBool_SetValue)(void *inst, bool val) = nullptr;
    static void  (*orig_SettingBool_Save)(void *inst) = nullptr;

    // PlayerPrefs functions
    static void (*orig_PlayerPrefs_SetInt)(void *key, int val) = nullptr;
    static void (*orig_PlayerPrefs_DeleteKey)(void *key) = nullptr;
    static void (*orig_PlayerPrefs_Save)() = nullptr;

    // PermissionController.Check (0x3F89344)
    static bool (*orig_PermCtrl_Check)(void *instance) = nullptr;

    static bool hook_PermCtrl_Check(void *instance) {
        return true;
    }

    // Grants the push notification reward directly to the player without opening Android Settings
    static bool ClaimPushReward(void *popup) {
        if (!IsUnityObjectAlive(popup)) return false;

        LOG_INFO("[NOTIF_REWARD] Granting push notification reward directly on popup=%p", popup);
        bool claimed = false;

        // Call the internal permission granted callbacks with true
        if (orig_PushPopUp_OnGranted1 != nullptr) {
            orig_PushPopUp_OnGranted1(popup, true);
            claimed = true;
            LOG_INFO("[NOTIF_REWARD] Called PushPopUpWindowUi.OnGranted1 callback (0x4E083E4)");
        }
        if (orig_PushPopUp_OnGranted2 != nullptr) {
            orig_PushPopUp_OnGranted2(popup, true);
            claimed = true;
            LOG_INFO("[NOTIF_REWARD] Called PushPopUpWindowUi.OnGranted2 callback (0x4E08680)");
        }

        return claimed;
    }

    // Hook: PushPopUpWindowUi.AcceptEnablePush (0x4E080A0)
    // Intercepts the "YA" button on the UI reward dialog.
    // Instead of launching Android OS notification settings, this directly awards the reward in-game!
    static void hook_PushPopUp_AcceptEnable(void *instance) {
        s_pPushPopUp = instance;
        LOG_INFO("[NOTIF_REWARD] Intercepted PushPopUpWindowUi.AcceptEnablePush: popup=%p. Claiming reward in-game (No Settings redirect).", instance);

        bool claimed = ClaimPushReward(instance);
        if (!claimed && orig_PushPopUp_AcceptEnablePush != nullptr) {
            orig_PushPopUp_AcceptEnablePush(instance);
        }
    }

    // Hook: PushPopUpWindowUi..ctor (0x4E07DB0)
    static void hook_PushPopUp_ctor(void *instance) {
        s_pPushPopUp = instance;
        LOG_INFO("[NOTIF_REWARD] Hooked PushPopUpWindowUi ctor: popup=%p", instance);
        if (orig_PushPopUp_ctor) orig_PushPopUp_ctor(instance);
    }

    // Hook: PushPopUpWindowUi.bbbbabaabbabbabbaaaaababaababbababbabaaabaaabaa (Slot 11: Awake, 0x4E073E4)
    static void hook_PushPopUp_Awake(void *instance) {
        s_pPushPopUp = instance;
        LOG_INFO("[NOTIF_REWARD] Hooked PushPopUpWindowUi Awake: popup=%p", instance);
        if (orig_PushPopUp_Awake) orig_PushPopUp_Awake(instance);
    }

    // Hook: PushPopUpWindowUi.aabbababbaabaaabbbabaaaaaabbbbabbbabbbaaabaaaab (Slot 13: OnShow, 0x4E08A68)
    static void hook_PushPopUp_OnShow(void *instance, int transitionType) {
        s_pPushPopUp = instance;
        LOG_INFO("[NOTIF_REWARD] Hooked PushPopUpWindowUi OnShow: popup=%p, type=%d", instance, transitionType);
        if (orig_PushPopUp_OnShow) orig_PushPopUp_OnShow(instance, transitionType);
    }

    // Hook: PushPopUpWindowUi.ClosePopUp (0x4E08B24)
    static void hook_PushPopUp_ClosePopUp(void *instance) {
        // Keep s_pPushPopUp pointer so we can re-open it if still alive in scene hierarchy
        LOG_INFO("[NOTIF_REWARD] PushPopUpWindowUi closed: popup=%p (retained for re-claim)", instance);
        if (orig_PushPopUp_ClosePopUp) orig_PushPopUp_ClosePopUp(instance);
    }

    // Dynamically searches WindowsUi.Instance's list of registered windows for PushPopUpWindowUi
    static void* FindPushPopUpInstance() {
        if (orig_WindowsUi_GetInstance == nullptr) return nullptr;
        void *winUi = orig_WindowsUi_GetInstance();
        if (!Memory::IsValidPtr(winUi)) return nullptr;

        void *winList = Memory::ReadField<void*>(winUi, Offsets::Notification::WindowsUi_ListOffset);
        if (!Memory::IsValidPtr(winList)) return nullptr;

        void *itemsArray = Memory::ReadField<void*>(winList, 0x10);
        int count = Memory::ReadField<int>(winList, 0x18);
        if (!Memory::IsValidPtr(itemsArray) || count <= 0 || count > 200) return nullptr;

        for (int i = 0; i < count; i++) {
            void *win = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(itemsArray) + 0x20 + (i * sizeof(void*))
            );
            if (IsUnityObjectAlive(win)) {
                void *klass = *reinterpret_cast<void**>(win);
                if (Memory::IsValidPtr(klass)) {
                    // In IL2CPP ARM64, klass->name is at offset 0x10
                    const char *className = *reinterpret_cast<const char**>(
                        reinterpret_cast<uintptr_t>(klass) + 0x10
                    );
                    if (className && strcmp(className, "PushPopUpWindowUi") == 0) {
                        LOG_INFO("[NOTIF_REWARD] Located PushPopUpWindowUi in WindowsUi list: %p", win);
                        return win;
                    }
                }
            }
        }
        return nullptr;
    }

    // Displays the UI Reward popup window (with female soldier and gold reward box) on screen
    static bool ShowPushPopUpWindow(void *popup) {
        if (!IsUnityObjectAlive(popup)) return false;

        // 1. Activate the window's main GameObject
        void *winGo = UnityGetGameObject(popup);
        if (Memory::IsValidPtr(winGo)) {
            UnitySetActive(winGo, true);
        }

        // 2. Activate _firstPopUp (offset 0x28) - dialog with female soldier, title, and buttons
        void *firstPopUp = Memory::ReadField<void*>(popup, Offsets::Notification::PushPopUp_FirstPopUp);
        if (Memory::IsValidPtr(firstPopUp)) {
            UnitySetActive(firstPopUp, true);
        }

        // 3. Activate _anyPopUp (offset 0x30) if _firstPopUp is null
        void *anyPopUp = Memory::ReadField<void*>(popup, Offsets::Notification::PushPopUp_AnyPopUp);
        if (!Memory::IsValidPtr(firstPopUp) && Memory::IsValidPtr(anyPopUp)) {
            UnitySetActive(anyPopUp, true);
        }

        // 4. Invoke show/open method on the window if available
        if (orig_PushPopUp_ShowPopup != nullptr) {
            orig_PushPopUp_ShowPopup(popup);
        } else if (orig_PushPopUp_OnShow != nullptr) {
            orig_PushPopUp_OnShow(popup, 0);
        }

        LOG_INFO("[NOTIF_REWARD] Displayed UI Reward window: popup=%p, winGo=%p, firstPopUp=%p",
                 popup, winGo, firstPopUp);
        return true;
    }

    // Resolves Il2Cpp API exports safely
    static void ResolveIl2CppExports() {
        void *hLib = dlopen("libil2cpp.so", RTLD_NOLOAD);
        if (!hLib) hLib = dlopen("libil2cpp.so", RTLD_NOW);
        if (hLib) {
            s_pfnStringNew = reinterpret_cast<il2cpp_string_new_t>(dlsym(hLib, "il2cpp_string_new"));
            s_pfnDomainGet = reinterpret_cast<il2cpp_domain_get_t>(dlsym(hLib, "il2cpp_domain_get"));
            s_pfnThreadAttach = reinterpret_cast<il2cpp_thread_attach_t>(dlsym(hLib, "il2cpp_thread_attach"));
            LOG_INFO("[NOTIF_REWARD] Resolved libil2cpp exports: string_new=%p, domain_get=%p, thread_attach=%p",
                     s_pfnStringNew, s_pfnDomainGet, s_pfnThreadAttach);
        }
    }

    void Init() {
        LOG_INFO("[NOTIF_REWARD] Initializing Notification Reward module (UI Reward Caller)...");

        ResolveIl2CppExports();

        // Resolve PushPopUpWindowUi functions from dump.cs RVAs
        orig_PushPopUp_AcceptEnablePush = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PushPopUpWindowUi_AcceptEnable)
        );
        orig_PushPopUp_ShowPopup = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PushPopUpWindowUi_ShowPopup)
        );
        orig_PushPopUp_OnShow = reinterpret_cast<void (*)(void*, int)>(
            Memory::GetRVA(Offsets::Notification::PushPopUpWindowUi_OnShow)
        );
        orig_PushPopUp_OnGranted1 = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PushPopUpWindowUi_OnGranted1)
        );
        orig_PushPopUp_OnGranted2 = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PushPopUpWindowUi_OnGranted2)
        );

        // Resolve WindowsUi instance getter from dump.cs RVAs
        orig_WindowsUi_GetInstance = reinterpret_cast<void* (*)()>(
            Memory::GetRVA(Offsets::Notification::WindowsUi_GetInstance)
        );

        // Resolve Settings Provider & Reactive Settings
        orig_Settings_GetSetting = reinterpret_cast<void* (*)(int)>(
            Memory::GetRVA(Offsets::Notification::Settings_GetSetting)
        );
        orig_SettingBool_SetValue = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::SettingBool_SetValue)
        );
        orig_SettingBool_Save = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::SettingBool_Save)
        );

        // Resolve PlayerPrefs functions from dump.cs RVAs
        orig_PlayerPrefs_SetInt = reinterpret_cast<void (*)(void*, int)>(
            Memory::GetRVA(Offsets::Notification::PlayerPrefs_SetInt)
        );
        orig_PlayerPrefs_DeleteKey = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PlayerPrefs_DeleteKey)
        );
        orig_PlayerPrefs_Save = reinterpret_cast<void (*)()>(
            Memory::GetRVA(Offsets::Notification::PlayerPrefs_Save)
        );

        // Install hooks for PushPopUpWindowUi lifecycle and button interception
        Memory::HookRVA(Offsets::Notification::PushPopUpWindowUi_ctor,
                        reinterpret_cast<void*>(hook_PushPopUp_ctor),
                        reinterpret_cast<void**>(&orig_PushPopUp_ctor));

        Memory::HookRVA(Offsets::Notification::PushPopUpWindowUi_Awake,
                        reinterpret_cast<void*>(hook_PushPopUp_Awake),
                        reinterpret_cast<void**>(&orig_PushPopUp_Awake));

        Memory::HookRVA(Offsets::Notification::PushPopUpWindowUi_OnShow,
                        reinterpret_cast<void*>(hook_PushPopUp_OnShow),
                        reinterpret_cast<void**>(&orig_PushPopUp_OnShow));

        Memory::HookRVA(Offsets::Notification::PushPopUpWindowUi_AcceptEnable,
                        reinterpret_cast<void*>(hook_PushPopUp_AcceptEnable),
                        reinterpret_cast<void**>(&orig_PushPopUp_AcceptEnablePush));

        Memory::HookRVA(Offsets::Notification::PushPopUpWindowUi_ClosePopUp,
                        reinterpret_cast<void*>(hook_PushPopUp_ClosePopUp),
                        reinterpret_cast<void**>(&orig_PushPopUp_ClosePopUp));

        // Keep PermissionController.Check true so eligible check passes
        Memory::HookRVA(Offsets::Notification::PermissionController_Check,
                        reinterpret_cast<void*>(hook_PermCtrl_Check),
                        reinterpret_cast<void**>(&orig_PermCtrl_Check));

        LOG_INFO("[NOTIF_REWARD] Module initialized: UI Reward Caller & Direct Claim hooks installed successfully.");
    }

    // Executes on the engine/render thread (100% thread-safe within Unity context)
    static void ExecuteRewardFlow() {
        LOG_INFO("[NOTIF_REWARD] Executing reward trigger on engine thread. cached_popup=%p", s_pPushPopUp);

        // Ensure current thread is attached to Il2Cpp domain
        if (s_pfnDomainGet != nullptr && s_pfnThreadAttach != nullptr) {
            void *domain = s_pfnDomainGet();
            if (domain != nullptr) {
                s_pfnThreadAttach(domain);
            }
        }

        // 1. Clear PlayerPrefs push notification flags safely
        if (s_pfnStringNew != nullptr && orig_PlayerPrefs_DeleteKey != nullptr && orig_PlayerPrefs_SetInt != nullptr) {
            void *k1 = s_pfnStringNew("SETTINGS_PUSH_REWARD");
            void *k2 = s_pfnStringNew("SETTINGS_PUSH_REWARD_LOCAL_DISABLED");
            void *k3 = s_pfnStringNew("settings_notification_available");

            if (k1 != nullptr) {
                orig_PlayerPrefs_DeleteKey(k1);
                orig_PlayerPrefs_SetInt(k1, 0);
            }
            if (k2 != nullptr) {
                orig_PlayerPrefs_DeleteKey(k2);
                orig_PlayerPrefs_SetInt(k2, 0);
            }
            if (k3 != nullptr) {
                orig_PlayerPrefs_SetInt(k3, 1);
            }
            if (orig_PlayerPrefs_Save != nullptr) {
                orig_PlayerPrefs_Save();
            }
            LOG_INFO("[NOTIF_REWARD] Reset PlayerPrefs push notification keys.");
        }

        // 2. Reset reactive game setting for PushNotificationReward (key 23)
        if (orig_Settings_GetSetting != nullptr) {
            void *setting = orig_Settings_GetSetting(Offsets::Notification::SettingKey::PushNotificationReward);
            if (Memory::IsValidPtr(setting)) {
                if (orig_SettingBool_SetValue != nullptr) {
                    orig_SettingBool_SetValue(setting, false);
                }
                if (orig_SettingBool_Save != nullptr) {
                    orig_SettingBool_Save(setting);
                }
                LOG_INFO("[NOTIF_REWARD] Reset reactive setting PushNotificationReward to false.");
            }
        }

        // 3. Resolve PushPopUpWindowUi instance if not already cached
        if (!IsUnityObjectAlive(s_pPushPopUp)) {
            s_pPushPopUp = FindPushPopUpInstance();
        }

        // 4. Show UI Reward dialog or claim directly if already open on screen
        bool triggered = false;
        if (IsUnityObjectAlive(s_pPushPopUp)) {
            if (UnityIsActiveAndEnabled(s_pPushPopUp)) {
                // If the UI is already open and visible on screen, directly claim the reward!
                triggered = ClaimPushReward(s_pPushPopUp);
                LOG_INFO("[NOTIF_REWARD] Popup was already open. Claimed reward directly: success=%d", (int)triggered);
            } else {
                // If the UI is not visible yet, call and display the UI reward window!
                triggered = ShowPushPopUpWindow(s_pPushPopUp);
                LOG_INFO("[NOTIF_REWARD] UI Reward window called and shown on screen: success=%d", (int)triggered);
            }
        } else {
            LOG_INFO("[NOTIF_REWARD] Notification keys successfully reset to uncompleted. PushPopUpWindowUi will auto-open in Lobby.");
        }

        s_claimCount++;
        LOG_INFO("[NOTIF_REWARD] Reward flow completed successfully (total claims=%d, triggered=%d).",
                 s_claimCount.load(), (int)triggered);
    }

    void OnRenderFrame() {
        if (!s_pendingTrigger.load(std::memory_order_relaxed)) {
            return;
        }

        if (s_pendingTrigger.exchange(false, std::memory_order_acquire)) {
            ExecuteRewardFlow();
        }
    }

    bool TriggerReward(JNIEnv *env, jobject ctx) {
        LOG_INFO("[NOTIF_REWARD] TriggerReward() button clicked in menu. Queueing to engine thread...");

        // Safely queue execution to the engine thread to avoid cross-thread crash
        s_pendingTrigger.store(true, std::memory_order_release);

        if (env != nullptr && ctx != nullptr) {
            Toast(env, ctx, OBFUSCATE("🎁 UI Reward Notifikasi Dipanggil!"), ToastLength::LENGTH_SHORT);
        }

        return true;
    }

    int GetClaimCount() {
        return s_claimCount.load();
    }

} // namespace NotificationReward
