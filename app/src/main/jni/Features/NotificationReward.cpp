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
    static void  (*orig_PermCtrl_OnGranted1)(void *instance, bool granted) = nullptr;
    static void  (*orig_PermCtrl_OnGranted2)(void *instance, bool granted) = nullptr;
    static void  (*orig_PermCtrl_OnResult)(void *instance, bool granted) = nullptr;

    static void  (*orig_PermScreen_ctor)(void *instance) = nullptr;
    static void  (*orig_PermScreen_OnClickYes)(void *instance) = nullptr;

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
        orig_PermCtrl_OnGranted1 = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_OnGranted1)
        );
        orig_PermCtrl_OnGranted2 = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_OnGranted2)
        );
        orig_PermCtrl_OnResult = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Notification::PermissionController_OnResult)
        );

        // Resolve Permission Screen functions from dump.cs RVAs
        orig_PermScreen_OnClickYes = reinterpret_cast<void (*)(void*)>(
            Memory::GetRVA(Offsets::Notification::PermissionScreen_OnClickYes)
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

        // Install hooks to capture active Lobby instances and bypass checks
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

    // Executes on the engine/render thread (100% thread-safe within Unity context)
    static void ExecuteRewardFlow() {
        LOG_INFO("[NOTIF_REWARD] Executing reward trigger on engine thread. controller=%p, screen=%p",
                 s_pPermController, s_pPermScreen);

        // Ensure current thread is attached to Il2Cpp domain
        if (s_pfnDomainGet != nullptr && s_pfnThreadAttach != nullptr) {
            void *domain = s_pfnDomainGet();
            if (domain != nullptr) {
                s_pfnThreadAttach(domain);
            }
        }

        // 1. Reset PermissionController internal state flag (offset 0x30)
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

        // 3. Dispatch permission show and reward claim
        bool triggered = false;

        // Trigger Show dialog in lobby
        if (Memory::IsValidPtr(s_pPermController) && orig_PermCtrl_Show != nullptr) {
            orig_PermCtrl_Show(s_pPermController);
            triggered = true;
            LOG_INFO("[NOTIF_REWARD] Called PermissionController.Show (0x3F8B5E0).");
        }

        // Trigger OnClickYes to claim the reward
        if (Memory::IsValidPtr(s_pPermScreen) && orig_PermScreen_OnClickYes != nullptr) {
            orig_PermScreen_OnClickYes(s_pPermScreen);
            triggered = true;
            LOG_INFO("[NOTIF_REWARD] Called PermissionRequestScreen.OnClickYes (0x445E3E0).");
        }

        // Trigger PermissionController.OnResult(true) callback
        if (Memory::IsValidPtr(s_pPermController) && orig_PermCtrl_OnResult != nullptr) {
            orig_PermCtrl_OnResult(s_pPermController, true);
            triggered = true;
            LOG_INFO("[NOTIF_REWARD] Dispatched PermissionController.OnResult(true).");
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
