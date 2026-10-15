#ifndef FEATURE_REWARD_BOOSTER_H
#define FEATURE_REWARD_BOOSTER_H

#include <cstdint>

namespace RewardBooster {
    void Init();
    void SetEnabled(bool enable);
    bool IsEnabled();
    void SetTargetAmount(int amount);
    int GetTargetAmount();
}

#endif // FEATURE_REWARD_BOOSTER_H
