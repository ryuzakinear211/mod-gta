#include "StatusPanel.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include "BigHead.h"
#include <vector>
#include <mutex>
#include <unordered_map>
#include <set>
#include <chrono>
#include <atomic>
#include <algorithm>

namespace StatusPanel {

    static std::atomic<bool> s_enabled{false};
    static std::mutex s_entityMutex;

    // Entity tracking maps (Instance -> last seen millisecond)
    static std::unordered_map<void*, uint64_t> s_networkPlayers;
    static std::unordered_map<void*, uint64_t> s_botPlayers;
    static std::unordered_map<void*, void*>    s_botNetPlayers;

    // Team and Name tracking
    struct TrackedEntityInfo {
        uint64_t lastSeen;
        std::string name;
        int team; // 0 = TeamA (Blue), 1 = TeamB (Red), -1 = Unknown
        bool isBot;
    };
    static std::unordered_map<void*, TrackedEntityInfo> s_trackedEntities;

    // Local player tracking
    static void *s_localPlayerFPC = nullptr;
    static std::string s_localPlayerName = "";
    static int s_localPlayerTeam = 0; // Default TeamA (Blue)

    // Unity function pointers resolved from libil2cpp.so
    static void* (*Object_GetName)(void*) = nullptr;
    static void* (*Component_GetGameObject)(void*) = nullptr;
    static int   (*BotPlayer_GetTeam)(void*) = nullptr;

    static inline uint64_t GetCurrentTimeMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
    }

    // Throttled heartbeat interval (500ms) to drastically cut mutex contention
    static constexpr uint64_t HEARTBEAT_INTERVAL_MS = 500;
    static constexpr uint64_t TIMEOUT_MS = 2500;

    // Convert IL2CPP System.String to UTF-8 std::string safely
    static std::string Il2CppStringToStdString(void *il2cppString) {
        if (!il2cppString || !Memory::IsValidPtr(il2cppString)) return "";

        int32_t length = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(il2cppString) + 0x10);
        if (length <= 0 || length > 128) return "";

        const uint16_t *chars = reinterpret_cast<const uint16_t*>(reinterpret_cast<uintptr_t>(il2cppString) + 0x14);
        if (!Memory::IsValidPtr(const_cast<uint16_t*>(chars))) return "";

        std::string result;
        result.reserve(length * 2);
        for (int i = 0; i < length; ++i) {
            uint16_t c = chars[i];
            if (c < 0x80) {
                if (c == 0) break;
                result += static_cast<char>(c);
            } else if (c < 0x800) {
                result += static_cast<char>(0xC0 | ((c >> 6) & 0x1F));
                result += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                result += static_cast<char>(0xE0 | ((c >> 12) & 0x0F));
                result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        return result;
    }

    // Resolve name from Unity Component / GameObject
    static std::string ResolveEntityName(void *instance, bool isBot, int id) {
        std::string name;
        if (instance && Memory::IsValidPtr(instance)) {
            // 1. Try Object.get_name(instance)
            if (Object_GetName) {
                try {
                    void *strPtr = Object_GetName(instance);
                    if (strPtr && Memory::IsValidPtr(strPtr)) {
                        name = Il2CppStringToStdString(strPtr);
                    }
                } catch (...) {}
            }

            // 2. Try Component.get_gameObject() -> Object.get_name(gameObject)
            if (name.empty() && Component_GetGameObject && Object_GetName) {
                try {
                    void *go = Component_GetGameObject(instance);
                    if (go && Memory::IsValidPtr(go)) {
                        void *strPtr = Object_GetName(go);
                        if (strPtr && Memory::IsValidPtr(strPtr)) {
                            name = Il2CppStringToStdString(strPtr);
                        }
                    }
                } catch (...) {}
            }
        }

        // Clean up "(Clone)" suffix if present
        size_t clonePos = name.find("(Clone)");
        if (clonePos != std::string::npos) {
            name = name.substr(0, clonePos);
        }

        // Trim spaces
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());

        // Check if name is generic or empty, provide clean fallback
        if (name.empty() || name == "NetworkPlayer" || name == "BotPlayer") {
            if (isBot) {
                name = "Bot_" + std::to_string(id);
            } else {
                name = "Player_" + std::to_string(id);
            }
        }

        return name;
    }

    // Helper: Read bot's team enum (0 = TeamA/Blue, 1 = TeamB/Red, -1 = Unknown)
    static int GetBotTeam(void *botPlayer) {
        if (!botPlayer || !Memory::IsValidPtr(botPlayer)) return -1;

        if (BotPlayer_GetTeam) {
            int t = BotPlayer_GetTeam(botPlayer);
            if (t == 0 || t == 1) return t;
        }

        int t0 = Memory::ReadField<int>(botPlayer, 0xE0);
        if (t0 == 0 || t0 == 1) return t0;

        int t1 = Memory::ReadField<int>(botPlayer, 0xE4);
        if (t1 == 0 || t1 == 1) return t1;

        void *spawnPoint = Memory::ReadField<void*>(botPlayer, 0xB0);
        if (spawnPoint && Memory::IsValidPtr(spawnPoint)) {
            int t2 = Memory::ReadField<int>(spawnPoint, 0x38);
            if (t2 == 0 || t2 == 1) return t2;
        }

        return -1;
    }

    // Helper: Read NetworkPlayer's team enum from TargetInfo (0x30)
    static int GetPlayerTeam(void *netPlayer) {
        if (!netPlayer || !Memory::IsValidPtr(netPlayer)) return -1;
        void *targetInfo = Memory::ReadField<void*>(netPlayer, Offsets::NetworkPlayer::TargetInfo);
        if (targetInfo && Memory::IsValidPtr(targetInfo)) {
            int teamVal = Memory::ReadField<int>(targetInfo, 0x30);
            if (teamVal == 0 || teamVal == 1) return teamVal;
        }
        return -1;
    }

    void OnLocalPlayerUpdate(void *instance) {
        if (!instance) return;
        s_localPlayerFPC = instance;

        if (s_localPlayerName.empty()) {
            // Check string field at FirstPersonController + 0xD8
            void *nameStr = Memory::ReadField<void*>(instance, 0xD8);
            if (nameStr && Memory::IsValidPtr(nameStr)) {
                std::string n = Il2CppStringToStdString(nameStr);
                if (!n.empty() && n != "FirstPersonController") {
                    s_localPlayerName = n;
                }
            }

            if (s_localPlayerName.empty() && Object_GetName) {
                try {
                    void *strPtr = Object_GetName(instance);
                    if (strPtr && Memory::IsValidPtr(strPtr)) {
                        std::string n = Il2CppStringToStdString(strPtr);
                        if (!n.empty() && n != "FirstPersonController") {
                            size_t clonePos = n.find("(Clone)");
                            if (clonePos != std::string::npos) n = n.substr(0, clonePos);
                            s_localPlayerName = n;
                        }
                    }
                } catch (...) {}
            }
        }
    }

    void OnNetworkPlayerUpdate(void *instance) {
        if (!instance) return;

        // BigHead check (runs directly on render thread)
        BigHead::OnNetworkPlayerUpdate(instance);

        uint64_t now = GetCurrentTimeMs();
        std::lock_guard<std::mutex> lock(s_entityMutex);
        auto it = s_networkPlayers.find(instance);
        if (it == s_networkPlayers.end() || (now - it->second) >= HEARTBEAT_INTERVAL_MS) {
            s_networkPlayers[instance] = now;

            auto trackIt = s_trackedEntities.find(instance);
            if (trackIt == s_trackedEntities.end()) {
                TrackedEntityInfo info;
                info.lastSeen = now;
                info.isBot = false;
                info.team = GetPlayerTeam(instance);
                int entityId = static_cast<int>(s_trackedEntities.size() + 1);
                info.name = ResolveEntityName(instance, false, entityId);
                s_trackedEntities[instance] = info;
            } else {
                trackIt->second.lastSeen = now;
                if (trackIt->second.team != 0 && trackIt->second.team != 1) {
                    int t = GetPlayerTeam(instance);
                    if (t == 0 || t == 1) trackIt->second.team = t;
                }
            }
        }
    }

    void OnNetworkPlayerDestroy(void *instance) {
        if (!instance) return;
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_networkPlayers.erase(instance);
        s_trackedEntities.erase(instance);
    }

    void OnBotPlayerStart(void *instance) {
        if (!instance) return;
        uint64_t now = GetCurrentTimeMs();
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_botPlayers[instance] = now;

        void *netPlayer = Memory::ReadField<void*>(instance, Offsets::BotPlayer::NetPlayer);
        if (netPlayer) {
            s_botNetPlayers[instance] = netPlayer;
        }

        TrackedEntityInfo info;
        info.lastSeen = now;
        info.isBot = true;
        info.team = GetBotTeam(instance);
        int entityId = static_cast<int>(s_trackedEntities.size() + 1);
        info.name = ResolveEntityName(instance, true, entityId);
        s_trackedEntities[instance] = info;
    }

    void OnBotPlayerUpdate(void *instance) {
        if (!instance) return;

        // BigHead check
        BigHead::OnBotPlayerUpdate(instance);

        uint64_t now = GetCurrentTimeMs();
        std::lock_guard<std::mutex> lock(s_entityMutex);
        auto it = s_botPlayers.find(instance);
        if (it == s_botPlayers.end() || (now - it->second) >= HEARTBEAT_INTERVAL_MS) {
            s_botPlayers[instance] = now;

            void *netPlayer = Memory::ReadField<void*>(instance, Offsets::BotPlayer::NetPlayer);
            if (netPlayer) {
                s_botNetPlayers[instance] = netPlayer;
            }

            auto trackIt = s_trackedEntities.find(instance);
            if (trackIt == s_trackedEntities.end()) {
                TrackedEntityInfo info;
                info.lastSeen = now;
                info.isBot = true;
                info.team = GetBotTeam(instance);
                int entityId = static_cast<int>(s_trackedEntities.size() + 1);
                info.name = ResolveEntityName(instance, true, entityId);
                s_trackedEntities[instance] = info;
            } else {
                trackIt->second.lastSeen = now;
                if (trackIt->second.team != 0 && trackIt->second.team != 1) {
                    int t = GetBotTeam(instance);
                    if (t == 0 || t == 1) trackIt->second.team = t;
                }
            }
        }
    }

    void OnBotPlayerDestroy(void *instance) {
        if (!instance) return;
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_botPlayers.erase(instance);
        s_botNetPlayers.erase(instance);
        s_trackedEntities.erase(instance);
    }

    Stats GetStats() {
        std::lock_guard<std::mutex> lock(s_entityMutex);
        uint64_t now = GetCurrentTimeMs();

        std::set<void*> activeBotNets;
        for (auto it = s_botPlayers.begin(); it != s_botPlayers.end(); ) {
            if (now - it->second > TIMEOUT_MS) {
                s_botNetPlayers.erase(it->first);
                s_trackedEntities.erase(it->first);
                it = s_botPlayers.erase(it);
            } else {
                auto netIt = s_botNetPlayers.find(it->first);
                if (netIt != s_botNetPlayers.end()) {
                    activeBotNets.insert(netIt->second);
                }
                ++it;
            }
        }

        int totalNetPlayers = 0;
        int realCount = 0;
        for (auto it = s_networkPlayers.begin(); it != s_networkPlayers.end(); ) {
            if (now - it->second > TIMEOUT_MS) {
                s_trackedEntities.erase(it->first);
                it = s_networkPlayers.erase(it);
            } else {
                totalNetPlayers++;
                if (activeBotNets.find(it->first) == activeBotNets.end()) {
                    realCount++;
                }
                ++it;
            }
        }

        // Clean stale tracked entities
        for (auto it = s_trackedEntities.begin(); it != s_trackedEntities.end(); ) {
            if (now - it->second.lastSeen > TIMEOUT_MS) {
                it = s_trackedEntities.erase(it);
            } else {
                ++it;
            }
        }

        int botCount = static_cast<int>(s_botPlayers.size());
        if (totalNetPlayers >= botCount) {
            if (realCount == 0 && totalNetPlayers > botCount) {
                realCount = totalNetPlayers - botCount;
            }
        } else {
            if (realCount < 0) realCount = 0;
        }

        // Build Blue Team (0) and Red Team (1) lists
        std::vector<MemberInfo> blueList;
        std::vector<MemberInfo> redList;
        std::vector<MemberInfo> unassigned;

        bool localPlayerFound = false;

        for (const auto &pair : s_trackedEntities) {
            void *entityObj = pair.first;
            const TrackedEntityInfo &info = pair.second;

            // Avoid double counting bots registered under NetworkPlayer
            if (!info.isBot && activeBotNets.find(entityObj) != activeBotNets.end()) {
                continue;
            }

            if (s_localPlayerFPC && entityObj == s_localPlayerFPC) {
                localPlayerFound = true;
            }

            MemberInfo m(info.name, info.isBot, info.team);
            if (info.team == 0) {
                blueList.push_back(m);
            } else if (info.team == 1) {
                redList.push_back(m);
            } else {
                unassigned.push_back(m);
            }
        }

        // Distribute unassigned entities evenly between teams
        for (const auto &m : unassigned) {
            if (blueList.size() <= redList.size()) {
                blueList.push_back(MemberInfo(m.name, m.isBot, 0));
            } else {
                redList.push_back(MemberInfo(m.name, m.isBot, 1));
            }
        }

        // Include local player if in game but not in remote network players
        if (s_localPlayerFPC && !localPlayerFound) {
            std::string myName = s_localPlayerName.empty() ? "You" : (s_localPlayerName + " (You)");
            MemberInfo myMember(myName, false, s_localPlayerTeam);
            if (s_localPlayerTeam == 1) {
                redList.insert(redList.begin(), myMember);
            } else {
                blueList.insert(blueList.begin(), myMember);
            }
            realCount++;
        }

        int total = (totalNetPlayers > botCount) ? totalNetPlayers : (realCount + botCount);

        Stats stats;
        stats.realPlayerCount = realCount;
        stats.botCount = botCount;
        stats.totalEntities = total;
        stats.inGame = (total > 0);
        stats.blueTeam = blueList;
        stats.redTeam = redList;
        return stats;
    }

    std::string GetFormattedStatus() {
        Stats s = GetStats();

        // Build blue team member string: "name1,name2,name3"
        std::string blueStr;
        for (size_t i = 0; i < s.blueTeam.size(); ++i) {
            if (i > 0) blueStr += ",";
            blueStr += s.blueTeam[i].name;
            if (s.blueTeam[i].isBot) {
                blueStr += " [BOT]";
            }
        }

        // Build red team member string: "name1,name2,name3"
        std::string redStr;
        for (size_t i = 0; i < s.redTeam.size(); ++i) {
            if (i > 0) redStr += ",";
            redStr += s.redTeam[i].name;
            if (s.redTeam[i].isBot) {
                redStr += " [BOT]";
            }
        }

        char header[256];
        snprintf(header, sizeof(header), "%s;%d;%d;%d;DISABLED;",
                 s.inGame ? "IN_GAME" : "LOBBY",
                 s.realPlayerCount, s.botCount, s.totalEntities);

        return std::string(header) + blueStr + ";" + redStr;
    }

    std::vector<EntitySnapshot> GetActiveEntities() {
        std::vector<EntitySnapshot> result;
        std::lock_guard<std::mutex> lock(s_entityMutex);
        uint64_t now = GetCurrentTimeMs();

        std::set<void*> activeBotNets;
        for (auto it = s_botPlayers.begin(); it != s_botPlayers.end(); ) {
            if (now - it->second > TIMEOUT_MS) {
                s_botNetPlayers.erase(it->first);
                s_trackedEntities.erase(it->first);
                it = s_botPlayers.erase(it);
            } else {
                void *botObj = it->first;
                void *netPlayer = nullptr;
                auto netIt = s_botNetPlayers.find(botObj);
                if (netIt != s_botNetPlayers.end()) {
                    netPlayer = netIt->second;
                } else {
                    netPlayer = Memory::ReadField<void*>(botObj, Offsets::BotPlayer::NetPlayer);
                    if (netPlayer) s_botNetPlayers[botObj] = netPlayer;
                }
                if (netPlayer) {
                    activeBotNets.insert(netPlayer);
                }

                int team = -1;
                std::string name;
                auto trackIt = s_trackedEntities.find(botObj);
                if (trackIt != s_trackedEntities.end()) {
                    team = trackIt->second.team;
                    name = trackIt->second.name;
                }

                result.push_back(EntitySnapshot(botObj, true, netPlayer, team, name));
                ++it;
            }
        }

        for (auto it = s_networkPlayers.begin(); it != s_networkPlayers.end(); ) {
            if (now - it->second > TIMEOUT_MS) {
                s_trackedEntities.erase(it->first);
                it = s_networkPlayers.erase(it);
            } else {
                void *netObj = it->first;
                if (activeBotNets.find(netObj) == activeBotNets.end()) {
                    int team = -1;
                    std::string name;
                    auto trackIt = s_trackedEntities.find(netObj);
                    if (trackIt != s_trackedEntities.end()) {
                        team = trackIt->second.team;
                        name = trackIt->second.name;
                    }
                    result.push_back(EntitySnapshot(netObj, false, netObj, team, name));
                }
                ++it;
            }
        }

        return result;
    }

    void ResetCounters() {
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_networkPlayers.clear();
        s_botPlayers.clear();
        s_botNetPlayers.clear();
        s_trackedEntities.clear();
        LOG_INFO("[STATS] Entity counters and team tracking manually reset.");
    }

    // =========================================================================
    // Hooks
    // =========================================================================

    // NetworkPlayer.Update: RVA 0x42CC8C8
    static void (*old_NetworkPlayer_Update)(void *instance) = nullptr;
    static void hook_NetworkPlayer_Update(void *instance) {
        if (instance) {
            OnNetworkPlayerUpdate(instance);
        }
        if (old_NetworkPlayer_Update) old_NetworkPlayer_Update(instance);
    }

    // NetworkPlayer.OnDestroy: RVA 0x42C9540
    static void (*old_NetworkPlayer_OnDestroy)(void *instance) = nullptr;
    static void hook_NetworkPlayer_OnDestroy(void *instance) {
        if (instance) {
            OnNetworkPlayerDestroy(instance);
        }
        if (old_NetworkPlayer_OnDestroy) old_NetworkPlayer_OnDestroy(instance);
    }

    // BotPlayer.Start: RVA 0x44493F4
    static void (*old_BotPlayer_Start)(void *instance) = nullptr;
    static void hook_BotPlayer_Start(void *instance) {
        if (instance) {
            OnBotPlayerStart(instance);
        }
        if (old_BotPlayer_Start) old_BotPlayer_Start(instance);
    }

    // BotPlayer.Update: RVA 0x444A310
    static void (*old_BotPlayer_Update)(void *instance) = nullptr;
    static void hook_BotPlayer_Update(void *instance) {
        if (instance) {
            OnBotPlayerUpdate(instance);
        }
        if (old_BotPlayer_Update) old_BotPlayer_Update(instance);
    }

    // BotPlayer.OnDestroy: RVA 0x4449EB0
    static void (*old_BotPlayer_OnDestroy)(void *instance) = nullptr;
    static void hook_BotPlayer_OnDestroy(void *instance) {
        if (instance) {
            OnBotPlayerDestroy(instance);
        }
        if (old_BotPlayer_OnDestroy) old_BotPlayer_OnDestroy(instance);
    }

    void Init() {
        uintptr_t libBase = Memory::GetLibBase();
        if (libBase) {
            Object_GetName = reinterpret_cast<void*(*)(void*)>(libBase + Offsets::Unity::Object_GetName);
            Component_GetGameObject = reinterpret_cast<void*(*)(void*)>(libBase + Offsets::Unity::Component_GetGameObject);
            BotPlayer_GetTeam = reinterpret_cast<int(*)(void*)>(libBase + Offsets::BotPlayer::GetTeam);
        }

        Memory::HookRVA(Offsets::NetworkPlayer::Update,    reinterpret_cast<void*>(hook_NetworkPlayer_Update),    reinterpret_cast<void**>(&old_NetworkPlayer_Update));
        Memory::HookRVA(Offsets::NetworkPlayer::OnDestroy, reinterpret_cast<void*>(hook_NetworkPlayer_OnDestroy), reinterpret_cast<void**>(&old_NetworkPlayer_OnDestroy));
        Memory::HookRVA(Offsets::BotPlayer::Start,         reinterpret_cast<void*>(hook_BotPlayer_Start),         reinterpret_cast<void**>(&old_BotPlayer_Start));
        Memory::HookRVA(Offsets::BotPlayer::Update,        reinterpret_cast<void*>(hook_BotPlayer_Update),        reinterpret_cast<void**>(&old_BotPlayer_Update));
        Memory::HookRVA(Offsets::BotPlayer::OnDestroy,     reinterpret_cast<void*>(hook_BotPlayer_OnDestroy),     reinterpret_cast<void**>(&old_BotPlayer_OnDestroy));

        LOG_INFO("[STATS] StatusPanel entity lifecycle & team tracking hooks installed.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[STATS] StatusPanel toggle: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace StatusPanel
