#ifndef NOTIFICATION_REWARD_H
#define NOTIFICATION_REWARD_H

#include <jni.h>

namespace NotificationReward {

    // Initializes hooks and resolves function pointers for notification permission reward flow
    void Init();

    // Called on the engine / render thread (e.g. from glDrawElements or FPC_Update)
    // to execute any queued triggers safely on the Unity main thread.
    void OnRenderFrame();

    // Queues a reset and re-claim of the notification reward to be executed on the engine thread.
    bool TriggerReward(JNIEnv *env = nullptr, jobject ctx = nullptr);

    // Returns total count of re-claims triggered in current session
    int GetClaimCount();

} // namespace NotificationReward

#endif // NOTIFICATION_REWARD_H
