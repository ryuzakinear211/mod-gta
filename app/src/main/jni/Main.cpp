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

static void *(*get_transform)(void *) = nullptr;
static void (*set_localScale)(void *, Vector3) = nullptr;

static void initUnityPointers() {
    if (get_transform == nullptr) {
        get_transform = (void *(*)(void *)) getAbsoluteAddress(targetLibName, 0x8597C20);
    }
    if (set_localScale == nullptr) {
        set_localScale = (void (*)(void *, Vector3)) getAbsoluteAddress(targetLibName, 0x85B21BC);
    }
}

// =========================================================================
// Real-time Player & Bot Tracking System
// =========================================================================
static std::mutex g_entityMutex;
static std::unordered_map<void*, uint64_t> g_networkPlayers; // NetworkPlayer* -> last seen ms
static std::unordered_map<void*, uint64_t> g_botPlayers;     // BotPlayer* -> last seen ms
static std::unordered_map<void*, void*> g_botNetPlayers;     // BotPlayer* -> NetworkPlayer*

static bool g_showStatusPanel = false;
static bool g_autoCount = false;
static bool g_bigHead = false;
static bool g_needsBigHeadReset = false;
static int g_bigHeadResetFrames = 0;

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
// Big Head Implementation
// =========================================================================
static void applyBigHead(void *playerInstance, bool enabled) {
    if (playerInstance == nullptr) return;

    initUnityPointers();
    if (get_transform == nullptr || set_localScale == nullptr) return;

    Vector3 targetScale = enabled ? Vector3(2.3f, 2.3f, 2.3f) : Vector3(1.0f, 1.0f, 1.0f);

    // Access BodyPointsManager at offset 0xC8 of NetworkPlayer
    void *bpm = *(void **)((uintptr_t)playerInstance + 0xC8);
    if (bpm != nullptr) {
        // _bodyPoints array at offset 0x20
        void *bodyPointsArr = *(void **)((uintptr_t)bpm + 0x20);
        if (bodyPointsArr != nullptr) {
            // Read array length at offset 0x18 in Il2Cpp array
            uintptr_t length = *(uintptr_t *)((uintptr_t)bodyPointsArr + 0x18);
            if (length > 0 && length < 32) {
                void **items = (void **)((uintptr_t)bodyPointsArr + 0x20);
                for (uintptr_t i = 0; i < length; i++) {
                    void *bodyPoint = items[i];
                    if (bodyPoint != nullptr) {
                        // BodyPointView at offset 0x10 of BodyPoint
                        void *bpView = *(void **)((uintptr_t)bodyPoint + 0x10);
                        if (bpView != nullptr) {
                            // BodyPointType at offset 0x20 of BodyPointView (0 = Head)
                            int pointType = *(int *)((uintptr_t)bpView + 0x20);
                            if (pointType == 0) {
                                void *headTransform = get_transform(bpView);
                                if (headTransform != nullptr) {
                                    set_localScale(headTransform, targetScale);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// =========================================================================
// Il2Cpp Hooks for NetworkPlayer & BotPlayer
// =========================================================================

// NetworkPlayer.Update: RVA 0x42CC8C8
void (*old_NetworkPlayer_Update)(void *instance) = nullptr;
void hook_NetworkPlayer_Update(void *instance) {
    if (instance != nullptr) {
        if (g_autoCount) {
            onNetworkPlayerUpdate(instance);
        }
        if (g_bigHead) {
            applyBigHead(instance, true);
        } else if (g_needsBigHeadReset) {
            applyBigHead(instance, false);
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
    if (instance != nullptr && g_autoCount) {
        onBotPlayerUpdate(instance);
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
    }
    if (old_BotPlayer_OnDestroy != nullptr) {
        old_BotPlayer_OnDestroy(instance);
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

#if defined(__aarch64__)
    // Hook NetworkPlayer lifecycle
    HOOK("0x42CC8C8", hook_NetworkPlayer_Update, old_NetworkPlayer_Update);
    HOOK("0x42C9540", hook_NetworkPlayer_OnDestroy, old_NetworkPlayer_OnDestroy);

    // Hook BotPlayer lifecycle
    HOOK("0x44493F4", hook_BotPlayer_Start, old_BotPlayer_Start);
    HOOK("0x444A310", hook_BotPlayer_Update, old_BotPlayer_Update);
    HOOK("0x4449EB0", hook_BotPlayer_OnDestroy, old_BotPlayer_OnDestroy);

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
        OBFUSCATE("Toggle_Big Head"),             // featNum 1: Ukuran kepala player & bot membesar
        OBFUSCATE("Category_📊 STATUS PANEL INFO"),
        OBFUSCATE("RichTextView_<div style='background-color:#16222F;padding:10px;border:1px solid #00E5FF;border-radius:6px;'><font color='#00FF7F'><b>[ STATUS PANEL & BIG HEAD ]</b></font><br><font color='#FFFFFF'>• <b>Status Panel Overlay:</b> Cukup aktifkan toggle ini untuk menampilkan HUD dan otomatis menghitung jumlah Player & Bot secara real-time.<br><br>• <b>Big Head:</b> Memperbesar ukuran kepala (body part) Player & Bot saat di game.</font></div>")
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
                g_bigHeadResetFrames = 60; // Reset scale back across next 60 frames
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
