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

    // Entity snapshot for ESP and Aim features
    struct EntitySnapshot {
        void *object;
        bool isBot;
        void *netPlayer;
        EntitySnapshot(void *obj = nullptr, bool bot = false, void *net = nullptr)
            : object(obj), isBot(bot), netPlayer(net) {}
    };

    // Stats and active entity queries
    Stats GetStats();
    std::string GetFormattedStatus();
    std::vector<EntitySnapshot> GetActiveEntities();
    void ResetCounters();

} // namespace StatusPanel

#endif // FEATURE_STATUS_PANEL_H
