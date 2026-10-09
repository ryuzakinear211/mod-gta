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

// Struct for passing active entities directly from Status Overlay synchronization
struct ESPEntityItem {
    void *object;
    bool isBot;
    void *netPlayer;

    ESPEntityItem() : object(nullptr), isBot(false), netPlayer(nullptr) {}
    ESPEntityItem(void *obj, bool bot, void *net = nullptr)
        : object(obj), isBot(bot), netPlayer(net) {}
};

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

    // Direct collection from active entities list (synchronized with status overlay)
    int collectESPDataFromList(void *cameraObj, int screenWidth, int screenHeight,
                              float maxDistance, bool enemyOnly,
                              const std::vector<ESPEntityItem> &entityList,
                              std::vector<float> &outBuffer) {
        if (cameraObj == nullptr || !isUnityObjectAlive(cameraObj) || entityList.empty()) {
            return 0;
        }

        int visibleCount = 0;

        for (const auto &item : entityList) {
            void *entityObj = item.object;
            if (entityObj == nullptr || !isUnityObjectAlive(entityObj)) {
                continue;
            }

            bool isBot = item.isBot;
            void *netPlayer = item.netPlayer;

            // Health extraction & death verification
            int currentHp = 100;
            int maxHp = 100;
            bool isDead = false;

            if (isBot) {
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
                        if (!isDead && BotPlayerHealth_GetHealth != nullptr) {
                            float hp = BotPlayerHealth_GetHealth(botHealth);
                            if (hp <= 0.0f) {
                                isDead = true;
                            } else if (hp > 0.0f) {
                                currentHp = static_cast<int>(hp);
                            }
                        }
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botHealth) + 0x28))) {
                            float mHp = *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(botHealth) + 0x28);
                            if (mHp > 0.0f) maxHp = static_cast<int>(mHp);
                        }
                    }
                }
            } else {
                // Real player
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
                continue;
            }

            // Skip local player
            void *localNet = g_localPlayerNetPlayer.load();
            if (localNet != nullptr && (entityObj == localNet || netPlayer == localNet)) {
                continue;
            }

            // Resolve targetible object & netPlayer
            void *targetibleObj = nullptr;
            if (isBot) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x60))) {
                    targetibleObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x60);
                }
                if (targetibleObj == nullptr && BotPlayer_GetTargetibleObject != nullptr) {
                    targetibleObj = BotPlayer_GetTargetibleObject(entityObj);
                }
                if (netPlayer == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x50))) {
                    netPlayer = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x50);
                }
            } else {
                if (NetworkPlayer_GetTargetibleObject != nullptr) {
                    targetibleObj = NetworkPlayer_GetTargetibleObject(entityObj);
                }
            }

            // Check if local player via targetInfo
            if (netPlayer != nullptr && isPointerReadable(netPlayer)) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC0))) {
                    void *targetInfo = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC0);
                    if (targetInfo != nullptr && isPointerReadable(targetInfo)) {
                        int tType = 0;
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30))) {
                            tType = *reinterpret_cast<int *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30);
                        }
                        if (tType == 0 && get_TargetType != nullptr) {
                            tType = get_TargetType(targetInfo);
                        }
                        if (tType == 1) { // LocalPlayer
                            g_localPlayerNetPlayer.store(netPlayer);
                            continue;
                        }
                    }
                }
            }

            // Exclude dead entities / corpses
            if (isEntityDeadOrCorpse(netPlayer, isBot ? entityObj : nullptr, targetibleObj)) {
                continue;
            }

            // Team & Enemy discrimination using game engine logic
            bool isEnemy = isEntityEnemy(netPlayer, targetibleObj, isBot ? entityObj : nullptr);

            // Filter out teammates (including AI bot teammates!) if enemyOnly is ON
            if (enemyOnly && !isEnemy) {
                continue;
            }

            // Resolve Bones (Head bone and Ground/Root transform)
            void *headBone = nullptr;
            void *rootTrans = nullptr;

            if (isBot) {
                // 1. ThirdPersonController at offset 0x58 -> Animator at 0x78
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x58))) {
                    void *tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x58);
                    if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78))) {
                            void *animator = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78);
                            if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                                void *hb = GetBoneTransform(animator, 10); // Head bone (10)
                                if (hb != nullptr && isUnityObjectAlive(hb)) headBone = hb;
                            }
                        }
                    }
                }
                // 2. BotPlayerLook at offset 0x28 -> Transform at 0x28
                if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x28))) {
                    void *botLook = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x28);
                    if (botLook != nullptr && isUnityObjectAlive(botLook)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botLook) + 0x28))) {
                            void *t = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botLook) + 0x28);
                            if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                        }
                    }
                }
                // 3. From associated NetworkPlayer (dollsMgr -> tpCtrl -> animator)
                if (headBone == nullptr && netPlayer != nullptr && isUnityObjectAlive(netPlayer)) {
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(netPlayer) + 0x88))) {
                        void *dollsMgr = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(netPlayer) + 0x88);
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
                }
                // 4. Fallback from TargetibleObject
                if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(entityObj) + 0x60))) {
                    void *tObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(entityObj) + 0x60);
                    if (tObj != nullptr && isPointerReadable(tObj) && isUnityObjectAlive(tObj)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tObj) + 0x40))) {
                            void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tObj) + 0x40);
                            if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                                void *t = get_transform(col);
                                if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                            }
                        }
                        if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tObj) + 0x38))) {
                            void *t = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tObj) + 0x38);
                            if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                        }
                    }
                }
            } else {
                // Real NetworkPlayer
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
                if (headBone == nullptr && NetworkPlayer_GetTargetibleObject != nullptr) {
                    void *tObj = NetworkPlayer_GetTargetibleObject(entityObj);
                    if (tObj != nullptr && isUnityObjectAlive(tObj)) {
                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tObj) + 0x40))) {
                            void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tObj) + 0x40);
                            if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                                void *t = get_transform(col);
                                if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                            }
                        }
                    }
                }
            }

            // Resolve root transform
            if (get_transform != nullptr) {
                rootTrans = get_transform(entityObj);
                if ((rootTrans == nullptr || !isUnityObjectAlive(rootTrans)) && netPlayer != nullptr && isUnityObjectAlive(netPlayer)) {
                    rootTrans = get_transform(netPlayer);
                }
            }
            if (rootTrans == nullptr || !isUnityObjectAlive(rootTrans)) {
                continue;
            }
            Vector3 charRoot = getTransformPosition(rootTrans);
            if (charRoot.x == 0.0f && charRoot.y == 0.0f && charRoot.z == 0.0f) {
                continue;
            }

            // Resolve Head 3D position
            Vector3 headPos(0.0f, 0.0f, 0.0f);
            if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                headPos = getTransformPosition(headBone);
            }
            if (headPos.x == 0.0f && headPos.y == 0.0f && headPos.z == 0.0f) {
                headPos = Vector3(charRoot.x, charRoot.y + 1.65f, charRoot.z);
            }

            // Ground plane Y
            float groundY = charRoot.y;
            if (groundY > headPos.y - 0.5f || groundY < headPos.y - 2.5f) {
                groundY = headPos.y - 1.70f;
            }

            // Anchor both worldHead and worldFeet along the EXACT same 3D vertical centerline (headPos.x, headPos.z)
            // This ensures screenHeadX == screenRootX and centers the box dead-on the character's body!
            Vector3 worldHead(headPos.x, headPos.y + 0.15f, headPos.z);
            Vector3 worldFeet(headPos.x, groundY, headPos.z);

            // World to Viewport Projection
            Vector3 vpHead(0.0f, 0.0f, 0.0f);
            Vector3 vpFeet(0.0f, 0.0f, 0.0f);

            bool headValid = worldToViewport(cameraObj, worldHead, vpHead);
            bool feetValid = worldToViewport(cameraObj, worldFeet, vpFeet);

            if ((!headValid && !feetValid) || (vpHead.z < 0.2f && vpFeet.z < 0.2f)) {
                continue; // Behind camera
            }

            float dist = (vpFeet.z > 0.2f) ? vpFeet.z : vpHead.z;
            if (dist < 0.2f || dist > maxDistance) {
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
            outBuffer.push_back(isBot ? 1.0f : 0.0f);
            outBuffer.push_back(isEnemy ? 1.0f : 0.0f);

            visibleCount++;
        }

        return visibleCount;
    }

    // Legacy fallback method matching tutorial signature
    int collectESPData(void *cameraObj, int screenWidth, int screenHeight,
                       float maxDistance, bool enemyOnly, void *localPlayer,
                       std::vector<float> &outBuffer) {
        std::vector<ESPEntityItem> list;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto enemy : enemies) {
                if (enemy != nullptr && enemy->object != nullptr && enemy->object != localPlayer) {
                    list.push_back(ESPEntityItem(enemy->object, enemy->isBot, nullptr));
                }
            }
        }
        return collectESPDataFromList(cameraObj, screenWidth, screenHeight, maxDistance, enemyOnly, list, outBuffer);
    }
};

#endif // ESP_MANAGER_H
