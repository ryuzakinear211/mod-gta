#include "RewardBooster.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>

namespace RewardBooster {

    static std::atomic<bool> s_enabled{false};
    static std::atomic<int>  s_targetAmount{5000};

    // Original function pointers for Domain Models (SerializableUserReward & List)
    static void  (*orig_SerialReward_ctor)(void *instance, int type, int value, int count, int comp, void *meta) = nullptr;
    static void  (*orig_SerialList_AddReward)(void *instance, void *reward) = nullptr;
    static void* (*orig_SerialList_GetList1)(void *instance) = nullptr;
    static void* (*orig_SerialList_GetList2)(void *instance) = nullptr;
    static void* (*orig_SerialList_GetList3)(void *instance) = nullptr;
    static void* (*orig_SerialList_GetList4)(void *instance) = nullptr;
    static void* (*orig_SerialList_GetList5)(void *instance) = nullptr;
    static void* (*orig_SerialList_GetList6)(void *instance) = nullptr;

    // Original function pointers for RewardItem
    static void (*orig_RewardItem_ctor)(void *instance, int type, int id, int amount, int p3, void *p4) = nullptr;
    static int  (*orig_RewardItem_GetAmount)(void *instance) = nullptr;
    static void (*orig_RewardItem_SetAmount)(void *instance, int amount) = nullptr;

    // Original function pointers for Popup & UI
    static void (*orig_PopupData_ctor)(void *instance, void *p1, void *rewardList, void *p3) = nullptr;
    static void (*orig_RewardView_Setup)(void *view, void *data, void *rewardItem, void *rect, bool flag) = nullptr;

    static inline bool IsTargetReward(int type, int id) {
        // Type 22 = GameMoney, ID 1 = Soft (Money/Credits), ID 2 = Hard (Gold/Gems)
        return (type == Offsets::Reward::RewardType::GameMoney ||
                id == Offsets::Reward::CurrencyType::Soft ||
                id == Offsets::Reward::CurrencyType::Hard);
    }

    // Safely boosts all target rewards in an Il2Cpp List<RewardItem>
    static void BoostRewardList(void *listInstance) {
        if (!Memory::IsValidPtr(listInstance)) return;
        int target = s_targetAmount.load(std::memory_order_relaxed);

        void *itemsArray = Memory::ReadField<void*>(listInstance, 0x10);
        int size = Memory::ReadField<int>(listInstance, 0x18);

        if (!Memory::IsValidPtr(itemsArray) || size <= 0 || size > 1000) return;

        for (int i = 0; i < size; i++) {
            uintptr_t itemAddr = reinterpret_cast<uintptr_t>(itemsArray) + 0x20 + (i * sizeof(void*));
            void *rewardItem = *reinterpret_cast<void**>(itemAddr);
            if (Memory::IsValidPtr(rewardItem)) {
                int type = Memory::ReadField<int>(rewardItem, Offsets::Reward::RewardItem_Type);
                int curId = Memory::ReadField<int>(rewardItem, Offsets::Reward::RewardItem_CurrencyId);
                int amt = Memory::ReadField<int>(rewardItem, Offsets::Reward::RewardItem_Amount);
                if (IsTargetReward(type, curId) && amt > 0 && amt < target) {
                    Memory::WriteField<int>(rewardItem, Offsets::Reward::RewardItem_Amount, target);
                    LOG_INFO("[REWARD] Boosted RewardItem in List: type=%d, id=%d, %d -> %d",
                             type, curId, amt, target);
                }
            }
        }
    }

    // --------------------------------------------------------------------------
    // Hooks for Domain Model: SerializableUserReward (LastLevel.Legion.Dom)
    // --------------------------------------------------------------------------

    static void hook_SerialReward_ctor(void *instance, int type, int value, int count, int comp, void *meta) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetReward(type, value) && count > 0 && count < target) {
                LOG_INFO("[REWARD] Boosting SerializableUserReward .ctor: type=%d, value=%d, count=%d -> %d",
                         type, value, count, target);
                count = target;
            }
        }

        if (orig_SerialReward_ctor) {
            orig_SerialReward_ctor(instance, type, value, count, comp, meta);
        }

        if (instance && s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            if (IsTargetReward(type, value)) {
                int curCount = Memory::ReadField<int>(instance, Offsets::Reward::SerialReward_Count);
                if (curCount > 0 && curCount < target) {
                    Memory::WriteField<int>(instance, Offsets::Reward::SerialReward_Count, target);
                }
            }
        }
    }

    // --------------------------------------------------------------------------
    // Hooks for Domain Model: SerializableUserRewardList
    // --------------------------------------------------------------------------

    static void hook_SerialList_AddReward(void *instance, void *reward) {
        if (reward && s_enabled.load(std::memory_order_relaxed)) {
            int target = s_targetAmount.load(std::memory_order_relaxed);
            int type  = Memory::ReadField<int>(reward, Offsets::Reward::SerialReward_Type);
            int value = Memory::ReadField<int>(reward, Offsets::Reward::SerialReward_Value);
            int count = Memory::ReadField<int>(reward, Offsets::Reward::SerialReward_Count);
            if (IsTargetReward(type, value) && count > 0 && count < target) {
                Memory::WriteField<int>(reward, Offsets::Reward::SerialReward_Count, target);
                LOG_INFO("[REWARD] SerializableUserRewardList.Add boosted count: %d -> %d", count, target);
            }
        }
        if (orig_SerialList_AddReward) {
            orig_SerialList_AddReward(instance, reward);
        }
    }

    static void* hook_SerialList_GetList1(void *instance) {
        void *list = orig_SerialList_GetList1 ? orig_SerialList_GetList1(instance) : nullptr;
        if (s_enabled.load(std::memory_order_relaxed)) BoostRewardList(list);
        return list;
    }

    static void* hook_SerialList_GetList2(void *instance) {
        void *list = orig_SerialList_GetList2 ? orig_SerialList_GetList2(instance) : nullptr;
        if (s_enabled.load(std::memory_order_relaxed)) BoostRewardList(list);
        return list;
    }

    static void* hook_SerialList_GetList3(void *instance) {
        void *list = orig_SerialList_GetList3 ? orig_SerialList_GetList3(instance) : nullptr;
        if (s_enabled.load(std::memory_order_relaxed)) BoostRewardList(list);
        return list;
    }

    static void* hook_SerialList_GetList4(void *instance) {
        void *list = orig_SerialList_GetList4 ? orig_SerialList_GetList4(instance) : nullptr;
        if (s_enabled.load(std::memory_order_relaxed)) BoostRewardList(list);
        return list;
    }

    static void* hook_SerialList_GetList5(void *instance) {
        void *list = orig_SerialList_GetList5 ? orig_SerialList_GetList5(instance) : nullptr;
        if (s_enabled.load(std::memory_order_relaxed)) BoostRewardList(list);
        return list;
    }

    static void* hook_SerialList_GetList6(void *instance) {
        void *list = orig_SerialList_GetList6 ? orig_SerialList_GetList6(instance) : nullptr;
        if (s_enabled.load(std::memory_order_relaxed)) BoostRewardList(list);
        return list;
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
    // Hooks for Reward Popup Data (baabbabbbabaabaaaaaabbbabbbbbbbbbbbbabaaabbaaaa)
    // --------------------------------------------------------------------------

    static void hook_PopupData_ctor(void *instance, void *p1, void *rewardList, void *p3) {
        if (orig_PopupData_ctor) {
            orig_PopupData_ctor(instance, p1, rewardList, p3);
        }
        if (s_enabled.load(std::memory_order_relaxed)) {
            BoostRewardList(rewardList);
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
    // Initialization & Hook Installation
    // --------------------------------------------------------------------------

    void Init() {
        LOG_INFO("[REWARD] Installing RewardBooster hooks (pure reward modification, profile currency untouched)...");

        // 1. Hook Domain Model: SerializableUserReward
        Memory::HookRVA(Offsets::Reward::SerialReward_ctor,
                        reinterpret_cast<void*>(hook_SerialReward_ctor),
                        reinterpret_cast<void**>(&orig_SerialReward_ctor));

        // 2. Hook Domain Model: SerializableUserRewardList
        Memory::HookRVA(Offsets::Reward::SerialList_AddReward,
                        reinterpret_cast<void*>(hook_SerialList_AddReward),
                        reinterpret_cast<void**>(&orig_SerialList_AddReward));

        Memory::HookRVA(Offsets::Reward::SerialList_GetList1,
                        reinterpret_cast<void*>(hook_SerialList_GetList1),
                        reinterpret_cast<void**>(&orig_SerialList_GetList1));

        Memory::HookRVA(Offsets::Reward::SerialList_GetList2,
                        reinterpret_cast<void*>(hook_SerialList_GetList2),
                        reinterpret_cast<void**>(&orig_SerialList_GetList2));

        Memory::HookRVA(Offsets::Reward::SerialList_GetList3,
                        reinterpret_cast<void*>(hook_SerialList_GetList3),
                        reinterpret_cast<void**>(&orig_SerialList_GetList3));

        Memory::HookRVA(Offsets::Reward::SerialList_GetList4,
                        reinterpret_cast<void*>(hook_SerialList_GetList4),
                        reinterpret_cast<void**>(&orig_SerialList_GetList4));

        Memory::HookRVA(Offsets::Reward::SerialList_GetList5,
                        reinterpret_cast<void*>(hook_SerialList_GetList5),
                        reinterpret_cast<void**>(&orig_SerialList_GetList5));

        Memory::HookRVA(Offsets::Reward::SerialList_GetList6,
                        reinterpret_cast<void*>(hook_SerialList_GetList6),
                        reinterpret_cast<void**>(&orig_SerialList_GetList6));

        // 3. Hook Presentation Model: RewardItem
        Memory::HookRVA(Offsets::Reward::RewardItem_ctor,
                        reinterpret_cast<void*>(hook_RewardItem_ctor),
                        reinterpret_cast<void**>(&orig_RewardItem_ctor));

        Memory::HookRVA(Offsets::Reward::RewardItem_GetAmount,
                        reinterpret_cast<void*>(hook_RewardItem_GetAmount),
                        reinterpret_cast<void**>(&orig_RewardItem_GetAmount));

        Memory::HookRVA(Offsets::Reward::RewardItem_SetAmount,
                        reinterpret_cast<void*>(hook_RewardItem_SetAmount),
                        reinterpret_cast<void**>(&orig_RewardItem_SetAmount));

        // 4. Hook Reward Popup Data Model
        Memory::HookRVA(Offsets::Reward::PopupData_ctor,
                        reinterpret_cast<void*>(hook_PopupData_ctor),
                        reinterpret_cast<void**>(&orig_PopupData_ctor));

        // 5. Hook Reward UI View
        Memory::HookRVA(Offsets::Reward::RewardView_Setup,
                        reinterpret_cast<void*>(hook_RewardView_Setup),
                        reinterpret_cast<void**>(&orig_RewardView_Setup));

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
