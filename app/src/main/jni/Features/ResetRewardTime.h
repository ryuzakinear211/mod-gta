#ifndef FEATURE_RESET_REWARD_TIME_H
#define FEATURE_RESET_REWARD_TIME_H

namespace ResetRewardTime {

    // Initializes hooks for Free Reward / Gold timer cooldown reset (1h 57m -> 0s)
    void Init();

    // Toggle reset reward timer on / off
    void SetEnabled(bool enable);
    bool IsEnabled();

} // namespace ResetRewardTime

#endif // FEATURE_RESET_REWARD_TIME_H
