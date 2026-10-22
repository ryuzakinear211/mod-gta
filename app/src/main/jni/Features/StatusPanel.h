#ifndef FEATURE_STATUS_PANEL_H
#define FEATURE_STATUS_PANEL_H

#include <string>
#include <vector>

namespace StatusPanel {

    struct MemberInfo {
        std::string name;
        bool isBot;
        int team; // 0 = TeamA (Blue), 1 = TeamB (Red), -1 = Unknown
        MemberInfo(const std::string &n = "", bool bot = false, int t = -1)
            : name(n), isBot(bot), team(t) {}
    };

    struct Stats {
        int realPlayerCount;
        int botCount;
        int totalEntities;
        bool inGame;
        std::vector<MemberInfo> blueTeam;
        std::vector<MemberInfo> redTeam;
    };

    void Init();
    void SetEnabled(bool enable);
    bool IsEnabled();

    // Local player hook
    void OnLocalPlayerUpdate(void *instance);

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
        int team;
        std::string name;
        EntitySnapshot(void *obj = nullptr, bool bot = false, void *net = nullptr, int t = -1, const std::string &n = "")
            : object(obj), isBot(bot), netPlayer(net), team(t), name(n) {}
    };

    // Stats and active entity queries
    Stats GetStats();
    std::string GetFormattedStatus();
    std::vector<EntitySnapshot> GetActiveEntities();
    void ResetCounters();
    void OnMatchFinished();
    void OnLocalPlayerDestroy(void *instance);

    // Affiliation and team queries for ESP and Chams
    int GetLocalPlayerTeam();
    void SetLocalPlayerTeam(int team);
    bool IsLocalPlayer(void *entity);
    bool IsEnemy(int entityTeam);
    bool IsEnemyEntity(void *entity, bool isBot = false, void *netPlayer = nullptr);
    bool IsTeammateEntity(void *entity, bool isBot = false, void *netPlayer = nullptr);

} // namespace StatusPanel

#endif // FEATURE_STATUS_PANEL_H
