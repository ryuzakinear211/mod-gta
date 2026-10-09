#ifndef ESP_MANAGER_H
#define ESP_MANAGER_H

#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>
#include "Includes/Utils.h"
#include "Includes/Logger.h"
#include "hook.h"

// Forward declaration of helpers defined in Main.cpp
static bool isNetworkPlayerTeammate(void *netPlayer);
static bool isEntityEnemy(void *netPlayer, void *targetibleObj, void *botPlayer);
static bool isEntityDeadOrCorpse(void *netPlayer, void *botPlayer, void *targetibleObj);

// Struct enemy_t matching tutorial structure with enhanced 3D/2D tracking
struct enemy_t {
    void *object;
    bool isBot;
    Vector3 location;     // World root position
    Vector3 headLocation; // World head position
    int health;
    int maxHealth;
    bool isEnemy;
    float distance;
    float screenX;
    float screenY;
    float screenHeadX;
    float screenHeadY;
    bool onScreen;

    enemy_t()
        : object(nullptr), isBot(false), health(100), maxHealth(100),
          isEnemy(true), distance(0.0f), screenX(0.0f), screenY(0.0f),
          screenHeadX(0.0f), screenHeadY(0.0f), onScreen(false) {}

    explicit enemy_t(void *obj, bool bot = false)
        : object(obj), isBot(bot), health(100), maxHealth(100),
          isEnemy(true), distance(0.0f), screenX(0.0f), screenY(0.0f),
          screenHeadX(0.0f), screenHeadY(0.0f), onScreen(false) {}
};

class ESPManager {
public:
    std::vector<enemy_t *> enemies;
    mutable std::mutex m_mutex;

    ESPManager() {}

    ~ESPManager() {
        clear();
    }

    bool isEnemyPresent(void *enemyObject) {
        if (enemyObject == nullptr) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto enemy : enemies) {
            if (enemy != nullptr && enemy->object == enemyObject) {
                return true;
            }
        }
        return false;
    }

    void removeEnemy(enemy_t *enemy) {
        if (enemy == nullptr) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = enemies.begin(); it != enemies.end(); ++it) {
            if (*it == enemy) {
                delete *it;
                enemies.erase(it);
                return;
            }
        }
    }

    void removeEnemyGivenObject(void *enemyObject) {
        if (enemyObject == nullptr) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = enemies.begin(); it != enemies.end(); ++it) {
            if ((*it) != nullptr && (*it)->object == enemyObject) {
                delete *it;
                enemies.erase(it);
                return;
            }
        }
    }

    void tryAddEnemy(void *enemyObject, bool isBot = false) {
        if (enemyObject == nullptr || !isUnityObjectAlive(enemyObject)) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto enemy : enemies) {
            if (enemy != nullptr && enemy->object == enemyObject) {
                return; // Already registered
            }
        }

        enemy_t *newEnemy = new enemy_t(enemyObject, isBot);
        enemies.push_back(newEnemy);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto enemy : enemies) {
            delete enemy;
        }
        enemies.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return enemies.size();
    }

    // Main ESP extraction function: projects 3D entity positions to 2D screen space
    int collectESPData(void *cameraObj, int screenWidth, int screenHeight,
                       float maxDistance, bool enemyOnly, void *localPlayer,
                       std::vector<float> &outBuffer) {
        if (cameraObj == nullptr || !isUnityObjectAlive(cameraObj)) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (enemies.empty()) {
            return 0;
        }

        int visibleCount = 0;

        for (auto it = enemies.begin(); it != enemies.end(); ) {
            enemy_t *enemy = *it;
            if (enemy == nullptr || enemy->object == nullptr || !isUnityObjectAlive(enemy->object)) {
                delete enemy;
                it = enemies.erase(it);
                continue;
            }

            void *entityObj = enemy->object;

            // Skip local player
            if (localPlayer != nullptr && entityObj == localPlayer) {
                ++it;
                continue;
            }

            // Check if entity is dead/corpse
            bool isDead = false;
            if (enemy->isBot) {
                isDead = isEntityDeadOrCorpse(nullptr, entityObj, nullptr);
            } else {
                isDead = isEntityDeadOrCorpse(entityObj, nullptr, nullptr);
            }

            if (isDead) {
                delete enemy;
                it = enemies.erase(it);
                continue;
            }

            // Teammate & Enemy filtering
            bool isEnemy = true;
            if (enemy->isBot) {
                isEnemy = isEntityEnemy(nullptr, nullptr, entityObj);
            } else {
                isEnemy = isEntityEnemy(entityObj, nullptr, nullptr);
            }
            enemy->isEnemy = isEnemy;

            if (enemyOnly && !isEnemy) {
                ++it;
                continue;
            }

            // Resolve 3D Bone Coordinates (Head & Root)
            void *headBone = nullptr;
            void *bodyBone = nullptr;
            int currentHp = 100;
            int maxHp = 100;

            if (enemy->isBot) {
                // AI Bot Bone Resolution
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x58))) {
                    void *tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x58);
                    if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78))) {
                            void *animator = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78);
                            if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                                void *hb = GetBoneTransform(animator, 10);
                                if (hb != nullptr && isUnityObjectAlive(hb)) headBone = hb;
                                void *bb = GetBoneTransform(animator, 9);
                                if (bb == nullptr || !isUnityObjectAlive(bb)) {
                                    bb = GetBoneTransform(animator, 8);
                                }
                                if (bb != nullptr && isUnityObjectAlive(bb)) bodyBone = bb;
                            }
                        }
                    }
                }

                if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x28))) {
                    void *botLook = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x28);
                    if (botLook != nullptr && isUnityObjectAlive(botLook)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botLook) + 0x28))) {
                            void *t = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botLook) + 0x28);
                            if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                        }
                    }
                }

                // Bot Health Resolution (BotPlayerHealth at 0x30)
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x30))) {
                    void *botHealth = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x30);
                    if (botHealth != nullptr && isPointerReadable(botHealth) && isUnityObjectAlive(botHealth)) {
                        float hpVal = 100.0f;
                        if (BotPlayerHealth_GetHealth != nullptr) {
                            hpVal = BotPlayerHealth_GetHealth(botHealth);
                        } else if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C))) {
                            hpVal = *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C);
                        }
                        currentHp = static_cast<int>(std::max(0.0f, hpVal));
                    }
                }
            } else {
                // Real Network Player Bone Resolution
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x88))) {
                    void *dollsMgr = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x88);
                    if (dollsMgr != nullptr && isUnityObjectAlive(dollsMgr)) {
                        void *tpCtrl = nullptr;
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x50))) {
                            tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x50);
                        }
                        if (tpCtrl == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x60))) {
                            tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x60);
                        }
                        if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78))) {
                                void *animator = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78);
                                if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                                    void *hb = GetBoneTransform(animator, 10);
                                    if (hb != nullptr && isUnityObjectAlive(hb)) headBone = hb;
                                    void *bb = GetBoneTransform(animator, 9);
                                    if (bb == nullptr || !isUnityObjectAlive(bb)) {
                                        bb = GetBoneTransform(animator, 8);
                                    }
                                    if (bb != nullptr && isUnityObjectAlive(bb)) bodyBone = bb;
                                }
                            }
                        }
                    }
                }

                if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0xC8))) {
                    void *bpm = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0xC8);
                    if (bpm != nullptr && isPointerReadable(bpm)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bpm) + 0x20))) {
                            void *bodyPointsArr = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(bpm) + 0x20);
                            if (bodyPointsArr != nullptr && isPointerReadable(bodyPointsArr)) {
                                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bodyPointsArr) + 0x18))) {
                                    uintptr_t length = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(bodyPointsArr) + 0x18);
                                    if (length > 0 && length < 32) {
                                        void **items = reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(bodyPointsArr) + 0x20);
                                        for (uintptr_t i = 0; i < length; i++) {
                                            if (isPointerReadable(&items[i])) {
                                                void *bp = items[i];
                                                if (bp != nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bp) + 0x10))) {
                                                    void *bpView = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(bp) + 0x10);
                                                    if (bpView != nullptr && isUnityObjectAlive(bpView) &&
                                                        isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bpView) + 0x20))) {
                                                        int pType = *reinterpret_cast<int *>(reinterpret_cast<uintptr_t>(bpView) + 0x20);
                                                        if (pType == 0 && headBone == nullptr && get_transform != nullptr) {
                                                            void *t = get_transform(bpView);
                                                            if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                                                        } else if (pType != 0 && bodyBone == nullptr && get_transform != nullptr) {
                                                            void *t = get_transform(bpView);
                                                            if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Calculate world positions
            Vector3 worldHead(0.0f, 0.0f, 0.0f);
            Vector3 worldRoot(0.0f, 0.0f, 0.0f);

            if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                worldHead = getTransformPosition(headBone);
            }
            if (bodyBone != nullptr && isUnityObjectAlive(bodyBone)) {
                worldRoot = getTransformPosition(bodyBone);
            }

            // Fallback to transform if either bone is missing
            if (worldRoot.x == 0.0f && worldRoot.y == 0.0f && worldRoot.z == 0.0f) {
                if (get_transform != nullptr) {
                    void *t = get_transform(entityObj);
                    if (t != nullptr && isUnityObjectAlive(t)) {
                        worldRoot = getTransformPosition(t);
                    }
                }
            }
            if (worldHead.x == 0.0f && worldHead.y == 0.0f && worldHead.z == 0.0f) {
                worldHead = worldRoot + Vector3(0.0f, 1.65f, 0.0f);
            }
            // Root height adjustment (feet ground level)
            Vector3 worldFeet = worldRoot;
            if (worldFeet.y > worldHead.y - 0.5f) {
                worldFeet.y = worldHead.y - 1.65f;
            }

            // World to Viewport Projection
            Vector3 vpHead(0.0f, 0.0f, 0.0f);
            Vector3 vpFeet(0.0f, 0.0f, 0.0f);

            bool headValid = worldToViewport(cameraObj, worldHead, vpHead);
            bool feetValid = worldToViewport(cameraObj, worldFeet, vpFeet);

            if (!headValid && !feetValid) {
                // Completely behind camera
                ++it;
                continue;
            }

            float dist = feetValid ? vpFeet.z : vpHead.z;
            if (dist < 0.2f || dist > maxDistance) {
                ++it;
                continue;
            }

            // Convert Viewport coordinates (0..1) to Android Screen Canvas coordinates
            float screenRootX = vpFeet.x * screenWidth;
            float screenRootY = (1.0f - vpFeet.y) * screenHeight;
            float screenHeadX = vpHead.x * screenWidth;
            float screenHeadY = (1.0f - vpHead.y) * screenHeight;

            // Push 9 float attributes into the buffer
            outBuffer.push_back(screenRootX);
            outBuffer.push_back(screenRootY);
            outBuffer.push_back(screenHeadX);
            outBuffer.push_back(screenHeadY);
            outBuffer.push_back(dist);
            outBuffer.push_back(static_cast<float>(currentHp));
            outBuffer.push_back(static_cast<float>(maxHp));
            outBuffer.push_back(enemy->isBot ? 1.0f : 0.0f);
            outBuffer.push_back(isEnemy ? 1.0f : 0.0f);

            visibleCount++;
            ++it;
        }

        return visibleCount;
    }
};

#endif // ESP_MANAGER_H
