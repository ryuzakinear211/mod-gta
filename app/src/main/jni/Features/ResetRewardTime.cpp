#include "ResetRewardTime.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"

#include <atomic>
#include <cstdint>

namespace ResetRewardTime {

    static std::atomic<bool> s_enabled{false};

    // --------------------------------------------------------------------------
    // FreeDailyReward Presenter Hooks (dump.cs class: baaaaababbababababbbabaabbbbbbbbbbaaabbbabbbbbb)
    // Only hooking verified large functions (>= 68 bytes) to ensure 100% safety with 20-byte inline hooks
    // --------------------------------------------------------------------------

    // CanTakeReward() -> bool: RVA 0x4849B84 (Function length: 68 bytes)
    static bool (*orig_Presenter_CanTake)(void *instance) = nullptr;
    static bool hook_Presenter_CanTake(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_Presenter_CanTake ? orig_Presenter_CanTake(instance) : true;
    }

    // GetRemainingSeconds() -> int: RVA 0x4849C80 (Function length: 124 bytes)
    // Directly bypasses the 1h 57m (7200s) cooldown timer to 0s
    static int (*orig_Presenter_GetRemainSec)(void *instance) = nullptr;
    static int hook_Presenter_GetRemainSec(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return 0; // 0 seconds remaining -> ready to claim immediately
        }
        return orig_Presenter_GetRemainSec ? orig_Presenter_GetRemainSec(instance) : 0;
    }

    // GetCooldownSeconds() -> int: RVA 0x484A2EC (Function length: 124 bytes)
    static int (*orig_Presenter_GetCooldown)(void *instance) = nullptr;
    static int hook_Presenter_GetCooldown(void *instance) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            return 0;
        }
        return orig_Presenter_GetCooldown ? orig_Presenter_GetCooldown(instance) : 0;
    }

    void Init() {
        LOG_INFO("[FREE_REWARD] Installing safe Reset Reward Time (1h 57m -> 0s) hooks...");

        // Hook Presenter methods with safe margins (> 60 bytes)
        Memory::HookRVA(Offsets::FreeReward::Presenter_CanTake,
                        reinterpret_cast<void*>(hook_Presenter_CanTake),
                        reinterpret_cast<void**>(&orig_Presenter_CanTake));

        Memory::HookRVA(Offsets::FreeReward::Presenter_GetRemainSec,
                        reinterpret_cast<void*>(hook_Presenter_GetRemainSec),
                        reinterpret_cast<void**>(&orig_Presenter_GetRemainSec));

        Memory::HookRVA(Offsets::FreeReward::Presenter_GetCooldown,
                        reinterpret_cast<void*>(hook_Presenter_GetCooldown),
                        reinterpret_cast<void**>(&orig_Presenter_GetCooldown));

        LOG_INFO("[FREE_REWARD] Safe Reset Reward Time hooks installed successfully.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[FREE_REWARD] Reset Reward Time (1h 57m): %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace ResetRewardTime
