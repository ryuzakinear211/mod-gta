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

    // Atomic claim counter and trigger queue flag
    static std::atomic<int>  s_claimCount{0};
    static std::atomic<bool> s_pendingTrigger{false};

    // Captured pointers from Il2Cpp runtime (initialized when Lobby loads)
    static void *s_pPermController  = nullptr;
    static void *s_pPermScreen      = nullptr;
    static void *s_pPushPopUp       = nullptr;

    // Helper: Check if a managed Unity Object is still alive and has a valid native C++ peer
    static bool IsUnityObjectAlive(void *uObject) {
        if (!Memory::IsValidPtr(uObject)) return false;
        // In Unity IL2CPP, UnityEngine.Object has IntPtr m_CachedPtr at offset 0x10.
        // When destroyed by Unity (e.g. scene unloaded), m_CachedPtr is set to nullptr.
        void *cachedPtr = Memory::ReadField<void*>(uObject, 0x10);
        return Memory::IsValidPtr(cachedPtr);
    }

    // Il2Cpp runtime exports
    typedef void* (*il2cpp_string_new_t)(const char *str);
    typedef void* (*il2cpp_domain_get_t)();
    typedef void* (*il2cpp_thread_attach_t)(void *domain);

    static il2cpp_string_new_t     s_pfnStringNew     = nullptr;
    static il2cpp_domain_get_t     s_pfnDomainGet     = nullptr;
    static il2cpp_thread_attach_t s_pfnThreadAttach  = nullptr;

    // Original function pointers
    static void  (*orig_PermCtrl_ctor)(void *instance, void *screen) = nullptr;
    static bool  (*orig_PermCtrl_Check)(void *instance) = nullptr;
    static void  (*orig_PermCtrl_Show)(void *instance) = nullptr;

    static void  (*orig_PermScreen_ctor)(void *instance) = nullptr;

    static void  (*orig_PushPopUp_ctor)(void *instance) = nullptr;
    static void  (*orig_PushPopUp_AcceptEnablePush)(void *instance) = nullptr;
    static void  (*orig_PushPopUp_ClosePopUp)(void *instance) = nullptr;

    static void  (*orig_PlayerPrefs_SetInt)(void *key, int val) = nullptr;
    static void  (*orig_PlayerPrefs_DeleteKey)(void *key) = nullptr;
    static void  (*orig_PlayerPrefs_Save)() = nullptr;

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

    // Hook: PushPopUpWindowUi..ctor (0x4E07DB0)
    static void hook_PushPopUp_ctor(void *instance) {
        s_pPushPopUp = instance;
        LOG_INFO("[NOTIF_REWARD] Hooked PushPopUpWindowUi ctor: popup=%p", instance);
        if (orig_PushPopUp_ctor) orig_PushPopUp_ctor(instance);
    }

    // Hook: PushPopUpWindowUi.ClosePopUp (0x4E08B24)
    static void hook_PushPopUp_ClosePopUp(void *instance) {
        if (instance == s_pPushPopUp) {
            s_pPushPopUp = nullptr;
            LOG_INFO("[NOTIF_REWARD] PushPopUpWindowUi closed: popup=%p", instance);
        }
        if (orig_PushPopUp_ClosePopUp) orig_PushPopUp_ClosePopUp(instance);
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
        LOG_INFO("[NOTIF_REWARD] Initializing Notification Permission Reward module...");

        ResolveIl2CppExports();

        // Resolve Permission Controller functions from dump.cs RVAs
        orig_PermCtrl_Show = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_Show)
        );

        // Resolve Lobby PushPopUpWindowUi functions from dump.cs RVAs
        orig_PushPopUp_AcceptEnablePush = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PushPopUpWindowUi_AcceptEnable)
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

        // Install hooks to capture active Lobby/Startup instances and bypass checks
        Memory::HookRVA(Offsets::Notification::PermissionController_Check,
                        reinterpret_cast<void*>(hook_PermCtrl_Check),
                        reinterpret_cast<void**>(&orig_PermCtrl_Check));

        Memory::HookRVA(Offsets::Notification::PermissionController_ctor,
                        reinterpret_cast<void*>(hook_PermCtrl_ctor),
                        reinterpret_cast<void**>(&orig_PermCtrl_ctor));

        Memory::HookRVA(Offsets::Notification::PermissionScreen_ctor,
                        reinterpret_cast<void*>(hook_PermScreen_ctor),
                        reinterpret_cast<void**>(&orig_PermScreen_ctor));

        Memory::HookRVA(Offsets::Notification::PushPopUpWindowUi_ctor,
                        reinterpret_cast<void*>(hook_PushPopUp_ctor),
                        reinterpret_cast<void**>(&orig_PushPopUp_ctor));

        Memory::HookRVA(Offsets::Notification::PushPopUpWindowUi_ClosePopUp,
                        reinterpret_cast<void*>(hook_PushPopUp_ClosePopUp),
                        reinterpret_cast<void**>(&orig_PushPopUp_ClosePopUp));

        LOG_INFO("[NOTIF_REWARD] Module initialized and hooks installed successfully.");
    }

    // Executes on the engine/render thread (100% thread-safe within Unity context)
    static void ExecuteRewardFlow() {
        LOG_INFO("[NOTIF_REWARD] Executing reward trigger on engine thread. controller=%p, screen=%p, popup=%p",
                 s_pPermController, s_pPermScreen, s_pPushPopUp);

        // Ensure current thread is attached to Il2Cpp domain
        if (s_pfnDomainGet != nullptr && s_pfnThreadAttach != nullptr) {
            void *domain = s_pfnDomainGet();
            if (domain != nullptr) {
                s_pfnThreadAttach(domain);
            }
        }

        // 1. Reset PermissionController internal state flag (offset 0x30) safely if pointer is valid
        if (Memory::IsValidPtr(s_pPermController)) {
            *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(s_pPermController) + 0x30) = false;
            LOG_INFO("[NOTIF_REWARD] Reset controller internal flag at 0x30 to false.");
        }

        // 2. Clear PlayerPrefs push notification flags safely
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

        // 3. Dispatch permission show or reward claim safely (Never access destroyed Unity objects)
        bool triggered = false;

        // A. If active Lobby PushPopUpWindowUi is open, trigger AcceptEnablePush directly
        if (IsUnityObjectAlive(s_pPushPopUp) && orig_PushPopUp_AcceptEnablePush != nullptr) {
            orig_PushPopUp_AcceptEnablePush(s_pPushPopUp);
            triggered = true;
            LOG_INFO("[NOTIF_REWARD] Called PushPopUpWindowUi.AcceptEnablePush (0x4E080A0).");
        }
        // B. Otherwise, if preloader permission screen is alive, safely show dialog
        else if (IsUnityObjectAlive(s_pPermScreen) && Memory::IsValidPtr(s_pPermController) && orig_PermCtrl_Show != nullptr) {
            orig_PermCtrl_Show(s_pPermController);
            triggered = true;
            LOG_INFO("[NOTIF_REWARD] Called PermissionController.Show (0x3F8B5E0).");
        }
        else {
            LOG_INFO("[NOTIF_REWARD] Notification keys successfully reset to uncompleted. Ready for claim.");
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
            Toast(env, ctx, OBFUSCATE("🎁 Klaim Reward Notifikasi Diproses!"), ToastLength::LENGTH_SHORT);
        }

        return true;
    }

    int GetClaimCount() {
        return s_claimCount.load();
    }

} // namespace NotificationReward
