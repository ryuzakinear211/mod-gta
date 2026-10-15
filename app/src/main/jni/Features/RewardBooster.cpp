#include "RewardBooster.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>

namespace RewardBooster {

    static std::atomic<bool> s_enabled{false};
    static std::atomic<int>  s_targetAmount{5000};
    static std::atomic<bool> s_freezeClaim{false};

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

    // Original function pointers for Freeze Claim (Quests & Daily Rewards)
    static int   (*orig_QuestData_GetState)(void *instance) = nullptr;
    static int   (*orig_QuestPresenter_GetState)(void *instance) = nullptr;
    static void  (*orig_QuestData_SetState1)(void *instance, int state) = nullptr;
    static void  (*orig_QuestData_SetState2)(void *instance, int state) = nullptr;
    static void  (*orig_QuestItemView_UpdateState)(void *view) = nullptr;
    static void  (*orig_DailyRepeatingQuestView_UpdateState)(void *view) = nullptr;
    static int   (*orig_DailyQuestGroupRewards_GetState)(void *instance) = nullptr;
    static bool  (*orig_GroupRewardItemView_CanClaim)(void *instance) = nullptr;
    static void  (*orig_DailyRewardContainer_Setup)(void *instance, void *p1, void *p2, void *p3) = nullptr;

    // Unity UI native helpers
    static void UnitySetActive(void *go, bool active) {
        if (!Memory::IsValidPtr(go)) return;
        static void (*func_SetActive)(void*, bool) = nullptr;
        if (!func_SetActive) {
            func_SetActive = reinterpret_cast<void(*)(void*, bool)>(Memory::GetRVA(Offsets::Unity::GameObject_SetActive));
        }
        if (func_SetActive) {
            func_SetActive(go, active);
        }
    }

    static void SetButtonInteractable(void *btn, bool interactable) {
        if (!Memory::IsValidPtr(btn)) return;
        static void (*func_SetInteractable)(void*, bool) = nullptr;
        static void* (*func_GetGameObject)(void*) = nullptr;
        if (!func_SetInteractable) {
            func_SetInteractable = reinterpret_cast<void(*)(void*, bool)>(Memory::GetRVA(Offsets::Unity::Selectable_SetInteractable));
        }
        if (!func_GetGameObject) {
            func_GetGameObject = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::Unity::Component_GetGameObject));
        }
        if (func_SetInteractable) {
            func_SetInteractable(btn, interactable);
        }
        if (func_GetGameObject) {
            void *go = func_GetGameObject(btn);
            if (go) UnitySetActive(go, true);
        }
    }

    static void SetListGameObjectsActive(void *listInstance, bool active) {
        if (!Memory::IsValidPtr(listInstance)) return;
        void *itemsArray = Memory::ReadField<void*>(listInstance, 0x10);
        int size = Memory::ReadField<int>(listInstance, 0x18);
        if (!Memory::IsValidPtr(itemsArray) || size <= 0 || size > 50) return;
        for (int i = 0; i < size; i++) {
            uintptr_t itemAddr = reinterpret_cast<uintptr_t>(itemsArray) + 0x20 + (i * sizeof(void*));
            void *go = *reinterpret_cast<void**>(itemAddr);
            if (Memory::IsValidPtr(go)) {
                UnitySetActive(go, active);
            }
        }
    }

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
    // Hooks for Freeze Claim (Quests, Daily Quests & Calendar Rewards)
    // --------------------------------------------------------------------------

    static int hook_QuestData_GetState(void *instance) {
        int state = orig_QuestData_GetState ? orig_QuestData_GetState(instance) : 0;
        if (s_freezeClaim.load(std::memory_order_relaxed)) {
            // If quest is Finished (claimed = 2), freeze it at Complete (claimable = 1)
            if (state == Offsets::Quest::QuestState::Finished) {
                return Offsets::Quest::QuestState::Complete;
            }
        }
        return state;
    }

    static int hook_QuestPresenter_GetState(void *instance) {
        int state = orig_QuestPresenter_GetState ? orig_QuestPresenter_GetState(instance) : 0;
        if (s_freezeClaim.load(std::memory_order_relaxed)) {
            if (state == Offsets::Quest::QuestState::Finished) {
                return Offsets::Quest::QuestState::Complete;
            }
        }
        return state;
    }

    static void hook_QuestData_SetState1(void *instance, int state) {
        if (s_freezeClaim.load(std::memory_order_relaxed)) {
            if (state == Offsets::Quest::QuestState::Finished) {
                LOG_INFO("[FREEZE_CLAIM] Intercepted QuestData.SetState1: Finished (2) -> Complete (1)");
                state = Offsets::Quest::QuestState::Complete;
            }
        }
        if (orig_QuestData_SetState1) {
            orig_QuestData_SetState1(instance, state);
        }
        if (instance && s_freezeClaim.load(std::memory_order_relaxed)) {
            int cur = Memory::ReadField<int>(instance, Offsets::Quest::QuestData_StateField);
            if (cur == Offsets::Quest::QuestState::Finished) {
                Memory::WriteField<int>(instance, Offsets::Quest::QuestData_StateField, Offsets::Quest::QuestState::Complete);
            }
        }
    }

    static void hook_QuestData_SetState2(void *instance, int state) {
        if (s_freezeClaim.load(std::memory_order_relaxed)) {
            if (state == Offsets::Quest::QuestState::Finished) {
                LOG_INFO("[FREEZE_CLAIM] Intercepted QuestData.SetState2: Finished (2) -> Complete (1)");
                state = Offsets::Quest::QuestState::Complete;
            }
        }
        if (orig_QuestData_SetState2) {
            orig_QuestData_SetState2(instance, state);
        }
        if (instance && s_freezeClaim.load(std::memory_order_relaxed)) {
            int cur = Memory::ReadField<int>(instance, Offsets::Quest::QuestData_StateField);
            if (cur == Offsets::Quest::QuestState::Finished) {
                Memory::WriteField<int>(instance, Offsets::Quest::QuestData_StateField, Offsets::Quest::QuestState::Complete);
            }
        }
    }

    static void hook_QuestItemView_UpdateState(void *view) {
        if (orig_QuestItemView_UpdateState) {
            orig_QuestItemView_UpdateState(view);
        }
        if (view && s_freezeClaim.load(std::memory_order_relaxed)) {
            // Keep local state field at Complete (1)
            Memory::WriteField<int>(view, Offsets::Quest::QuestItemView_StateField, Offsets::Quest::QuestState::Complete);

            // Hide finished state GameObjects
            void *finishedList = Memory::ReadField<void*>(view, Offsets::Quest::QuestItemView_FinishedState);
            SetListGameObjectsActive(finishedList, false);

            // Show available reward GameObjects
            void *availList = Memory::ReadField<void*>(view, Offsets::Quest::QuestItemView_RewardAvailable);
            SetListGameObjectsActive(availList, true);

            // Keep claim button enabled and interactable
            void *btn = Memory::ReadField<void*>(view, Offsets::Quest::QuestItemView_ClaimButton);
            SetButtonInteractable(btn, true);
        }
    }

    static void hook_DailyRepeatingQuestView_UpdateState(void *view) {
        if (orig_DailyRepeatingQuestView_UpdateState) {
            orig_DailyRepeatingQuestView_UpdateState(view);
        }
        if (view && s_freezeClaim.load(std::memory_order_relaxed)) {
            Memory::WriteField<int>(view, Offsets::Quest::DailyRepeatingQuestView_StateField, Offsets::Quest::QuestState::Complete);

            void *finishedList = Memory::ReadField<void*>(view, Offsets::Quest::DailyRepeatingQuestView_FinishedState);
            SetListGameObjectsActive(finishedList, false);

            void *availList = Memory::ReadField<void*>(view, Offsets::Quest::DailyRepeatingQuestView_RewardAvailable);
            SetListGameObjectsActive(availList, true);

            void *btn = Memory::ReadField<void*>(view, Offsets::Quest::DailyRepeatingQuestView_ClaimButton);
            SetButtonInteractable(btn, true);
        }
    }

    static int hook_DailyQuestGroupRewards_GetState(void *instance) {
        int state = orig_DailyQuestGroupRewards_GetState ? orig_DailyQuestGroupRewards_GetState(instance) : 0;
        if (s_freezeClaim.load(std::memory_order_relaxed)) {
            if (state == Offsets::Quest::RewardState::Claimed) {
                return Offsets::Quest::RewardState::Available;
            }
        }
        return state;
    }

    static bool hook_GroupRewardItemView_CanClaim(void *instance) {
        if (s_freezeClaim.load(std::memory_order_relaxed)) {
            return true;
        }
        return orig_GroupRewardItemView_CanClaim ? orig_GroupRewardItemView_CanClaim(instance) : false;
    }

    static void hook_DailyRewardContainer_Setup(void *instance, void *param1, void *param2, void *param3) {
        if (orig_DailyRewardContainer_Setup) {
            orig_DailyRewardContainer_Setup(instance, param1, param2, param3);
        }
        if (instance && s_freezeClaim.load(std::memory_order_relaxed)) {
            // Offset 0x90 is stored struct: +0x98 RewardState, +0xA4 bool isClaimed
            int st = Memory::ReadField<int>(instance, 0x98);
            if (st == Offsets::Quest::RewardState::Claimed) {
                Memory::WriteField<int>(instance, 0x98, Offsets::Quest::RewardState::Available);
                Memory::WriteField<bool>(instance, 0xA4, false);

                void *claimedBg = Memory::ReadField<void*>(instance, Offsets::Quest::DailyReward_ClaimedBg);
                void *claimedFrame = Memory::ReadField<void*>(instance, Offsets::Quest::DailyReward_ClaimedFrame);
                UnitySetActive(claimedBg, false);
                UnitySetActive(claimedFrame, false);

                void *toggle = Memory::ReadField<void*>(instance, Offsets::Quest::DailyReward_ItemToggle);
                SetButtonInteractable(toggle, true);
            }
        }
    }

    // --------------------------------------------------------------------------
    // Initialization & Hook Installation
    // --------------------------------------------------------------------------

    void Init() {
        LOG_INFO("[REWARD] Installing RewardBooster & Freeze Claim hooks...");

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

        // 6. Hook Freeze Claim / Infinite Quest Claim State
        Memory::HookRVA(Offsets::Quest::QuestData_GetState,
                        reinterpret_cast<void*>(hook_QuestData_GetState),
                        reinterpret_cast<void**>(&orig_QuestData_GetState));

        Memory::HookRVA(Offsets::Quest::QuestPresenter_GetState,
                        reinterpret_cast<void*>(hook_QuestPresenter_GetState),
                        reinterpret_cast<void**>(&orig_QuestPresenter_GetState));

        Memory::HookRVA(Offsets::Quest::QuestData_SetState1,
                        reinterpret_cast<void*>(hook_QuestData_SetState1),
                        reinterpret_cast<void**>(&orig_QuestData_SetState1));

        Memory::HookRVA(Offsets::Quest::QuestData_SetState2,
                        reinterpret_cast<void*>(hook_QuestData_SetState2),
                        reinterpret_cast<void**>(&orig_QuestData_SetState2));

        Memory::HookRVA(Offsets::Quest::QuestItemView_UpdateState,
                        reinterpret_cast<void*>(hook_QuestItemView_UpdateState),
                        reinterpret_cast<void**>(&orig_QuestItemView_UpdateState));

        Memory::HookRVA(Offsets::Quest::DailyRepeatingQuestView_UpdateState,
                        reinterpret_cast<void*>(hook_DailyRepeatingQuestView_UpdateState),
                        reinterpret_cast<void**>(&orig_DailyRepeatingQuestView_UpdateState));

        Memory::HookRVA(Offsets::Quest::DailyQuestGroupRewards_GetState,
                        reinterpret_cast<void*>(hook_DailyQuestGroupRewards_GetState),
                        reinterpret_cast<void**>(&orig_DailyQuestGroupRewards_GetState));

        Memory::HookRVA(Offsets::Quest::GroupRewardItemView_CanClaim,
                        reinterpret_cast<void*>(hook_GroupRewardItemView_CanClaim),
                        reinterpret_cast<void**>(&orig_GroupRewardItemView_CanClaim));

        Memory::HookRVA(Offsets::Quest::DailyRewardContainer_Setup,
                        reinterpret_cast<void*>(hook_DailyRewardContainer_Setup),
                        reinterpret_cast<void**>(&orig_DailyRewardContainer_Setup));

        LOG_INFO("[REWARD] RewardBooster & Freeze Claim hooks installed successfully.");
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

    void SetFreezeClaim(bool enable) {
        s_freezeClaim.store(enable, std::memory_order_relaxed);
        LOG_INFO("[REWARD] Freeze Claim (Klaim Berulang): %s", enable ? "ON" : "OFF");
    }

    bool IsFreezeClaim() {
        return s_freezeClaim.load(std::memory_order_relaxed);
    }

} // namespace RewardBooster
