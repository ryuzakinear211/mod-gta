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
    static void *s_localPlayerNet = nullptr;
    static std::string s_localPlayerName = "";
    static int s_localPlayerTeam = 0; // Default TeamA (Blue)

    // Unity function pointers resolved from libil2cpp.so
    static void* (*Object_GetName)(void*) = nullptr;
    static void* (*Component_GetGameObject)(void*) = nullptr;
    static int   (*BotPlayer_GetTeam)(void*) = nullptr;
    static void* (*Text_GetText)(void*) = nullptr;

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
        uintptr_t cAddr = reinterpret_cast<uintptr_t>(chars);
        // Valid user-space pointer boundary with at least 2-byte alignment for uint16_t characters
        if (cAddr < 0x10000000ULL || cAddr > 0x00007FFFFFFFFFFFULL || (cAddr & 0x1) != 0) return "";

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

    // Helper: Clean up "(Clone)" suffix and extra whitespace
    static inline void CleanEntityName(std::string &name) {
        size_t clonePos = name.find("(Clone)");
        if (clonePos != std::string::npos) {
            name = name.substr(0, clonePos);
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n')) name.pop_back();
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '\r' || name.front() == '\n')) name.erase(name.begin());
    }

    // Helper: Verify name is an actual player username and not a generic class/prefab name
    static inline bool IsValidCustomName(const std::string &name) {
        if (name.empty()) return false;
        std::string lower;
        lower.reserve(name.size());
        for (char c : name) lower += static_cast<char>(tolower(c));

        // Exact matches
        if (lower == "networkplayer" || lower == "botplayer" || 
            lower == "firstpersoncontroller" || lower == "thirdpersoncontroller" ||
            lower == "player" || lower == "bot" || lower == "gameobject" || lower == "default" ||
            lower == "firstpersondoll" || lower == "thirdpersondoll" ||
            lower == "botdollmanager" || lower == "botdoll" ||
            lower == "network fps player" || lower == "networkfpsplayer" ||
            lower == "fps player" || lower == "fpsplayer") {
            return false;
        }

        // Substring checks for engine / prefab names
        if (lower.find("doll") != std::string::npos || 
            lower.find("network fps") != std::string::npos ||
            lower.find("networkfps") != std::string::npos ||
            lower.find("fps player") != std::string::npos ||
            lower.find("botdoll") != std::string::npos ||
            lower.find("controller") != std::string::npos) {
            return false;
        }

        return true;
    }

    // Helper: Detect whether a NetworkPlayer belongs to the local player
    static inline bool IsLocalNetworkPlayer(void *netPlayer) {
        if (!netPlayer || !Memory::IsValidPtr(netPlayer)) return false;
        if (s_localPlayerNet && netPlayer == s_localPlayerNet) return true;

        // 1. Check DollsManager.FirstPersonController (offset 0x40)
        void *dolls = Memory::ReadField<void*>(netPlayer, Offsets::NetworkPlayer::DollsManager);
        if (dolls && Memory::IsValidPtr(dolls)) {
            void *fpc = Memory::ReadField<void*>(dolls, 0x40);
            if (fpc && Memory::IsValidPtr(fpc)) {
                s_localPlayerFPC = fpc;
                return true;
            }
        }

        // 2. Check GameObject name: local player's network GameObject is named "network fps player"
        if (Component_GetGameObject && Object_GetName) {
            try {
                void *go = Component_GetGameObject(netPlayer);
                if (go && Memory::IsValidPtr(go)) {
                    void *strPtr = Object_GetName(go);
                    if (strPtr && Memory::IsValidPtr(strPtr)) {
                        std::string n = Il2CppStringToStdString(strPtr);
                        std::string lower;
                        lower.reserve(n.size());
                        for (char c : n) lower += static_cast<char>(tolower(c));
                        if (lower.find("network fps") != std::string::npos || 
                            lower.find("networkfps") != std::string::npos ||
                            lower.find("fps player") != std::string::npos ||
                            lower.find("firstpersondoll") != std::string::npos) {
                            return true;
                        }
                    }
                }
            } catch (...) {}
        }

        return false;
    }

    // Resolve real in-game username from IL2CPP object hierarchy
    static std::string ResolvePlayerName(void *instance, bool isBot) {
        if (!instance || !Memory::IsValidPtr(instance)) return "";

        void *tpCtrl = nullptr;

        if (isBot) {
            // BotPlayer -> ThirdPersonController (0x58)
            tpCtrl = Memory::ReadField<void*>(instance, Offsets::BotPlayer::ThirdPersonCtrl);
            if (!tpCtrl || !Memory::IsValidPtr(tpCtrl)) {
                void *net = Memory::ReadField<void*>(instance, Offsets::BotPlayer::NetPlayer);
                if (net && Memory::IsValidPtr(net)) {
                    void *dolls = Memory::ReadField<void*>(net, Offsets::NetworkPlayer::DollsManager);
                    if (dolls && Memory::IsValidPtr(dolls)) {
                        tpCtrl = Memory::ReadField<void*>(dolls, Offsets::Dolls::BotTPCtrl);
                        if (!tpCtrl || !Memory::IsValidPtr(tpCtrl)) {
                            tpCtrl = Memory::ReadField<void*>(dolls, Offsets::Dolls::RemoteTPCtrl);
                        }
                    }
                }
            }
        } else {
            // NetworkPlayer -> DollsManager (0x88) -> RemoteTPCtrl (0x50) / BotTPCtrl (0x60)
            void *dolls = Memory::ReadField<void*>(instance, Offsets::NetworkPlayer::DollsManager);
            if (dolls && Memory::IsValidPtr(dolls)) {
                tpCtrl = Memory::ReadField<void*>(dolls, Offsets::Dolls::RemoteTPCtrl);
                if (!tpCtrl || !Memory::IsValidPtr(tpCtrl)) {
                    tpCtrl = Memory::ReadField<void*>(dolls, Offsets::Dolls::BotTPCtrl);
                }
            }
        }

        // 1. Traverse ThirdPersonController -> SceneTitle
        if (tpCtrl && Memory::IsValidPtr(tpCtrl)) {
            void *sceneTitle = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::SceneTitle); // 0x40
            if (!sceneTitle || !Memory::IsValidPtr(sceneTitle)) {
                sceneTitle = Memory::ReadField<void*>(tpCtrl, Offsets::Dolls::RaidAllySceneTitle); // 0x48
            }

            if (sceneTitle && Memory::IsValidPtr(sceneTitle)) {
                // Method A: Direct MainData username string at sceneTitle + 0x40
                void *nameStr = Memory::ReadField<void*>(sceneTitle, Offsets::SceneTitle::MainDataName);
                if (nameStr && Memory::IsValidPtr(nameStr)) {
                    std::string n = Il2CppStringToStdString(nameStr);
                    CleanEntityName(n);
                    if (IsValidCustomName(n)) return n;
                }

                // Method B: CanvasTitleController (0x28) -> CanvasTitleRenderer (0x20) -> Text (0x40) -> m_Text (0xE0)
                void *canvasCtrl = Memory::ReadField<void*>(sceneTitle, Offsets::SceneTitle::CanvasTitleController);
                if (canvasCtrl && Memory::IsValidPtr(canvasCtrl)) {
                    void *renderer = Memory::ReadField<void*>(canvasCtrl, Offsets::SceneTitle::CanvasTitleRenderer);
                    if (renderer && Memory::IsValidPtr(renderer)) {
                        void *textField = Memory::ReadField<void*>(renderer, Offsets::SceneTitle::TextField);
                        if (textField && Memory::IsValidPtr(textField)) {
                            if (Text_GetText) {
                                try {
                                    void *tStr = Text_GetText(textField);
                                    if (tStr && Memory::IsValidPtr(tStr)) {
                                        std::string n = Il2CppStringToStdString(tStr);
                                        CleanEntityName(n);
                                        if (IsValidCustomName(n)) return n;
                                    }
                                } catch (...) {}
                            }
                            void *tStr = Memory::ReadField<void*>(textField, Offsets::SceneTitle::TextString);
                            if (tStr && Memory::IsValidPtr(tStr)) {
                                std::string n = Il2CppStringToStdString(tStr);
                                CleanEntityName(n);
                                if (IsValidCustomName(n)) return n;
                            }
                        }
                    }
                }
            }
        }

        // 2. Try Unity Object.get_name(instance)
        if (Object_GetName) {
            try {
                void *strPtr = Object_GetName(instance);
                if (strPtr && Memory::IsValidPtr(strPtr)) {
                    std::string n = Il2CppStringToStdString(strPtr);
                    CleanEntityName(n);
                    if (IsValidCustomName(n)) return n;
                }
            } catch (...) {}
        }

        // 3. Try Component.get_gameObject() -> Object.get_name(gameObject)
        if (Component_GetGameObject && Object_GetName) {
            try {
                void *go = Component_GetGameObject(instance);
                if (go && Memory::IsValidPtr(go)) {
                    void *strPtr = Object_GetName(go);
                    if (strPtr && Memory::IsValidPtr(strPtr)) {
                        std::string n = Il2CppStringToStdString(strPtr);
                        CleanEntityName(n);
                        if (IsValidCustomName(n)) return n;
                    }
                }
            } catch (...) {}
        }

        return "";
    }

    // Resolve name with graceful numeric fallback
    static std::string ResolveEntityName(void *instance, bool isBot, int id) {
        std::string name = ResolvePlayerName(instance, isBot);
        if (!name.empty()) {
            return name;
        }

        if (isBot) {
            return "Bot_" + std::to_string(id);
        } else {
            return "Player_" + std::to_string(id);
        }
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

        if (s_localPlayerName.empty() || s_localPlayerName == "You") {
            // Check string field at FirstPersonController + 0xD8
            void *nameStr = Memory::ReadField<void*>(instance, Offsets::FPC::NameField);
            if (nameStr && Memory::IsValidPtr(nameStr)) {
                std::string n = Il2CppStringToStdString(nameStr);
                CleanEntityName(n);
                if (IsValidCustomName(n)) {
                    s_localPlayerName = n;
                }
            }
        }
    }

    void OnNetworkPlayerUpdate(void *instance) {
        if (!instance) return;

        // BigHead check (runs directly on render thread)
        BigHead::OnNetworkPlayerUpdate(instance);

        // Check if this NetworkPlayer belongs to the local player!
        if (IsLocalNetworkPlayer(instance)) {
            s_localPlayerNet = instance;
            int team = GetPlayerTeam(instance);
            if (team == 0 || team == 1) {
                s_localPlayerTeam = team;
            }
            std::lock_guard<std::mutex> lock(s_entityMutex);
            s_networkPlayers.erase(instance);
            s_trackedEntities.erase(instance);
            return; // CRITICAL: NEVER register local player as a remote entity!
        }

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
                // If name is currently fallback "Player_#" or invalid, attempt re-resolving real username
                if (trackIt->second.name.empty() || 
                    trackIt->second.name.rfind("Player_", 0) == 0 || 
                    !IsValidCustomName(trackIt->second.name)) {
                    std::string newName = ResolvePlayerName(instance, false);
                    if (!newName.empty() && IsValidCustomName(newName)) {
                        trackIt->second.name = newName;
                    }
                }
            }
        }
    }

    void OnNetworkPlayerDestroy(void *instance) {
        if (!instance) return;
        if (instance == s_localPlayerNet) {
            s_localPlayerNet = nullptr;
        }
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
                // If name is currently fallback "Bot_#" or invalid, keep re-resolving every heartbeat!
                if (trackIt->second.name.empty() || 
                    trackIt->second.name.rfind("Bot_", 0) == 0 || 
                    !IsValidCustomName(trackIt->second.name)) {
                    std::string newName = ResolvePlayerName(instance, true);
                    if (!newName.empty() && IsValidCustomName(newName)) {
                        trackIt->second.name = newName;
                    }
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
            // Safety: if local player somehow got into s_networkPlayers, purge it
            if (it->first == s_localPlayerNet || IsLocalNetworkPlayer(it->first)) {
                s_trackedEntities.erase(it->first);
                it = s_networkPlayers.erase(it);
                continue;
            }

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

        // Clean stale tracked entities and any local player entity
        for (auto it = s_trackedEntities.begin(); it != s_trackedEntities.end(); ) {
            if (it->first == s_localPlayerNet || it->first == s_localPlayerFPC || IsLocalNetworkPlayer(it->first)) {
                it = s_trackedEntities.erase(it);
                continue;
            }
            if (now - it->second.lastSeen > TIMEOUT_MS) {
                it = s_trackedEntities.erase(it);
            } else {
                ++it;
            }
        }

        int botCount = static_cast<int>(s_botPlayers.size());

        // When there are no remote network players, no bots, AND no local player FPC: match is inactive / in lobby
        if (!s_localPlayerFPC && totalNetPlayers == 0 && botCount == 0) {
            Stats stats;
            stats.realPlayerCount = 0;
            stats.botCount = 0;
            stats.totalEntities = 0;
            stats.inGame = false;
            stats.blueTeam.clear();
            stats.redTeam.clear();
            return stats;
        }

        // Build Blue Team (0) and Red Team (1) lists
        std::vector<MemberInfo> blueList;
        std::vector<MemberInfo> redList;
        std::vector<MemberInfo> unassigned;

        for (const auto &pair : s_trackedEntities) {
            void *entityObj = pair.first;
            const TrackedEntityInfo &info = pair.second;

            // Extra safety: skip local player and invalid entity names
            if (entityObj == s_localPlayerNet || entityObj == s_localPlayerFPC || IsLocalNetworkPlayer(entityObj)) {
                continue;
            }
            if (!info.isBot && !IsValidCustomName(info.name)) {
                continue;
            }

            // Avoid double counting bots registered under NetworkPlayer
            if (!info.isBot && activeBotNets.find(entityObj) != activeBotNets.end()) {
                continue;
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

        // Include local player exactly once
        if (s_localPlayerFPC) {
            std::string myName = (s_localPlayerName.empty() || !IsValidCustomName(s_localPlayerName)) ? "You" : (s_localPlayerName + " (You)");
            MemberInfo myMember(myName, false, s_localPlayerTeam);
            if (s_localPlayerTeam == 1) {
                redList.insert(redList.begin(), myMember);
            } else {
                blueList.insert(blueList.begin(), myMember);
            }
            realCount++;
        }

        int total = realCount + botCount;

        Stats stats;
        stats.realPlayerCount = realCount;
        stats.botCount = botCount;
        stats.totalEntities = total;
        stats.inGame = (total > 0 || s_localPlayerFPC != nullptr);
        stats.blueTeam = blueList;
        stats.redTeam = redList;
        return stats;
    }

    std::string GetFormattedStatus() {
        Stats s = GetStats();
        if (!s.inGame) {
            return "LOBBY;0;0;0;DISABLED;;";
        }

        // Build blue team member string: "name1,name2,name3"
        std::string blueStr;
        for (size_t i = 0; i < s.blueTeam.size(); ++i) {
            if (i > 0) blueStr += ",";
            std::string n = s.blueTeam[i].name;
            if (s.blueTeam[i].isBot) {
                if (n.find("(bot)") == std::string::npos && n.find("(Bot)") == std::string::npos) {
                    n += " (bot)";
                }
                n += " [BOT]";
            }
            blueStr += n;
        }

        // Build red team member string: "name1,name2,name3"
        std::string redStr;
        for (size_t i = 0; i < s.redTeam.size(); ++i) {
            if (i > 0) redStr += ",";
            std::string n = s.redTeam[i].name;
            if (s.redTeam[i].isBot) {
                if (n.find("(bot)") == std::string::npos && n.find("(Bot)") == std::string::npos) {
                    n += " (bot)";
                }
                n += " [BOT]";
            }
            redStr += n;
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
            if (it->first == s_localPlayerNet || IsLocalNetworkPlayer(it->first)) {
                it = s_networkPlayers.erase(it);
                continue;
            }
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

    void OnMatchFinished() {
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_networkPlayers.clear();
        s_botPlayers.clear();
        s_botNetPlayers.clear();
        s_trackedEntities.clear();
        s_localPlayerFPC = nullptr;
        s_localPlayerNet = nullptr;
        s_localPlayerName = "";
        s_localPlayerTeam = 0;
        LOG_INFO("[STATS] Match finished / scene change: entity cache and local player reset.");
    }

    void OnLocalPlayerDestroy(void *instance) {
        std::lock_guard<std::mutex> lock(s_entityMutex);
        if (s_localPlayerFPC == instance || instance == nullptr) {
            s_localPlayerFPC = nullptr;
            LOG_INFO("[STATS] Local player FPC destroyed (death/respawn).");
        }
    }

    void ResetCounters() {
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_networkPlayers.clear();
        s_botPlayers.clear();
        s_botNetPlayers.clear();
        s_trackedEntities.clear();
        s_localPlayerFPC = nullptr;
        s_localPlayerNet = nullptr;
        s_localPlayerName = "";
        s_localPlayerTeam = 0;
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
            Text_GetText = reinterpret_cast<void*(*)(void*)>(libBase + Offsets::Unity::Text_GetText);
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
