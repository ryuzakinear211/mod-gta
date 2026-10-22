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

namespace StatusPanel {

    static std::atomic<bool> s_enabled{false};
    static std::mutex s_entityMutex;

    // Entity tracking maps (Instance -> last seen millisecond)
    static std::unordered_map<void*, uint64_t> s_networkPlayers;
    static std::unordered_map<void*, uint64_t> s_botPlayers;
    static std::unordered_map<void*, void*>    s_botNetPlayers;

    static inline uint64_t GetCurrentTimeMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
    }

    // Throttled heartbeat interval (500ms) to drastically cut mutex contention
    static constexpr uint64_t HEARTBEAT_INTERVAL_MS = 500;
    static constexpr uint64_t TIMEOUT_MS = 2500;

    void OnNetworkPlayerUpdate(void *instance) {
        if (!instance) return;

        // BigHead check (runs directly on render thread)
        BigHead::OnNetworkPlayerUpdate(instance);

        // Keep entity tracking active continuously in background so re-enabling HUD displays stats instantly
        uint64_t now = GetCurrentTimeMs();
        std::lock_guard<std::mutex> lock(s_entityMutex);
        auto it = s_networkPlayers.find(instance);
        if (it == s_networkPlayers.end() || (now - it->second) >= HEARTBEAT_INTERVAL_MS) {
            s_networkPlayers[instance] = now;
        }
    }

    void OnNetworkPlayerDestroy(void *instance) {
        if (!instance) return;
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_networkPlayers.erase(instance);
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
        }
    }

    void OnBotPlayerDestroy(void *instance) {
        if (!instance) return;
        std::lock_guard<std::mutex> lock(s_entityMutex);
        s_botPlayers.erase(instance);
        s_botNetPlayers.erase(instance);
    }

    Stats GetStats() {
        std::lock_guard<std::mutex> lock(s_entityMutex);
        uint64_t now = GetCurrentTimeMs();

        std::set<void*> activeBotNets;
        for (auto it = s_botPlayers.begin(); it != s_botPlayers.end(); ) {
            if (now - it->second > TIMEOUT_MS) {
                s_botNetPlayers.erase(it->first);
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
                it = s_networkPlayers.erase(it);
            } else {
                totalNetPlayers++;
                if (activeBotNets.find(it->first) == activeBotNets.end()) {
                    realCount++;
                }
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

        int total = (totalNetPlayers > botCount) ? totalNetPlayers : (realCount + botCount);

        Stats stats;
        stats.realPlayerCount = realCount;
        stats.botCount = botCount;
        stats.totalEntities = total;
        stats.inGame = (total > 0);
        return stats;
    }

    std::string GetFormattedStatus() {
        Stats s = GetStats();
        char buf[256];
        snprintf(buf, sizeof(buf), "%s;%d;%d;%d;DISABLED",
                 s.inGame ? "IN_GAME" : "LOBBY",
                 s.realPlayerCount, s.botCount, s.totalEntities);
        return std::string(buf);
    }

    std::vector<EntitySnapshot> GetActiveEntities() {
        std::vector<EntitySnapshot> result;
        std::lock_guard<std::mutex> lock(s_entityMutex);
        uint64_t now = GetCurrentTimeMs();

        std::set<void*> activeBotNets;
        for (auto it = s_botPlayers.begin(); it != s_botPlayers.end(); ) {
            if (now - it->second > TIMEOUT_MS) {
                s_botNetPlayers.erase(it->first);
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
                result.push_back(EntitySnapshot(botObj, true, netPlayer));
                ++it;
            }
        }

        for (auto it = s_networkPlayers.begin(); it != s_networkPlayers.end(); ) {
            if (now - it->second > TIMEOUT_MS) {
                it = s_networkPlayers.erase(it);
            } else {
                void *netObj = it->first;
                if (activeBotNets.find(netObj) == activeBotNets.end()) {
                    result.push_back(EntitySnapshot(netObj, false, netObj));
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
        LOG_INFO("[STATS] Entity counters manually reset.");
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
        Memory::HookRVA(Offsets::NetworkPlayer::Update,    reinterpret_cast<void*>(hook_NetworkPlayer_Update),    reinterpret_cast<void**>(&old_NetworkPlayer_Update));
        Memory::HookRVA(Offsets::NetworkPlayer::OnDestroy, reinterpret_cast<void*>(hook_NetworkPlayer_OnDestroy), reinterpret_cast<void**>(&old_NetworkPlayer_OnDestroy));
        Memory::HookRVA(Offsets::BotPlayer::Start,         reinterpret_cast<void*>(hook_BotPlayer_Start),         reinterpret_cast<void**>(&old_BotPlayer_Start));
        Memory::HookRVA(Offsets::BotPlayer::Update,        reinterpret_cast<void*>(hook_BotPlayer_Update),        reinterpret_cast<void**>(&old_BotPlayer_Update));
        Memory::HookRVA(Offsets::BotPlayer::OnDestroy,     reinterpret_cast<void*>(hook_BotPlayer_OnDestroy),     reinterpret_cast<void**>(&old_BotPlayer_OnDestroy));

        LOG_INFO("[STATS] StatusPanel entity lifecycle hooks installed.");
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[STATS] StatusPanel toggle: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace StatusPanel
