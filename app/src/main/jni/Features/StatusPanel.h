#ifndef FEATURE_STATUS_PANEL_H
#define FEATURE_STATUS_PANEL_H

#include <string>

namespace StatusPanel {

    struct Stats {
        int realPlayerCount;
        int botCount;
        int totalEntities;
        bool inGame;
    };

    void Init();
    void SetEnabled(bool enable);
    bool IsEnabled();

    // Player lifecycle callbacks
    void OnNetworkPlayerUpdate(void *instance);
    void OnNetworkPlayerDestroy(void *instance);
    void OnBotPlayerStart(void *instance);
    void OnBotPlayerUpdate(void *instance);
    void OnBotPlayerDestroy(void *instance);

    // Stats queries for Java JNI
    Stats GetStats();
    std::string GetFormattedStatus();
    void ResetCounters();

} // namespace StatusPanel

#endif // FEATURE_STATUS_PANEL_H
