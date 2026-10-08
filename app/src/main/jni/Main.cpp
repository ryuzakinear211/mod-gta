#include <list>
#include <vector>
#include <string.h>
#include <pthread.h>
#include <thread>
#include <cstring>
#include <jni.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <dlfcn.h>
#include <mutex>
#include <unordered_map>
#include <set>
#include <chrono>
#include <cctype>

#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "KittyMemory/MemoryPatch.h"
#include "Menu/Setup.h"

// Target library for GTA SA FPS (il2cpp 64-bit)
#define targetLibName OBFUSCATE("libil2cpp.so")

#include "Includes/Macros.h"

// =========================================================================
// Unity Vector3 & Utility Structures
// =========================================================================
struct Vector3 {
    float x;
    float y;
    float z;
    Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

// Unity Engine C++ internal bindings
static void *(*get_transform)(void *) = nullptr;
static void (*set_localScale_Injected)(void *, const Vector3 *) = nullptr;
static void *(*GetBoneTransformInternal_Injected)(void *, int) = nullptr;
static int (*GetChildCount)(void *) = nullptr;
static void *(*GetChild)(void *, int) = nullptr;
static void *(*get_name)(void *) = nullptr;

static void initUnityPointers() {
    if (get_transform == nullptr) {
        get_transform = (void *(*)(void *)) getAbsoluteAddress(targetLibName, 0x8597C20);
    }
    if (set_localScale_Injected == nullptr) {
        set_localScale_Injected = (void (*)(void *, const Vector3 *)) getAbsoluteAddress(targetLibName, 0x85B224C);
    }
    if (GetBoneTransformInternal_Injected == nullptr) {
        GetBoneTransformInternal_Injected = (void *(*)(void *, int)) getAbsoluteAddress(targetLibName, 0x84EBEF4);
    }
    if (GetChildCount == nullptr) {
        GetChildCount = (int (*)(void *)) getAbsoluteAddress(targetLibName, 0x85B5CDC);
    }
    if (GetChild == nullptr) {
        GetChild = (void *(*)(void *, int)) getAbsoluteAddress(targetLibName, 0x85B5BEC);
    }
    if (get_name == nullptr) {
        get_name = (void *(*)(void *)) getAbsoluteAddress(targetLibName, 0x85A392C);
    }
}

// =========================================================================
// Real-time Player & Bot Tracking System
// =========================================================================
static std::mutex g_entityMutex;
static std::unordered_map<void*, uint64_t> g_networkPlayers; // NetworkPlayer* -> last seen ms
static std::unordered_map<void*, uint64_t> g_botPlayers;     // BotPlayer* -> last seen ms
static std::unordered_map<void*, void*> g_botNetPlayers;     // BotPlayer* -> NetworkPlayer*

static std::mutex g_headMutex;
static std::unordered_map<void*, void*> g_cachedHeadBones;   // Target object -> Head Transform*

static bool g_showStatusPanel = false;
static bool g_autoCount = false;
static bool g_bigHead = false;
static bool g_needsBigHeadReset = false;
static int g_bigHeadResetFrames = 0;

static const Vector3 BIG_HEAD_SCALE(2.5f, 2.5f, 2.5f);
static const Vector3 NORMAL_HEAD_SCALE(1.0f, 1.0f, 1.0f);

static uint64_t getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

static void onNetworkPlayerUpdate(void *instance) {
    if (instance == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_networkPlayers[instance] = getCurrentTimeMs();
}

static void onNetworkPlayerDestroy(void *instance) {
    if (instance == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_networkPlayers.erase(instance);
}

static void onBotPlayerUpdate(void *instance) {
    if (instance == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_botPlayers[instance] = getCurrentTimeMs();

    // Read NetworkPlayer field at offset 0x50 in BotPlayer struct
    void *netPlayer = *(void **)((uintptr_t)instance + 0x50);
    if (netPlayer != nullptr) {
        g_botNetPlayers[instance] = netPlayer;
    }
}

static void onBotPlayerDestroy(void *instance) {
    if (instance == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_botPlayers.erase(instance);
    g_botNetPlayers.erase(instance);
}

struct EntityStats {
    int realPlayerCount;
    int botCount;
    int totalEntities;
    bool inGame;
};

static EntityStats getEntityStats() {
    std::lock_guard<std::mutex> lock(g_entityMutex);
    uint64_t now = getCurrentTimeMs();
    const uint64_t TIMEOUT_MS = 2500; // 2.5 seconds timeout

    // 1. Purge stale bots
    std::set<void*> activeBotNets;
    for (auto it = g_botPlayers.begin(); it != g_botPlayers.end(); ) {
        if (now - it->second > TIMEOUT_MS) {
            g_botNetPlayers.erase(it->first);
            it = g_botPlayers.erase(it);
        } else {
            auto netIt = g_botNetPlayers.find(it->first);
            if (netIt != g_botNetPlayers.end()) {
                activeBotNets.insert(netIt->second);
            }
            ++it;
        }
    }

    // 2. Purge stale network players
    int totalNetPlayers = 0;
    int realCount = 0;
    for (auto it = g_networkPlayers.begin(); it != g_networkPlayers.end(); ) {
        if (now - it->second > TIMEOUT_MS) {
            it = g_networkPlayers.erase(it);
        } else {
            totalNetPlayers++;
            if (activeBotNets.find(it->first) == activeBotNets.end()) {
                realCount++;
            }
            ++it;
        }
    }

    int botCount = (int)g_botPlayers.size();

    // Fallback adjustment
    if (totalNetPlayers >= botCount) {
        if (realCount == 0 && totalNetPlayers > botCount) {
            realCount = totalNetPlayers - botCount;
        }
    } else {
        if (realCount < 0) realCount = 0;
    }

    int total = (totalNetPlayers > botCount) ? totalNetPlayers : (realCount + botCount);

    EntityStats stats;
    stats.realPlayerCount = realCount;
    stats.botCount = botCount;
    stats.totalEntities = total;
    stats.inGame = (total > 0);
    return stats;
}

static void resetEntityCounters() {
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_networkPlayers.clear();
    g_botPlayers.clear();
    g_botNetPlayers.clear();
}

// =========================================================================
// Big Head Implementation (Robust Client-Side Head Bone & Hitbox Scaling)
// =========================================================================

static std::string il2cppStringToStdString(void *il2cppStrObj) {
    if (il2cppStrObj == nullptr) return "";
    int32_t len = *(int32_t *)((uintptr_t)il2cppStrObj + 0x10);
    if (len <= 0 || len > 256) return "";
    const uint16_t *chars = (const uint16_t *)((uintptr_t)il2cppStrObj + 0x14);
    std::string result;
    result.reserve((size_t)len);
    for (int32_t i = 0; i < len; i++) {
        uint16_t c = chars[i];
        if (c < 128) {
            result.push_back((char)tolower(c));
        } else {
            result.push_back('?');
        }
    }
    return result;
}

static bool isLikelyHeadBoneName(const std::string &name) {
    if (name.empty()) return false;
    if (name.find("head") == std::string::npos) return false;

    // Filter out non-bone utility objects
    if (name.find("hitbox") != std::string::npos ||
        name.find("collider") != std::string::npos ||
        name.find("trigger") != std::string::npos ||
        name.find("camera") != std::string::npos ||
        name.find("cam") != std::string::npos ||
        name.find("ui") != std::string::npos ||
        name.find("sound") != std::string::npos ||
        name.find("audio") != std::string::npos) {
        return false;
    }
    return true;
}

static void* findHeadBoneRecursive(void *transform, int depth = 0) {
    if (transform == nullptr || depth > 12) return nullptr;

    if (get_name != nullptr) {
        void *nameObj = get_name(transform);
        if (nameObj != nullptr) {
            std::string nameStr = il2cppStringToStdString(nameObj);
            if (isLikelyHeadBoneName(nameStr)) {
                return transform;
            }
        }
    }

    if (GetChildCount == nullptr || GetChild == nullptr) return nullptr;
    int childCount = GetChildCount(transform);
    if (childCount <= 0 || childCount > 64) return nullptr;

    for (int i = 0; i < childCount; i++) {
        void *child = GetChild(transform, i);
        if (child != nullptr) {
            void *found = findHeadBoneRecursive(child, depth + 1);
            if (found != nullptr) return found;
        }
    }

    return nullptr;
}

static void scaleTransform(void *transform, const Vector3 &scale) {
    if (transform == nullptr || set_localScale_Injected == nullptr) return;
    set_localScale_Injected(transform, &scale);
}

static void* resolveHeadBoneFromThirdPerson(void *tpc) {
    if (tpc == nullptr) return nullptr;

    // Strategy 1: Animator at 0x78 of ThirdPersonController (HumanBodyBones.Head = 10)
    void *animator = *(void **)((uintptr_t)tpc + 0x78);
    if (animator != nullptr && GetBoneTransformInternal_Injected != nullptr) {
        void *bone = GetBoneTransformInternal_Injected(animator, 10);
        if (bone != nullptr) return bone;
    }

    // Strategy 2: ThirdSkinController at 0x58 -> _dollChanger at 0x28
    void *skinCtrl = *(void **)((uintptr_t)tpc + 0x58);
    if (skinCtrl != nullptr) {
        void *dollChanger = *(void **)((uintptr_t)skinCtrl + 0x28);
        if (dollChanger != nullptr) {
            void *dcAnim = *(void **)((uintptr_t)dollChanger + 0x50);
            if (dcAnim != nullptr && GetBoneTransformInternal_Injected != nullptr) {
                void *bone = GetBoneTransformInternal_Injected(dcAnim, 10);
                if (bone != nullptr) return bone;
            }
            void *rootBones = *(void **)((uintptr_t)dollChanger + 0x20);
            if (rootBones != nullptr) {
                void *bone = findHeadBoneRecursive(rootBones, 0);
                if (bone != nullptr) return bone;
            }
        }
    }

    // Strategy 3: AimIkController at 0x118 -> _animator (0x20)
    void *aimIk = *(void **)((uintptr_t)tpc + 0x118);
    if (aimIk != nullptr) {
        void *ikAnim = *(void **)((uintptr_t)aimIk + 0x20);
        if (ikAnim != nullptr && GetBoneTransformInternal_Injected != nullptr) {
            void *bone = GetBoneTransformInternal_Injected(ikAnim, 10);
            if (bone != nullptr) return bone;
        }
    }

    // Strategy 4: Recursive search from ThirdPersonController's own Transform
    if (get_transform != nullptr) {
        void *rootTransform = get_transform(tpc);
        if (rootTransform != nullptr) {
            void *bone = findHeadBoneRecursive(rootTransform, 0);
            if (bone != nullptr) return bone;
        }
    }

    return nullptr;
}

static void applyHeadScaleToThirdPerson(void *tpc, const Vector3 &scale) {
    if (tpc == nullptr) return;

    void *headBone = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_headMutex);
        auto it = g_cachedHeadBones.find(tpc);
        if (it != g_cachedHeadBones.end()) {
            headBone = it->second;
        }
    }

    if (headBone == nullptr) {
        headBone = resolveHeadBoneFromThirdPerson(tpc);
        if (headBone != nullptr) {
            std::lock_guard<std::mutex> lock(g_headMutex);
            g_cachedHeadBones[tpc] = headBone;
        }
    }

    if (headBone != nullptr) {
        scaleTransform(headBone, scale);
    }
}

static void resolveAndScaleNetworkPlayer(void *netPlayer, const Vector3 &scale) {
    if (netPlayer == nullptr) return;

    // 1. DollsManager at offset 0x88
    void *dollsMgr = *(void **)((uintptr_t)netPlayer + 0x88);
    if (dollsMgr != nullptr) {
        // ThirdPersonController at 0x50
        void *tpc = *(void **)((uintptr_t)dollsMgr + 0x50);
        if (tpc != nullptr) {
            applyHeadScaleToThirdPerson(tpc, scale);
        }
        // Bot ThirdPersonController at 0x60
        void *botTpc = *(void **)((uintptr_t)dollsMgr + 0x60);
        if (botTpc != nullptr) {
            applyHeadScaleToThirdPerson(botTpc, scale);
        }
        // Third person doll view at 0x48
        void *tpDoll = *(void **)((uintptr_t)dollsMgr + 0x48);
        if (tpDoll != nullptr && get_transform != nullptr) {
            void *t = get_transform(tpDoll);
            if (t != nullptr) {
                void *head = findHeadBoneRecursive(t, 0);
                if (head != nullptr) scaleTransform(head, scale);
            }
        }
        // First person doll view at 0x38 (Local player model)
        void *fpDoll = *(void **)((uintptr_t)dollsMgr + 0x38);
        if (fpDoll != nullptr && get_transform != nullptr) {
            void *t = get_transform(fpDoll);
            if (t != nullptr) {
                void *head = findHeadBoneRecursive(t, 0);
                if (head != nullptr) scaleTransform(head, scale);
            }
        }
    }

    // 2. Head Hitbox in BodyPointsManager at offset 0xC8
    void *bpm = *(void **)((uintptr_t)netPlayer + 0xC8);
    if (bpm != nullptr) {
        void *bodyPointsArr = *(void **)((uintptr_t)bpm + 0x20);
        if (bodyPointsArr != nullptr) {
            uintptr_t length = *(uintptr_t *)((uintptr_t)bodyPointsArr + 0x18);
            if (length > 0 && length < 32) {
                void **items = (void **)((uintptr_t)bodyPointsArr + 0x20);
                for (uintptr_t i = 0; i < length; i++) {
                    void *bodyPoint = items[i];
                    if (bodyPoint != nullptr) {
                        void *bpView = *(void **)((uintptr_t)bodyPoint + 0x10);
                        if (bpView != nullptr) {
                            int pointType = *(int *)((uintptr_t)bpView + 0x20);
                            if (pointType == 0) { // Head hitbox
                                void *headHitbox = get_transform ? get_transform(bpView) : nullptr;
                                if (headHitbox != nullptr) {
                                    scaleTransform(headHitbox, scale);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void resolveAndScaleBotPlayer(void *botPlayer, const Vector3 &scale) {
    if (botPlayer == nullptr) return;

    // ThirdPersonController at offset 0x58
    void *tpc = *(void **)((uintptr_t)botPlayer + 0x58);
    if (tpc != nullptr) {
        applyHeadScaleToThirdPerson(tpc, scale);
    }

    // NetworkPlayer at offset 0x50
    void *netPlayer = *(void **)((uintptr_t)botPlayer + 0x50);
    if (netPlayer != nullptr) {
        resolveAndScaleNetworkPlayer(netPlayer, scale);
    }
}

// =========================================================================
// Il2Cpp Hooks for NetworkPlayer, BotPlayer, ThirdPersonController & AimIK
// =========================================================================

// NetworkPlayer.Update: RVA 0x42CC8C8
void (*old_NetworkPlayer_Update)(void *instance) = nullptr;
void hook_NetworkPlayer_Update(void *instance) {
    if (instance != nullptr) {
        if (g_autoCount) {
            onNetworkPlayerUpdate(instance);
        }
        if (g_bigHead) {
            resolveAndScaleNetworkPlayer(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset) {
            resolveAndScaleNetworkPlayer(instance, NORMAL_HEAD_SCALE);
            if (g_bigHeadResetFrames > 0) {
                g_bigHeadResetFrames--;
                if (g_bigHeadResetFrames == 0) {
                    g_needsBigHeadReset = false;
                }
            }
        }
    }
    if (old_NetworkPlayer_Update != nullptr) {
        old_NetworkPlayer_Update(instance);
    }
}

// NetworkPlayer.OnDestroy: RVA 0x42C9540
void (*old_NetworkPlayer_OnDestroy)(void *instance) = nullptr;
void hook_NetworkPlayer_OnDestroy(void *instance) {
    if (instance != nullptr) {
        onNetworkPlayerDestroy(instance);
        {
            std::lock_guard<std::mutex> lock(g_headMutex);
            g_cachedHeadBones.erase(instance);
        }
    }
    if (old_NetworkPlayer_OnDestroy != nullptr) {
        old_NetworkPlayer_OnDestroy(instance);
    }
}

// BotPlayer.Start: RVA 0x44493F4
void (*old_BotPlayer_Start)(void *instance) = nullptr;
void hook_BotPlayer_Start(void *instance) {
    if (instance != nullptr && g_autoCount) {
        onBotPlayerUpdate(instance);
    }
    if (old_BotPlayer_Start != nullptr) {
        old_BotPlayer_Start(instance);
    }
}

// BotPlayer.Update: RVA 0x444A310
void (*old_BotPlayer_Update)(void *instance) = nullptr;
void hook_BotPlayer_Update(void *instance) {
    if (instance != nullptr) {
        if (g_autoCount) {
            onBotPlayerUpdate(instance);
        }
        if (g_bigHead) {
            resolveAndScaleBotPlayer(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset) {
            resolveAndScaleBotPlayer(instance, NORMAL_HEAD_SCALE);
        }
    }
    if (old_BotPlayer_Update != nullptr) {
        old_BotPlayer_Update(instance);
    }
}

// BotPlayer.OnDestroy: RVA 0x4449EB0
void (*old_BotPlayer_OnDestroy)(void *instance) = nullptr;
void hook_BotPlayer_OnDestroy(void *instance) {
    if (instance != nullptr) {
        onBotPlayerDestroy(instance);
        {
            std::lock_guard<std::mutex> lock(g_headMutex);
            g_cachedHeadBones.erase(instance);
        }
    }
    if (old_BotPlayer_OnDestroy != nullptr) {
        old_BotPlayer_OnDestroy(instance);
    }
}

// ThirdPersonController.Update: RVA 0x43607BC
void (*old_ThirdPersonController_Update)(void *instance) = nullptr;
void hook_ThirdPersonController_Update(void *instance) {
    if (old_ThirdPersonController_Update != nullptr) {
        old_ThirdPersonController_Update(instance);
    }
    if (instance != nullptr) {
        if (g_bigHead) {
            applyHeadScaleToThirdPerson(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset) {
            applyHeadScaleToThirdPerson(instance, NORMAL_HEAD_SCALE);
        }
    }
}

// ThirdPersonController.OnDestroy: RVA 0x4360FB8
void (*old_ThirdPersonController_OnDestroy)(void *instance) = nullptr;
void hook_ThirdPersonController_OnDestroy(void *instance) {
    if (instance != nullptr) {
        std::lock_guard<std::mutex> lock(g_headMutex);
        g_cachedHeadBones.erase(instance);
    }
    if (old_ThirdPersonController_OnDestroy != nullptr) {
        old_ThirdPersonController_OnDestroy(instance);
    }
}

// AimIkController.LateUpdate: RVA 0x407C85C
void (*old_AimIkController_LateUpdate)(void *instance) = nullptr;
void hook_AimIkController_LateUpdate(void *instance) {
    if (old_AimIkController_LateUpdate != nullptr) {
        old_AimIkController_LateUpdate(instance);
    }
    if (instance != nullptr) {
        // Offset 0x60 in AimIkController is ThirdPersonController
        void *tpc = *(void **)((uintptr_t)instance + 0x60);
        if (tpc != nullptr) {
            if (g_bigHead) {
                applyHeadScaleToThirdPerson(tpc, BIG_HEAD_SCALE);
            } else if (g_needsBigHeadReset) {
                applyHeadScaleToThirdPerson(tpc, NORMAL_HEAD_SCALE);
            }
        }
    }
}

// Thread to hook functions once libil2cpp.so is loaded
void *hack_thread(void *) {
    LOGI(OBFUSCATE("hack_thread started for GTA SA FPS"));

    // Check if target lib is loaded
    do {
        sleep(1);
    } while (!isLibraryLoaded(targetLibName));

    LOGI(OBFUSCATE("%s has been loaded"), (const char *) targetLibName);

    initUnityPointers();

#if defined(__aarch64__)
    // Hook NetworkPlayer lifecycle
    HOOK("0x42CC8C8", hook_NetworkPlayer_Update, old_NetworkPlayer_Update);
    HOOK("0x42C9540", hook_NetworkPlayer_OnDestroy, old_NetworkPlayer_OnDestroy);

    // Hook BotPlayer lifecycle
    HOOK("0x44493F4", hook_BotPlayer_Start, old_BotPlayer_Start);
    HOOK("0x444A310", hook_BotPlayer_Update, old_BotPlayer_Update);
    HOOK("0x4449EB0", hook_BotPlayer_OnDestroy, old_BotPlayer_OnDestroy);

    // Hook ThirdPersonController (3D character models in scene)
    HOOK("0x43607BC", hook_ThirdPersonController_Update, old_ThirdPersonController_Update);
    HOOK("0x4360FB8", hook_ThirdPersonController_OnDestroy, old_ThirdPersonController_OnDestroy);

    // Hook AimIkController (LateUpdate after animation / IK evaluation)
    HOOK("0x407C85C", hook_AimIkController_LateUpdate, old_AimIkController_LateUpdate);

    LOGI(OBFUSCATE("Player, Bot, and Big Head hooks installed successfully!"));
#else
    LOGI(OBFUSCATE("GTA SA FPS 64-bit target only."));
#endif

    return NULL;
}

// =========================================================================
// LGL Menu Features & JNI Interface
// =========================================================================

jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
        OBFUSCATE("Category_🎮 FITUR GTA SA FPS"),
        OBFUSCATE("Toggle_Status Panel Overlay"), // featNum 0: Panel HUD & Auto Hitung Player/Bot
        OBFUSCATE("Toggle_Big Head"),             // featNum 1: Ukuran kepala player & bot membesar (Client-side)
        OBFUSCATE("Category_📊 STATUS PANEL INFO"),
        OBFUSCATE("RichTextView_<div style='background-color:#16222F;padding:10px;border:1px solid #00E5FF;border-radius:6px;'><font color='#00FF7F'><b>[ STATUS PANEL & BIG HEAD ]</b></font><br><font color='#FFFFFF'>• <b>Status Panel Overlay:</b> Cukup aktifkan toggle ini untuk menampilkan HUD dan otomatis menghitung jumlah Player & Bot secara real-time.<br><br>• <b>Big Head:</b> Memperbesar ukuran kepala (body part) Player & Bot secara client-side (hanya di sisi client/layar Anda).</font></div>")
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}

void Changes(JNIEnv *env, jclass clazz, jobject ctx,
             jint featNum, jstring featName, jint value,
             jboolean boolean, jstring str) {

    LOGD(OBFUSCATE("Changes: featNum=%d, val=%d, bool=%d"), featNum, value, boolean);

    switch (featNum) {
        case 0: // Toggle_Status Panel Overlay (Sekaligus Auto Count Player & Bot)
            g_showStatusPanel = boolean;
            g_autoCount = boolean;
            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Status Panel & Auto Counter: ON"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Status Panel & Auto Counter: OFF"), ToastLength::LENGTH_SHORT);
            }
            break;

        case 1: // Toggle_Big Head
            g_bigHead = boolean;
            if (!boolean) {
                g_needsBigHeadReset = true;
                g_bigHeadResetFrames = 120; // Reset scale back across next 120 frames
                Toast(env, ctx, OBFUSCATE("Big Head: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Big Head: ON (Kepala Membesar)"), ToastLength::LENGTH_SHORT);
            }
            break;
    }
}

// Native getters for Java UI integration
jstring GetPlayerBotStatus(JNIEnv *env, jobject thiz) {
    EntityStats stats = getEntityStats();
    char buf[256];
    snprintf(buf, sizeof(buf), "%s;%d;%d;%d",
             stats.inGame ? "IN_GAME" : "LOBBY",
             stats.realPlayerCount,
             stats.botCount,
             stats.totalEntities);
    return env->NewStringUTF(buf);
}

jint GetPlayerCount(JNIEnv *env, jobject thiz) {
    return getEntityStats().realPlayerCount;
}

jint GetBotCount(JNIEnv *env, jobject thiz) {
    return getEntityStats().botCount;
}

jint GetTotalCount(JNIEnv *env, jobject thiz) {
    return getEntityStats().totalEntities;
}

jboolean IsInGame(JNIEnv *env, jobject thiz) {
    return getEntityStats().inGame;
}

void ResetEntityCounters(JNIEnv *env, jobject thiz) {
    resetEntityCounters();
}

__attribute__((constructor))
void lib_main() {
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}

int RegisterMenu(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("Icon"), OBFUSCATE("()Ljava/lang/String;"), reinterpret_cast<void *>(Icon)},
            {OBFUSCATE("IconWebViewData"),  OBFUSCATE("()Ljava/lang/String;"), reinterpret_cast<void *>(IconWebViewData)},
            {OBFUSCATE("IsGameLibLoaded"),  OBFUSCATE("()Z"), reinterpret_cast<void *>(isGameLibLoaded)},
            {OBFUSCATE("Init"),  OBFUSCATE("(Landroid/content/Context;Landroid/widget/TextView;Landroid/widget/TextView;)V"), reinterpret_cast<void *>(Init)},
            {OBFUSCATE("SettingsList"),  OBFUSCATE("()[Ljava/lang/String;"), reinterpret_cast<void *>(SettingsList)},
            {OBFUSCATE("GetFeatureList"),  OBFUSCATE("()[Ljava/lang/String;"), reinterpret_cast<void *>(GetFeatureList)},
            {OBFUSCATE("GetPlayerBotStatus"), OBFUSCATE("()Ljava/lang/String;"), reinterpret_cast<void *>(GetPlayerBotStatus)},
            {OBFUSCATE("GetPlayerCount"), OBFUSCATE("()I"), reinterpret_cast<void *>(GetPlayerCount)},
            {OBFUSCATE("GetBotCount"), OBFUSCATE("()I"), reinterpret_cast<void *>(GetBotCount)},
            {OBFUSCATE("GetTotalCount"), OBFUSCATE("()I"), reinterpret_cast<void *>(GetTotalCount)},
            {OBFUSCATE("IsInGame"), OBFUSCATE("()Z"), reinterpret_cast<void *>(IsInGame)},
            {OBFUSCATE("ResetEntityCounters"), OBFUSCATE("()V"), reinterpret_cast<void *>(ResetEntityCounters)},
    };

    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Menu"));
    if (!clazz)
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;
    return JNI_OK;
}

int RegisterPreferences(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("Changes"), OBFUSCATE("(Landroid/content/Context;ILjava/lang/String;IZLjava/lang/String;)V"), reinterpret_cast<void *>(Changes)},
    };
    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Preferences"));
    if (!clazz)
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;
    return JNI_OK;
}

int RegisterMain(JNIEnv *env) {
    JNINativeMethod methods[] = {
            {OBFUSCATE("CheckOverlayPermission"), OBFUSCATE("(Landroid/content/Context;)V"), reinterpret_cast<void *>(CheckOverlayPermission)},
    };
    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Main"));
    if (!clazz)
        return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0)
        return JNI_ERR;

    return JNI_OK;
}

extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    vm->GetEnv((void **) &env, JNI_VERSION_1_6);
    if (RegisterMenu(env) != 0)
        return JNI_ERR;
    if (RegisterPreferences(env) != 0)
        return JNI_ERR;
    if (RegisterMain(env) != 0)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}
