#include "ResetRewardTime.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"

#include <atomic>
#include <cstdint>

namespace ResetRewardTime {

    static std::atomic<bool> s_enabled{false};

    static void (*GameObject_SetActive)(void *go, bool active) = nullptr;
    static void (*Selectable_SetInteractable)(void *btn, bool interactable) = nullptr;

    // Helper: Enforce interactive Available state on FreeDailyRewardItemView
    static void ForceItemViewReady(void *viewInstance) {
        if (!viewInstance || !Memory::IsValidPtr(viewInstance)) return;

        if (!GameObject_SetActive) {
            GameObject_SetActive = reinterpret_cast<void(*)(void*, bool)>(Memory::GetRVA(Offsets::Unity::GameObject_SetActive));
        }
        if (!Selectable_SetInteractable) {
            Selectable_SetInteractable = reinterpret_cast<void(*)(void*, bool)>(Memory::GetRVA(Offsets::Unity::Selectable_SetInteractable));
        }

        // Hide unavailable countdown overlay panel
        void *unavailablePanel = Memory::ReadField<void*>(viewInstance, Offsets::FreeReward::View_UnavailablePanel);
        if (unavailablePanel && Memory::IsValidPtr(unavailablePanel) && GameObject_SetActive) {
            GameObject_SetActive(unavailablePanel, false);
        }

        // Enable claim button for immediate interaction
        void *claimButton = Memory::ReadField<void*>(viewInstance, Offsets::FreeReward::View_ClaimButton);
        if (claimButton && Memory::IsValidPtr(claimButton) && Selectable_SetInteractable) {
            Selectable_SetInteractable(claimButton, true);
        }
    }

    // --------------------------------------------------------------------------
    // 1. FreeDailyReward Presenter Hooks (baaaaababbababababbbabaabbbbbbbbbbaaabbbabbbbbb)
    // --------------------------------------------------------------------------

    // CanTakeReward() -> bool: RVA 0x4849B84 (68 bytes)
    static bool (*orig_Presenter_CanTake)(void *instance) = nullptr;
    static bool hook_Presenter_CanTake(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Presenter_CanTake ? orig_Presenter_CanTake(instance) : true;
    }

    // GetRemainingSeconds() -> int: RVA 0x4849C80 (124 bytes)
    static int (*orig_Presenter_GetRemainSec)(void *instance) = nullptr;
    static int hook_Presenter_GetRemainSec(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return 0; // 0 seconds remaining -> ready to claim immediately
        }
        return orig_Presenter_GetRemainSec ? orig_Presenter_GetRemainSec(instance) : 0;
    }

    // GetCooldownSeconds() -> int: RVA 0x484A2EC (124 bytes)
    static int (*orig_Presenter_GetCooldown)(void *instance) = nullptr;
    static int hook_Presenter_GetCooldown(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return 0;
        }
        return orig_Presenter_GetCooldown ? orig_Presenter_GetCooldown(instance) : 0;
    }

    // GetTimer2() -> int: RVA 0x484A1B8 (120 bytes)
    static int (*orig_Presenter_GetTimer2)(void *instance) = nullptr;
    static int hook_Presenter_GetTimer2(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return 0;
        }
        return orig_Presenter_GetTimer2 ? orig_Presenter_GetTimer2(instance) : 0;
    }

    // IsAvailable() -> bool: RVA 0x484A374 (68 bytes)
    static bool (*orig_Presenter_IsAvailable)(void *instance) = nullptr;
    static bool hook_Presenter_IsAvailable(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Presenter_IsAvailable ? orig_Presenter_IsAvailable(instance) : true;
    }

    // TakeReward() -> void: RVA 0x48499B4 (260 bytes)
    static void (*orig_Presenter_TakeReward)(void *instance) = nullptr;
    static void hook_Presenter_TakeReward(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed) && instance && Memory::IsValidPtr(instance)) {
            LOG_INFO("[FREE_REWARD] TakeReward called! Resetting presenter internal fields (0x50=0, 0x60=true)...");
            Memory::WriteField<int>(instance, 0x50, 0);
            Memory::WriteField<bool>(instance, 0x60, true);
        }
        if (orig_Presenter_TakeReward) {
            orig_Presenter_TakeReward(instance);
        }
    }

    // --------------------------------------------------------------------------
    // 2. FreeDailyReward Service / Model Hooks (aaaaaababaaaababaaababaabbbabbbabbaaabbaababaab)
    // --------------------------------------------------------------------------

    // Service_CanClaim() -> bool: RVA 0x4C7BE98 (288 bytes)
    static bool (*orig_Service_CanClaim)(void *instance) = nullptr;
    static bool hook_Service_CanClaim(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Service_CanClaim ? orig_Service_CanClaim(instance) : true;
    }

    // Service_IsAvailable() -> bool: RVA 0x4C7C05C (288 bytes)
    static bool (*orig_Service_IsAvailable)(void *instance) = nullptr;
    static bool hook_Service_IsAvailable(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Service_IsAvailable ? orig_Service_IsAvailable(instance) : true;
    }

    // --------------------------------------------------------------------------
    // 3. FreeDailyRewardItemView UI Hooks (Dom.FreeDailyReward.View.FreeDailyRewardItemView)
    // --------------------------------------------------------------------------

    // SetAvailableState(bool isAvailable): RVA 0x43F1400 (32 bytes)
    static void (*orig_View_SetAvailableState)(void *instance, bool isAvailable) = nullptr;
    static void hook_View_SetAvailableState(void *instance, bool isAvailable) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            isAvailable = true;
        }
        if (orig_View_SetAvailableState) {
            orig_View_SetAvailableState(instance, isAvailable);
        }
        if (s_enabled.load(std::memory_order_relaxed)) {
            ForceItemViewReady(instance);
        }
    }

    // UpdateState1(): RVA 0x43F15DC (448 bytes)
    static void (*orig_View_UpdateState1)(void *instance) = nullptr;
    static void hook_View_UpdateState1(void *instance) {
        if (orig_View_UpdateState1) {
            orig_View_UpdateState1(instance);
        }
        if (s_enabled.load(std::memory_order_relaxed)) {
            ForceItemViewReady(instance);
        }
    }

    // OnEnable(): RVA 0x43F1E94 (728 bytes)
    static void (*orig_View_OnEnable)(void *instance) = nullptr;
    static void hook_View_OnEnable(void *instance) {
        if (orig_View_OnEnable) {
            orig_View_OnEnable(instance);
        }
        if (s_enabled.load(std::memory_order_relaxed)) {
            ForceItemViewReady(instance);
        }
    }

    void Init() {
        LOG_INFO("[FREE_REWARD] Installing robust Reset Reward Time hooks (UI View + Presenter + Service)...");

        // Presenter Hooks (All body lengths >= 68 bytes)
        Memory::HookRVA(Offsets::FreeReward::Presenter_CanTake,
                        reinterpret_cast<void*>(hook_Presenter_CanTake),
                        reinterpret_cast<void**>(&orig_Presenter_CanTake));

        Memory::HookRVA(Offsets::FreeReward::Presenter_GetRemainSec,
                        reinterpret_cast<void*>(hook_Presenter_GetRemainSec),
                        reinterpret_cast<void**>(&orig_Presenter_GetRemainSec));

        Memory::HookRVA(Offsets::FreeReward::Presenter_GetCooldown,
                        reinterpret_cast<void*>(hook_Presenter_GetCooldown),
                        reinterpret_cast<void**>(&orig_Presenter_GetCooldown));

        Memory::HookRVA(Offsets::FreeReward::Presenter_GetTimer2,
                        reinterpret_cast<void*>(hook_Presenter_GetTimer2),
                        reinterpret_cast<void**>(&orig_Presenter_GetTimer2));

        Memory::HookRVA(Offsets::FreeReward::Presenter_IsAvailable,
                        reinterpret_cast<void*>(hook_Presenter_IsAvailable),
                        reinterpret_cast<void**>(&orig_Presenter_IsAvailable));

        Memory::HookRVA(Offsets::FreeReward::Presenter_TakeReward,
                        reinterpret_cast<void*>(hook_Presenter_TakeReward),
                        reinterpret_cast<void**>(&orig_Presenter_TakeReward));

        // Service / Model Hooks (Body lengths = 288 bytes)
        Memory::HookRVA(Offsets::FreeReward::Service_CanClaim,
                        reinterpret_cast<void*>(hook_Service_CanClaim),
                        reinterpret_cast<void**>(&orig_Service_CanClaim));

        Memory::HookRVA(Offsets::FreeReward::Service_IsAvailable,
                        reinterpret_cast<void*>(hook_Service_IsAvailable),
                        reinterpret_cast<void**>(&orig_Service_IsAvailable));

        // UI View Hooks (Body lengths: 32 bytes, 448 bytes, 728 bytes)
        Memory::HookRVA(Offsets::FreeReward::View_SetAvailableState,
                        reinterpret_cast<void*>(hook_View_SetAvailableState),
                        reinterpret_cast<void**>(&orig_View_SetAvailableState));

        Memory::HookRVA(Offsets::FreeReward::View_UpdateState1,
                        reinterpret_cast<void*>(hook_View_UpdateState1),
                        reinterpret_cast<void**>(&orig_View_UpdateState1));

        Memory::HookRVA(Offsets::FreeReward::View_OnEnable,
                        reinterpret_cast<void*>(hook_View_OnEnable),
                        reinterpret_cast<void**>(&orig_View_OnEnable));

        LOG_INFO("[FREE_REWARD] All robust Reset Reward Time hooks installed successfully.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[FREE_REWARD] Reset Reward Time (1h 57m): %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace ResetRewardTime
