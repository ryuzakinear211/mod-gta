#ifndef NOTIFICATION_REWARD_H
#define NOTIFICATION_REWARD_H

#include <jni.h>

namespace NotificationReward {

    // Initializes hooks and resolves function pointers for notification permission reward flow
    void Init();

    // Resets notification claim flags in Settings and PlayerPrefs,
    // and re-invokes the notification permission reward callback / screen.
    bool TriggerReward(JNIEnv *env = nullptr, jobject ctx = nullptr);

    // Returns total count of re-claims triggered in current session
    int GetClaimCount();

} // namespace NotificationReward

#endif // NOTIFICATION_REWARD_H
