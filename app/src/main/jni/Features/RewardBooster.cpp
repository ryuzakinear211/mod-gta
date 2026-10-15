#include "RewardBooster.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>

namespace RewardBooster {

    static std::atomic<bool> s_enabled{false};
    static std::atomic<int>  s_targetAmount{5000};

    // Original function pointers
    static void (*orig_RewardItem_ctor)(void *instance, int type, int id, int amount, int p3, void *p4) = nullptr;
    static int  (*orig_RewardItem_GetAmount)(void *instance) = nullptr;
    static void (*orig_RewardItem_SetAmount)(void *instance, int amount) = nullptr;

    static void (*orig_RewardView_Setup)(void *view, void *data, void *rewardItem, void *rect, bool flag) = nullptr;

    static void (*orig_Currency_AddInt1)(void *instance, int currency, int amount) = nullptr;
    static void (*orig_Currency_AddInt2)(void *instance, int currency, int amount) = nullptr;
    static void (*orig_Currency_AddLong1)(void *instance, int currency, int64_t amount) = nullptr;
    static void (*orig_Currency_AddLong2)(void *instance, int currency, int64_t amount) = nullptr;
    static void (*orig_Currency_AddLong3)(void *instance, int currency, int64_t amount) = nullptr;
    static void (*orig_Currency_AddInt3)(void *instance, int currency, int amount) = nullptr;
    static void (*orig_Currency_AddLong4)(void *instance, int currency, int64_t amount) = nullptr;

    static inline bool IsTargetCurrency(int currency) {
        // 1 = Soft (Money / Credits), 2 = Hard (Gold / Gems)
        return (currency == Offsets::Reward::CurrencyType::Soft ||
                currency == Offsets::Reward::CurrencyType::Hard);
    }

    static inline bool IsTargetReward(int type, int id) {
        // Type 22 = GameMoney, or Currency ID is Soft/Hard
        return (type == Offsets::Reward::RewardType::GameMoney ||
                id == Offsets::Reward::CurrencyType::Soft ||
                id == Offsets::Reward::CurrencyType::Hard);
    }

    // --------------------------------------------------------------------------
    // Hooks for RewardItem (abbbbabbbaaabababbbbbbbbaabbbbaababababaababbbb)
    // --------------------------------------------------------------------------

    static void hook_RewardItem_ctor(void *instance, int type, int id, int amount, int p3, void *p4) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetReward(type, id) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Boosting RewardItem .ctor: type=%d, id=%d, orig=%d -> %d",
                         type, id, amount, target);
                amount = target;
            }
        }

        if (orig_RewardItem_ctor) {
            orig_RewardItem_ctor(instance, type, id, amount, p3, p4);
        }

        // Ensure instance field 0x18 holds boosted amount
        if (instance && s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetReward(type, id)) {
                int curAmt = Memory::ReadField<int>(instance, Offsets::Reward::RewardItem_Amount);
                if (curAmt > 0 && curAmt < target) {
                    Memory::WriteField<int>(instance, Offsets::Reward::RewardItem_Amount, target);
                }
            }
        }
    }

    static int hook_RewardItem_GetAmount(void *instance) {
        int amt = orig_RewardItem_GetAmount ? orig_RewardItem_GetAmount(instance) : 0;
        if (instance && s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            int type = Memory::ReadField<int>(instance, Offsets::Reward::RewardItem_Type);
            int id   = Memory::ReadField<int>(instance, Offsets::Reward::RewardItem_CurrencyId);
            if (IsTargetReward(type, id) && amt > 0 && amt < target) {
                return target;
            }
        }
        return amt;
    }

    static void hook_RewardItem_SetAmount(void *instance, int amount) {
        if (instance && s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            int type = Memory::ReadField<int>(instance, Offsets::Reward::RewardItem_Type);
            int id   = Memory::ReadField<int>(instance, Offsets::Reward::RewardItem_CurrencyId);
            if (IsTargetReward(type, id) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Boosting RewardItem SetAmount: type=%d, id=%d, orig=%d -> %d",
                         type, id, amount, target);
                amount = target;
            }
        }
        if (orig_RewardItem_SetAmount) {
            orig_RewardItem_SetAmount(instance, amount);
        }
    }

    // --------------------------------------------------------------------------
    // Hook for BaseRewardElementView.Setup (UI Rendering of Reward Items)
    // --------------------------------------------------------------------------

    static void hook_RewardView_Setup(void *view, void *data, void *rewardItem, void *rect, bool flag) {
        if (rewardItem && s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            int type = Memory::ReadField<int>(rewardItem, Offsets::Reward::RewardItem_Type);
            int id   = Memory::ReadField<int>(rewardItem, Offsets::Reward::RewardItem_CurrencyId);
            int amt  = Memory::ReadField<int>(rewardItem, Offsets::Reward::RewardItem_Amount);
            if (IsTargetReward(type, id) && amt > 0 && amt < target) {
                Memory::WriteField<int>(rewardItem, Offsets::Reward::RewardItem_Amount, target);
                LOG_INFO("[REWARD] UI BaseRewardElementView.Setup boosted amount: %d -> %d", amt, target);
            }
        }

        if (orig_RewardView_Setup) {
            orig_RewardView_Setup(view, data, rewardItem, rect, flag);
        }
    }

    // --------------------------------------------------------------------------
    // Hooks for Currency / Wallet Manager (ababaaababaabaabbbababaaaabbbbaabaababbbbaaabbb)
    // --------------------------------------------------------------------------

    static void hook_Currency_AddInt1(void *instance, int currency, int amount) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetCurrency(currency) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Currency_AddInt1 boosted: cur=%d, %d -> %d", currency, amount, target);
                amount = target;
            }
        }
        if (orig_Currency_AddInt1) orig_Currency_AddInt1(instance, currency, amount);
    }

    static void hook_Currency_AddInt2(void *instance, int currency, int amount) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetCurrency(currency) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Currency_AddInt2 boosted: cur=%d, %d -> %d", currency, amount, target);
                amount = target;
            }
        }
        if (orig_Currency_AddInt2) orig_Currency_AddInt2(instance, currency, amount);
    }

    static void hook_Currency_AddLong1(void *instance, int currency, int64_t amount) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int64_t target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetCurrency(currency) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Currency_AddLong1 boosted: cur=%d, %lld -> %lld", currency, (long long)amount, (long long)target);
                amount = target;
            }
        }
        if (orig_Currency_AddLong1) orig_Currency_AddLong1(instance, currency, amount);
    }

    static void hook_Currency_AddLong2(void *instance, int currency, int64_t amount) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int64_t target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetCurrency(currency) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Currency_AddLong2 boosted: cur=%d, %lld -> %lld", currency, (long long)amount, (long long)target);
                amount = target;
            }
        }
        if (orig_Currency_AddLong2) orig_Currency_AddLong2(instance, currency, amount);
    }

    static void hook_Currency_AddLong3(void *instance, int currency, int64_t amount) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int64_t target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetCurrency(currency) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Currency_AddLong3 boosted: cur=%d, %lld -> %lld", currency, (long long)amount, (long long)target);
                amount = target;
            }
        }
        if (orig_Currency_AddLong3) orig_Currency_AddLong3(instance, currency, amount);
    }

    static void hook_Currency_AddInt3(void *instance, int currency, int amount) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetCurrency(currency) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Currency_AddInt3 boosted: cur=%d, %d -> %d", currency, amount, target);
                amount = target;
            }
        }
        if (orig_Currency_AddInt3) orig_Currency_AddInt3(instance, currency, amount);
    }

    static void hook_Currency_AddLong4(void *instance, int currency, int64_t amount) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int64_t target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetCurrency(currency) && amount > 0 && amount < target) {
                LOG_INFO("[REWARD] Currency_AddLong4 boosted: cur=%d, %lld -> %lld", currency, (long long)amount, (long long)target);
                amount = target;
            }
        }
        if (orig_Currency_AddLong4) orig_Currency_AddLong4(instance, currency, amount);
    }

    // --------------------------------------------------------------------------
    // Initialization & Hook Installation
    // --------------------------------------------------------------------------

    void Init() {
        LOG_INFO("[REWARD] Installing RewardBooster hooks...");

        // Hook RewardItem operations
        Memory::HookRVA(Offsets::Reward::RewardItem_ctor,
                        reinterpret_cast<void*>(hook_RewardItem_ctor),
                        reinterpret_cast<void**>(&orig_RewardItem_ctor));

        Memory::HookRVA(Offsets::Reward::RewardItem_GetAmount,
                        reinterpret_cast<void*>(hook_RewardItem_GetAmount),
                        reinterpret_cast<void**>(&orig_RewardItem_GetAmount));

        Memory::HookRVA(Offsets::Reward::RewardItem_SetAmount,
                        reinterpret_cast<void*>(hook_RewardItem_SetAmount),
                        reinterpret_cast<void**>(&orig_RewardItem_SetAmount));

        // Hook Reward UI display
        Memory::HookRVA(Offsets::Reward::RewardView_Setup,
                        reinterpret_cast<void*>(hook_RewardView_Setup),
                        reinterpret_cast<void**>(&orig_RewardView_Setup));

        // Hook Currency / Wallet deposit operations
        Memory::HookRVA(Offsets::Reward::Currency_AddInt1,
                        reinterpret_cast<void*>(hook_Currency_AddInt1),
                        reinterpret_cast<void**>(&orig_Currency_AddInt1));

        Memory::HookRVA(Offsets::Reward::Currency_AddInt2,
                        reinterpret_cast<void*>(hook_Currency_AddInt2),
                        reinterpret_cast<void**>(&orig_Currency_AddInt2));

        Memory::HookRVA(Offsets::Reward::Currency_AddLong1,
                        reinterpret_cast<void*>(hook_Currency_AddLong1),
                        reinterpret_cast<void**>(&orig_Currency_AddLong1));

        Memory::HookRVA(Offsets::Reward::Currency_AddLong2,
                        reinterpret_cast<void*>(hook_Currency_AddLong2),
                        reinterpret_cast<void**>(&orig_Currency_AddLong2));

        Memory::HookRVA(Offsets::Reward::Currency_AddLong3,
                        reinterpret_cast<void*>(hook_Currency_AddLong3),
                        reinterpret_cast<void**>(&orig_Currency_AddLong3));

        Memory::HookRVA(Offsets::Reward::Currency_AddInt3,
                        reinterpret_cast<void*>(hook_Currency_AddInt3),
                        reinterpret_cast<void**>(&orig_Currency_AddInt3));

        Memory::HookRVA(Offsets::Reward::Currency_AddLong4,
                        reinterpret_cast<void*>(hook_Currency_AddLong4),
                        reinterpret_cast<void**>(&orig_Currency_AddLong4));

        LOG_INFO("[REWARD] RewardBooster hooks installed successfully.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[REWARD] Reward Booster 5000: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

    void SetTargetAmount(int amount) {
        s_targetAmount.store(amount > 0 ? amount : 5000, std::memory_order_relaxed);
    }

    int GetTargetAmount() {
        return s_targetAmount.load(std::memory_order_relaxed);
    }

} // namespace RewardBooster
