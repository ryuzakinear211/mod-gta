#include "ResetRewardTime.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"

#include <atomic>
#include <cstdint>

namespace ResetRewardTime {

    static std::atomic<bool> s_enabled{false};

    // --------------------------------------------------------------------------
    // Unity Engine helpers
    // --------------------------------------------------------------------------
    static void UnitySetActive(void *go, bool active) {
        if (!Memory::IsValidPtr(go)) return;
        static auto pfnSetActive = reinterpret_cast<void (*)(void*, bool)>(
            Memory::GetRVA(Offsets::Unity::GameObject_SetActive)
        );
        if (pfnSetActive) pfnSetActive(go, active);
    }

    // --------------------------------------------------------------------------
    // 1. FreeDailyRewardItemView Hooks (View state & countdown timer)
    // --------------------------------------------------------------------------

    // SetAvailable(bool available): RVA 0x43F1400
    static void (*orig_View_SetAvailable)(void *instance, bool available) = nullptr;
    static void hook_View_SetAvailable(void *instance, bool available) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            available = true; // Force available state
        }
        if (orig_View_SetAvailable) {
            orig_View_SetAvailable(instance, available);
        }
    }

    // UpdateTimer(long seconds): RVA 0x43F1288
    static void (*orig_View_UpdateTimer)(void *instance, int64_t seconds) = nullptr;
    static void hook_View_UpdateTimer(void *instance, int64_t seconds) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            seconds = 0; // 0 seconds remaining (ready to claim)
        }
        if (orig_View_UpdateTimer) {
            orig_View_UpdateTimer(instance, seconds);
        }
    }

    // OnTimerTick(long seconds): RVA 0x43F24AC
    static void (*orig_View_OnTimerTick)(void *instance, int64_t seconds) = nullptr;
    static void hook_View_OnTimerTick(void *instance, int64_t seconds) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            seconds = 0;
        }
        if (orig_View_OnTimerTick) {
            orig_View_OnTimerTick(instance, seconds);
        }
    }

    // OnEnable(): RVA 0x43F1E94
    static void (*orig_View_OnEnable)(void *instance) = nullptr;
    static void hook_View_OnEnable(void *instance) {
        if (orig_View_OnEnable) {
            orig_View_OnEnable(instance);
        }
        if (instance && s_enabled.load(std::memory_order_relaxed)) {
            // Force available state
            if (orig_View_SetAvailable) {
                orig_View_SetAvailable(instance, true);
            }
            if (orig_View_UpdateTimer) {
                orig_View_UpdateTimer(instance, 0);
            }
            // Directly hide unavailable panel (offset 0x58: GameObject _unavailablePanel)
            void *unavailablePanel = Memory::ReadField<void*>(instance, 0x58);
            if (unavailablePanel) {
                UnitySetActive(unavailablePanel, false);
            }
        }
    }

    // --------------------------------------------------------------------------
    // 2. FreeDailyReward Presenter Hooks (Business logic & countdown provider)
    // --------------------------------------------------------------------------

    // CanTakeReward() -> bool: RVA 0x4849B84
    static bool (*orig_Presenter_CanTake)(void *instance) = nullptr;
    static bool hook_Presenter_CanTake(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Presenter_CanTake ? orig_Presenter_CanTake(instance) : true;
    }

    // GetRemainingSeconds() -> int: RVA 0x4849C80
    static int (*orig_Presenter_GetRemainSec)(void *instance) = nullptr;
    static int hook_Presenter_GetRemainSec(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return 0; // Cooldown 1h 57m -> 0s
        }
        return orig_Presenter_GetRemainSec ? orig_Presenter_GetRemainSec(instance) : 0;
    }

    // GetCooldownSeconds() -> int: RVA 0x484A2EC
    static int (*orig_Presenter_GetCooldown)(void *instance) = nullptr;
    static int hook_Presenter_GetCooldown(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return 0;
        }
        return orig_Presenter_GetCooldown ? orig_Presenter_GetCooldown(instance) : 0;
    }

    // IsAvailable() -> bool: RVA 0x484A374
    static bool (*orig_Presenter_IsAvailable)(void *instance) = nullptr;
    static bool hook_Presenter_IsAvailable(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Presenter_IsAvailable ? orig_Presenter_IsAvailable(instance) : true;
    }

    // IsReady() -> bool: RVA 0x484A7B8
    static bool (*orig_Presenter_IsReady)(void *instance) = nullptr;
    static bool hook_Presenter_IsReady(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Presenter_IsReady ? orig_Presenter_IsReady(instance) : true;
    }

    // --------------------------------------------------------------------------
    // 3. ChestBuyTimer Hooks (Shop chest purchase / claim countdown)
    // --------------------------------------------------------------------------

    // SetTime(long seconds): RVA 0x440D248
    static void (*orig_ChestTimer_SetTime)(void *instance, int64_t seconds) = nullptr;
    static void hook_ChestTimer_SetTime(void *instance, int64_t seconds) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            seconds = 0;
        }
        if (orig_ChestTimer_SetTime) {
            orig_ChestTimer_SetTime(instance, seconds);
        }
    }

    // CanClaim() -> bool: RVA 0x440D494
    static bool (*orig_ChestTimer_CanClaim)(void *instance) = nullptr;
    static bool hook_ChestTimer_CanClaim(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_ChestTimer_CanClaim ? orig_ChestTimer_CanClaim(instance) : true;
    }

    // --------------------------------------------------------------------------
    // Public API
    // --------------------------------------------------------------------------

    void Init() {
        LOG_INFO("[FREE_REWARD] Installing Reset Reward Time (1h 57m -> 0s) hooks...");

        // Hook FreeDailyRewardItemView
        Memory::HookRVA(Offsets::FreeReward::View_SetAvailable,
                        reinterpret_cast<void*>(hook_View_SetAvailable),
                        reinterpret_cast<void**>(&orig_View_SetAvailable));

        Memory::HookRVA(Offsets::FreeReward::View_UpdateTimer,
                        reinterpret_cast<void*>(hook_View_UpdateTimer),
                        reinterpret_cast<void**>(&orig_View_UpdateTimer));

        Memory::HookRVA(Offsets::FreeReward::View_OnTimerTick,
                        reinterpret_cast<void*>(hook_View_OnTimerTick),
                        reinterpret_cast<void**>(&orig_View_OnTimerTick));

        Memory::HookRVA(Offsets::FreeReward::View_OnEnable,
                        reinterpret_cast<void*>(hook_View_OnEnable),
                        reinterpret_cast<void**>(&orig_View_OnEnable));

        // Hook FreeDailyReward Presenter
        Memory::HookRVA(Offsets::FreeReward::Presenter_CanTake,
                        reinterpret_cast<void*>(hook_Presenter_CanTake),
                        reinterpret_cast<void**>(&orig_Presenter_CanTake));

        Memory::HookRVA(Offsets::FreeReward::Presenter_GetRemainSec,
                        reinterpret_cast<void*>(hook_Presenter_GetRemainSec),
                        reinterpret_cast<void**>(&orig_Presenter_GetRemainSec));

        Memory::HookRVA(Offsets::FreeReward::Presenter_GetCooldown,
                        reinterpret_cast<void*>(hook_Presenter_GetCooldown),
                        reinterpret_cast<void**>(&orig_Presenter_GetCooldown));

        Memory::HookRVA(Offsets::FreeReward::Presenter_IsAvailable,
                        reinterpret_cast<void*>(hook_Presenter_IsAvailable),
                        reinterpret_cast<void**>(&orig_Presenter_IsAvailable));

        Memory::HookRVA(Offsets::FreeReward::Presenter_IsReady,
                        reinterpret_cast<void*>(hook_Presenter_IsReady),
                        reinterpret_cast<void**>(&orig_Presenter_IsReady));

        // Hook ChestBuyTimer
        Memory::HookRVA(Offsets::FreeReward::ChestTimer_SetTime,
                        reinterpret_cast<void*>(hook_ChestTimer_SetTime),
                        reinterpret_cast<void**>(&orig_ChestTimer_SetTime));

        Memory::HookRVA(Offsets::FreeReward::ChestTimer_CanClaim,
                        reinterpret_cast<void*>(hook_ChestTimer_CanClaim),
                        reinterpret_cast<void**>(&orig_ChestTimer_CanClaim));

        LOG_INFO("[FREE_REWARD] Reset Reward Time hooks installed successfully.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[FREE_REWARD] Reset Reward Time (1h 57m): %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace ResetRewardTime
