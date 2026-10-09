#pragma once

#include <vector>
#include <mutex>
#include <cmath>
#include "hook.h"

// =========================================================================
// ESP Overlay Manager for GTA SA FPS Mod Menu
// Follows Simple ESP Tutorial with LGL Mod Menu Architecture
// =========================================================================

// Global player pointer matching tutorial
inline void *myPlayer = nullptr;

// Entity classification for accurate life checking and bone parsing
enum EntityType {
    ENTITY_TYPE_UNKNOWN = 0,
    ENTITY_TYPE_NET_PLAYER = 1,
    ENTITY_TYPE_BOT_PLAYER = 2
};

// Enemy entity structure matching tutorial
struct enemy_t {
    void *object;
    EntityType type;
    Vector3 location;
    int health;

    enemy_t() : object(nullptr), type(ENTITY_TYPE_UNKNOWN), location(Vector3(0.0f, 0.0f, 0.0f)), health(100) {}
    enemy_t(void *obj, EntityType t) : object(obj), type(t), location(Vector3(0.0f, 0.0f, 0.0f)), health(100) {}
};

// Forward declarations from Main.cpp
extern bool isEntityDeadOrCorpse(void *netPlayer, void *botPlayer, void *targetibleObj);
extern bool isNetworkPlayerTeammate(void *netPlayer);
extern std::atomic<void*> g_localPlayerNetPlayer;
extern std::atomic<void*> g_activeAimingCamera;

// Safe Health & Alive checking helpers matching tutorial
inline bool IsPlayerDead(void *player, EntityType type = ENTITY_TYPE_UNKNOWN) {
    if (player == nullptr || !isPointerReadable(player) || !isUnityObjectAlive(player)) return true;

    if (type == ENTITY_TYPE_NET_PLAYER) {
        return isEntityDeadOrCorpse(player, nullptr, nullptr);
    } else if (type == ENTITY_TYPE_BOT_PLAYER) {
        void *netPlayer = nullptr;
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(player) + 0x50))) {
            netPlayer = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(player) + 0x50);
            if (netPlayer != nullptr && !isUnityObjectAlive(netPlayer)) {
                netPlayer = nullptr;
            }
        }
        void *tObj = nullptr;
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(player) + 0x60))) {
            tObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(player) + 0x60);
            if (tObj != nullptr && !isUnityObjectAlive(tObj)) {
                tObj = nullptr;
            }
        }
        return isEntityDeadOrCorpse(netPlayer, player, tObj);
    }

    return false;
}

inline bool PlayerAlive(void *player, EntityType type = ENTITY_TYPE_UNKNOWN) {
    return !IsPlayerDead(player, type);
}

inline int GetPlayerHealth(void *player, EntityType type = ENTITY_TYPE_UNKNOWN) {
    if (player == nullptr || IsPlayerDead(player, type)) return 0;
    return 100;
}

// Transform & Position helpers matching tutorial
inline void *getTransform(void *player) {
    if (player == nullptr || !isPointerReadable(player) || !isUnityObjectAlive(player)) return nullptr;
    if (get_transform != nullptr) {
        return get_transform(player);
    }
    return nullptr;
}

inline Vector3 get_position(void *transform) {
    if (transform == nullptr) return Vector3(0.0f, 0.0f, 0.0f);
    return getTransformPosition(transform);
}

inline Vector3 GetPlayerLocation(void *player) {
    if (player == nullptr) return Vector3(0.0f, 0.0f, 0.0f);
    void *tf = getTransform(player);
    if (tf == nullptr) return Vector3(0.0f, 0.0f, 0.0f);
    return get_position(tf);
}

// Safe camera helper reading cached camera pointer from Unity engine thread
inline void *get_camera() {
    void *aimCam = g_activeAimingCamera.load();
    if (aimCam != nullptr && isUnityObjectAlive(aimCam)) {
        return aimCam;
    }
    return nullptr;
}

// WorldToScreenPoint helper matching tutorial
inline Vector3 WorldToScreenPoint(void *cameraObj, Vector3 test) {
    if (cameraObj == nullptr || !isUnityObjectAlive(cameraObj)) return Vector3(0.0f, 0.0f, -1.0f);
    void *nativeCam = getNativeUnityPointer(cameraObj);
    if (nativeCam == nullptr) nativeCam = cameraObj;
    Vector3 position(0.0f, 0.0f, -1.0f);
    if (WorldToScreenPoint_Injected != nullptr) {
        WorldToScreenPoint_Injected(nativeCam, &test, 2, &position);
    }
    return position;
}

// Color and Render helper matching tutorial
struct Color {
    float r, g, b, a;
    Color() : r(1), g(1), b(1), a(1) {}
    Color(float _r, float _g, float _b, float _a = 1.0f) : r(_r), g(_g), b(_b), a(_a) {}

    static Color Red()    { return Color(1.0f, 0.0f, 0.0f, 1.0f); }
    static Color Green()  { return Color(0.0f, 1.0f, 0.0f, 1.0f); }
    static Color Blue()   { return Color(0.0f, 0.0f, 1.0f, 1.0f); }
    static Color Yellow() { return Color(1.0f, 1.0f, 0.0f, 1.0f); } // Kuning (Yellow)
    static Color White()  { return Color(1.0f, 1.0f, 1.0f, 1.0f); }
};

struct ESPRenderer {
    void DrawLine(Color color, float thickness, Vector2 from, Vector2 to) {
        // Dispatched to floating Android overlay view via GetEspData JNI
    }
    void DrawBox(Color color, float thickness, float x, float y, float w, float h) {
        // Dispatched to floating Android overlay view via GetEspData JNI
    }
};
inline ESPRenderer esp;

// ESPManager Class matching tutorial with thread safety
class ESPManager {
private:
    std::mutex m_mutex;

    bool isEnemyPresentInternal(void *enemyObject) {
        if (!enemies) return false;
        for (auto it = enemies->begin(); it != enemies->end(); ++it) {
            if (*it != nullptr && (*it)->object == enemyObject) {
                return true;
            }
        }
        return false;
    }

public:
    std::vector<enemy_t *> *enemies;

    ESPManager() {
        enemies = new std::vector<enemy_t *>();
    }

    ~ESPManager() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (enemies != nullptr) {
            for (auto *e : *enemies) {
                delete e;
            }
            delete enemies;
            enemies = nullptr;
        }
    }

    std::mutex& getMutex() {
        return m_mutex;
    }

    bool isEnemyPresent(void *enemyObject) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return isEnemyPresentInternal(enemyObject);
    }

    void removeEnemy(enemy_t *enemy) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!enemies || enemy == nullptr) return;
        for (size_t i = 0; i < enemies->size(); i++) {
            if ((*enemies)[i] == enemy) {
                delete (*enemies)[i];
                enemies->erase(enemies->begin() + i);
                return;
            }
        }
    }

    void tryAddEnemy(void *enemyObject, EntityType type = ENTITY_TYPE_UNKNOWN) {
        if (enemyObject == nullptr) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!enemies) return;

        if (isEnemyPresentInternal(enemyObject)) {
            return;
        }

        if (IsPlayerDead(enemyObject, type)) {
            return;
        }

        enemy_t *newEnemy = new enemy_t(enemyObject, type);
        newEnemy->health = 100;
        newEnemy->location = GetPlayerLocation(enemyObject);

        enemies->push_back(newEnemy);
    }

    void updateEnemies() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!enemies) return;
        for (size_t i = 0; i < enemies->size(); ) {
            enemy_t *current = (*enemies)[i];
            if (current == nullptr || current->object == nullptr || IsPlayerDead(current->object, current->type)) {
                delete current;
                enemies->erase(enemies->begin() + i);
            } else {
                current->location = GetPlayerLocation(current->object);
                ++i;
            }
        }
    }

    void clearEnemies() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!enemies) return;
        for (auto *e : *enemies) {
            delete e;
        }
        enemies->clear();
    }

    void removeEnemyGivenObject(void *enemyObject) {
        if (enemyObject == nullptr) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!enemies) return;
        for (size_t i = 0; i < enemies->size(); i++) {
            if ((*enemies)[i] != nullptr && (*enemies)[i]->object == enemyObject) {
                delete (*enemies)[i];
                enemies->erase(enemies->begin() + i);
                return;
            }
        }
    }
};

// Global ESPManager instance matching tutorial
inline ESPManager *espManager = nullptr;
