#ifndef ESP_MANAGER_H
#define ESP_MANAGER_H

#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>
#include "Includes/Utils.h"
#include "Includes/Logger.h"
#include "hook.h"

// Forward declarations of helper functions defined in Main.cpp
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

        void *localTObj = g_localPlayerTargetibleObject.load();
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

            // Ensure transform is valid
            if (get_transform == nullptr) {
                ++it;
                continue;
            }
            void *rootTrans = get_transform(entityObj);
            if (rootTrans == nullptr || !isUnityObjectAlive(rootTrans)) {
                ++it;
                continue;
            }
            Vector3 charRoot = getTransformPosition(rootTrans);
            if (charRoot.x == 0.0f && charRoot.y == 0.0f && charRoot.z == 0.0f) {
                ++it;
                continue;
            }

            // Health extraction & death verification
            int currentHp = 100;
            int maxHp = 100;
            bool isDead = false;

            if (enemy->isBot) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x30))) {
                    void *botHealth = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x30);
                    if (botHealth != nullptr && isPointerReadable(botHealth) && isUnityObjectAlive(botHealth)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C))) {
                            float hpVal = *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C);
                            if (hpVal <= 0.0f) {
                                isDead = true;
                            } else {
                                currentHp = static_cast<int>(hpVal);
                            }
                        }
                    }
                }
            } else {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0xC0))) {
                    void *targetInfo = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0xC0);
                    if (targetInfo != nullptr && isPointerReadable(targetInfo)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x34))) {
                            bool isAlive = *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x34);
                            if (!isAlive) {
                                isDead = true;
                            }
                        }
                    }
                }
            }

            if (isDead) {
                // Skip rendering dead entity this frame without erasing it permanently
                ++it;
                continue;
            }

            // Team & Enemy discrimination
            bool isEnemy = true;
            void *targetibleObj = nullptr;

            if (enemy->isBot) {
                // AI Bots in match are enemies by default
                isEnemy = true;
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x60))) {
                    targetibleObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x60);
                }
                if (targetibleObj == nullptr && BotPlayer_GetTargetibleObject != nullptr) {
                    targetibleObj = BotPlayer_GetTargetibleObject(entityObj);
                }
                if (targetibleObj != nullptr && isPointerReadable(targetibleObj) && isUnityObjectAlive(targetibleObj)) {
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90))) {
                        void *customSettings = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90);
                        if (customSettings != nullptr && isPointerReadable(customSettings) && isUnityObjectAlive(customSettings)) {
                            if (get_AllyObjectToogle != nullptr && get_AllyObjectToogle(customSettings)) {
                                isEnemy = false;
                            } else if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(customSettings) + 0x20))) {
                                if (*reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(customSettings) + 0x20)) {
                                    isEnemy = false;
                                }
                            }
                        }
                    }
                }
            } else {
                // NetworkPlayer
                if (NetworkPlayer_GetTargetibleObject != nullptr) {
                    targetibleObj = NetworkPlayer_GetTargetibleObject(entityObj);
                }

                int targetType = 0;
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0xC0))) {
                    void *targetInfo = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0xC0);
                    if (targetInfo != nullptr && isPointerReadable(targetInfo)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30))) {
                            targetType = *reinterpret_cast<int *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30);
                        }
                        if (targetType == 0 && get_TargetType != nullptr) {
                            targetType = get_TargetType(targetInfo);
                        }
                    }
                }

                if (targetType == 1) { // LocalPlayer
                    g_localPlayerNetPlayer.store(entityObj);
                    ++it;
                    continue;
                }

                if (targetType != 0) {
                    if ((targetType & (2 | 8 | 32)) != 0) {
                        isEnemy = false; // Teammate/Ally
                    } else if ((targetType & (4 | 16 | 64 | 128)) != 0) {
                        isEnemy = true;  // Enemy
                    }
                } else {
                    if (targetibleObj != nullptr && isPointerReadable(targetibleObj) && isUnityObjectAlive(targetibleObj)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90))) {
                            void *customSettings = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90);
                            if (customSettings != nullptr && isPointerReadable(customSettings) && isUnityObjectAlive(customSettings)) {
                                if (get_AllyObjectToogle != nullptr && get_AllyObjectToogle(customSettings)) {
                                    isEnemy = false;
                                } else if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(customSettings) + 0x20))) {
                                    if (*reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(customSettings) + 0x20)) {
                                        isEnemy = false;
                                    }
                                }
                            }
                        }
                    } else {
                        void *localNet = g_localPlayerNetPlayer.load();
                        if (localNet != nullptr && NetworkPlayer_IsTeammate != nullptr && isUnityObjectAlive(localNet)) {
                            isEnemy = !NetworkPlayer_IsTeammate(entityObj, localNet);
                        } else {
                            isEnemy = true;
                        }
                    }
                }
            }
            enemy->isEnemy = isEnemy;

            // Filter if enemy only toggle is active
            if (enemyOnly && !isEnemy) {
                ++it;
                continue;
            }

            // Resolve 3D Bone Coordinates (Head & Ground/Feet)
            void *headBone = nullptr;

            if (enemy->isBot) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x58))) {
                    void *tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x58);
                    if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78))) {
                            void *animator = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78);
                            if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                                void *hb = GetBoneTransform(animator, 10); // Head bone
                                if (hb != nullptr && isUnityObjectAlive(hb)) headBone = hb;
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
                if (headBone == nullptr && targetibleObj != nullptr && isPointerReadable(targetibleObj)) {
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x38))) {
                        void *t = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x38);
                        if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                    }
                }
            } else {
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
                                }
                            }
                        }
                    }
                }
                if (headBone == nullptr && targetibleObj != nullptr && isPointerReadable(targetibleObj)) {
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40))) {
                        void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40);
                        if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                            void *t = get_transform(col);
                            if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                        }
                    }
                }
            }

            Vector3 worldFeet = charRoot;
            if (targetibleObj != nullptr && isPointerReadable(targetibleObj) && isUnityObjectAlive(targetibleObj)) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x80))) {
                    void *footTrans = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x80);
                    if (footTrans != nullptr && isUnityObjectAlive(footTrans)) {
                        Vector3 fPos = getTransformPosition(footTrans);
                        if (fPos.x != 0.0f || fPos.y != 0.0f || fPos.z != 0.0f) {
                            worldFeet = fPos;
                        }
                    }
                }
            }

            Vector3 worldHead(0.0f, 0.0f, 0.0f);

            if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                worldHead = getTransformPosition(headBone);
            }
            if (worldHead.x == 0.0f && worldHead.y == 0.0f && worldHead.z == 0.0f) {
                worldHead = worldFeet + Vector3(0.0f, 1.70f, 0.0f);
            } else if (worldHead.y <= worldFeet.y + 0.3f) {
                worldHead = worldFeet + Vector3(0.0f, 1.70f, 0.0f);
            } else {
                worldHead = worldHead + Vector3(0.0f, 0.15f, 0.0f);
            }

            // World to Viewport Projection
            Vector3 vpHead(0.0f, 0.0f, 0.0f);
            Vector3 vpFeet(0.0f, 0.0f, 0.0f);

            bool headValid = worldToViewport(cameraObj, worldHead, vpHead);
            bool feetValid = worldToViewport(cameraObj, worldFeet, vpFeet);

            // In Unity, z > 0.1f means the target is in front of the camera view
            if ((!headValid && !feetValid) || (vpHead.z < 0.2f && vpFeet.z < 0.2f)) {
                // Target is behind camera
                ++it;
                continue;
            }

            float dist = (vpFeet.z > 0.2f) ? vpFeet.z : vpHead.z;
            if (dist < 0.2f || dist > maxDistance) {
                ++it;
                continue;
            }

            // Convert Viewport coordinates (0..1) to Android Screen Canvas coordinates
            float screenHeadX = vpHead.x * static_cast<float>(screenWidth);
            float screenHeadY = (1.0f - vpHead.y) * static_cast<float>(screenHeight);

            float screenRootX = vpFeet.x * static_cast<float>(screenWidth);
            float screenRootY = (1.0f - vpFeet.y) * static_cast<float>(screenHeight);

            // Handle edge clipping when feet or head cross near camera plane
            float approxH = (static_cast<float>(screenHeight) * 1.70f) / std::max(0.5f, dist);
            if (!headValid || vpHead.z < 0.2f) {
                screenHeadX = screenRootX;
                screenHeadY = screenRootY - approxH;
            } else if (!feetValid || vpFeet.z < 0.2f) {
                screenRootX = screenHeadX;
                screenRootY = screenHeadY + approxH;
            }

            // Discard targets extremely far off-screen
            float offMargin = static_cast<float>(screenWidth) * 0.4f;
            if (screenHeadX < -offMargin || screenHeadX > static_cast<float>(screenWidth) + offMargin ||
                screenHeadY < -offMargin || screenHeadY > static_cast<float>(screenHeight) + offMargin) {
                ++it;
                continue;
            }

            // Push 9 float attributes into the serialized buffer for JNI
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
