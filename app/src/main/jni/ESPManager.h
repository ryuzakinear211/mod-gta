#ifndef ESP_MANAGER_H
#define ESP_MANAGER_H

#include <vector>
#include <mutex>
#include <atomic>
#include <string>
#include <cmath>
#include <algorithm>
#include "hook.h"

// =========================================================================
// Real-time ESP Entity Types & Drawing Data
// =========================================================================

enum ESPEntityType {
    ESP_TYPE_NONE          = 0,
    ESP_TYPE_ENEMY_PLAYER  = 1,
    ESP_TYPE_ENEMY_BOT     = 2,
    ESP_TYPE_ALLY_PLAYER   = 3,
    ESP_TYPE_ALLY_BOT      = 4
};

// 8x 32-bit floats per entity (32 bytes per record) - strictly matching ESPView.java float array
struct ESPEntityData {
    float type;          // ESPEntityType (1.0f = Enemy Player, 2.0f = Enemy Bot, 3.0f = Ally Player, 4.0f = Ally Bot)
    float screenHeadX;   // Projected 2D Screen X of Head
    float screenHeadY;   // Projected 2D Screen Y of Head
    float screenRootX;   // Projected 2D Screen X of Feet/Root
    float screenRootY;   // Projected 2D Screen Y of Feet/Root
    float distance;      // 3D Distance in meters
    float health;        // Entity health (0.0 to 100.0)
    float isObstructed;  // 0.0f = Clear line of sight, 1.0f = Blocked by wall
};

// =========================================================================
// ESPManager Class
// Manages real-time entity tracking, team filtering, and projection
// =========================================================================

class ESPManager {
public:
    // Core Feature Toggles
    std::atomic<bool> isEspEnabled{false};     // Master Switch
    std::atomic<bool> espLine{false};          // Snapline (Top of screen to target)
    std::atomic<bool> espBox{false};           // 2D Tactical Bounding Box
    std::atomic<bool> espHealth{true};         // Dynamic Vertical Health Bar
    std::atomic<bool> espDistance{true};       // Distance and Entity Tag
    std::atomic<bool> espTeamFilter{true};     // Filter Tim: Only Enemies when true, show all when false

    ESPManager();
    ~ESPManager();

    // Runs strictly on Unity Engine Main Thread (Thread-Safe Producer)
    void updateOnEngineThread(void *aimingControl);

    // Query active entities, filter by team, calculate screen projection on Unity thread
    int getVisibleEntities(ESPEntityData *outArray, int maxCount, int screenW, int screenH, void *aimingControl = nullptr);
};

// Global singleton instance
extern ESPManager *g_espManager;

#endif // ESP_MANAGER_H
