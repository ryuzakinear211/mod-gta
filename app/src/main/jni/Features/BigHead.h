#ifndef FEATURE_BIG_HEAD_H
#define FEATURE_BIG_HEAD_H

namespace BigHead {

    void Init();
    void SetEnabled(bool enable);
    bool IsEnabled();

    // Invoked from player update hooks
    void OnNetworkPlayerUpdate(void *netPlayer);
    void OnBotPlayerUpdate(void *botPlayer);

} // namespace BigHead

#endif // FEATURE_BIG_HEAD_H
