#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <jni.h>
#include <list>
#include <mutex>
#include <pthread.h>
#include <set>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "KittyMemory/MemoryPatch.h"
#include "Menu/Setup.h"

// Target library for GTA SA FPS (il2cpp 64-bit)
#define targetLibName OBFUSCATE("libil2cpp.so")

#include "Includes/Macros.h"

// =========================================================================
// Real-time Player & Bot Tracking System
// =========================================================================
static std::mutex g_entityMutex;
static std::unordered_map<void *, uint64_t>
    g_networkPlayers; // NetworkPlayer* -> last seen ms
static std::unordered_map<void *, uint64_t>
    g_botPlayers; // BotPlayer* -> last seen ms
static std::unordered_map<void *, void *>
    g_botNetPlayers; // BotPlayer* -> NetworkPlayer*

static bool g_showStatusPanel = false;
static bool g_autoCount = true;

static uint64_t getCurrentTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static void onNetworkPlayerUpdate(void *instance) {
  if (instance == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_entityMutex);
  g_networkPlayers[instance] = getCurrentTimeMs();
}

static void onNetworkPlayerDestroy(void *instance) {
  if (instance == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_entityMutex);
  g_networkPlayers.erase(instance);
}

static void onBotPlayerUpdate(void *instance) {
  if (instance == nullptr)
    return;
  std::lock_guard<std::mutex> lock(g_entityMutex);
  g_botPlayers[instance] = getCurrentTimeMs();

  // Read NetworkPlayer field at offset 0x50 in BotPlayer struct
  void *netPlayer = *(void **)((uintptr_t)instance + 0x50);
  if (netPlayer != nullptr) {
    g_botNetPlayers[instance] = netPlayer;
  }
}

static void onBotPlayerDestroy(void *instance) {
  if (instance == nullptr)
    return;
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
  std::set<void *> activeBotNets;
  for (auto it = g_botPlayers.begin(); it != g_botPlayers.end();) {
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
  for (auto it = g_networkPlayers.begin(); it != g_networkPlayers.end();) {
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

  // Fallback if botNetPlayers offset was detached
  if (totalNetPlayers >= botCount) {
    if (realCount == 0 && totalNetPlayers > botCount) {
      realCount = totalNetPlayers - botCount;
    }
  } else {
    if (realCount < 0)
      realCount = 0;
  }

  int total =
      (totalNetPlayers > botCount) ? totalNetPlayers : (realCount + botCount);

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
// Il2Cpp Hooks for NetworkPlayer & BotPlayer
// =========================================================================

// NetworkPlayer.Update: RVA 0x42CC8C8
void (*old_NetworkPlayer_Update)(void *instance) = nullptr;
void hook_NetworkPlayer_Update(void *instance) {
  if (instance != nullptr && g_autoCount) {
    onNetworkPlayerUpdate(instance);
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

// Thread to hook functions once cpp.so is loaded
void *hack_thread(void *) {
  LOGI(OBFUSCATE("hack_thread started for GTA SA FPS"));

  // Check if target lib is loaded
  do {
    sleep(1);
  } while (!isLibraryLoaded(targetLibName));

  LOGI(OBFUSCATE("%s has been loaded"), (const char *)targetLibName);

#if defined(__aarch64__)
  // Hook NetworkPlayer lifecycle
  HOOK("0x42CC8C8", hook_NetworkPlayer_Update, old_NetworkPlayer_Update);
  HOOK("0x42C9540", hook_NetworkPlayer_OnDestroy, old_NetworkPlayer_OnDestroy);

  // Hook BotPlayer lifecycle
  HOOK("0x44493F4", hook_BotPlayer_Start, old_BotPlayer_Start);
  HOOK("0x444A310", hook_BotPlayer_Update, old_BotPlayer_Update);
  HOOK("0x4449EB0", hook_BotPlayer_OnDestroy, old_BotPlayer_OnDestroy);

  LOGI(OBFUSCATE("Player & Bot hooks installed successfully!"));
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
      OBFUSCATE("Toggle_Status Panel Overlay"),         // featNum 0
      OBFUSCATE("Toggle_True_Auto Count Player & Bot"), // featNum 1
      OBFUSCATE("Button_Hitung Player & Bot Sekarang"), // featNum 2
      OBFUSCATE("Button_Reset Counter"),                // featNum 3
      OBFUSCATE("Category_📊 STATUS PANEL INFO"),
      OBFUSCATE(
          "RichTextView_<div "
          "style='background-color:#16222F;padding:10px;border:1px solid "
          "#00E5FF;border-radius:6px;'><font color='#00FF7F'><b>[ STATUS PANEL "
          "INFO ]</b></font><br><font color='#FFFFFF'>Fitur ini mendeteksi & "
          "menghitung jumlah <b>Pemain Nyata</b> dan <b>Bot</b> secara "
          "real-time di dalam game.</font><br><br><font color='#00E5FF'>• "
          "<b>Status Panel Overlay:</b> Menampilkan floating HUD di "
          "layar</font><br><font color='#00FF00'>• <b>Auto Count:</b> "
          "Memperbarui status otomatis</font><br><font color='#FFD700'>• "
          "<b>Hitung Sekarang:</b> Hitung seketika & popup info</font></div>")};

  int Total_Feature = (sizeof features / sizeof features[0]);
  ret = (jobjectArray)env->NewObjectArray(
      Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
      env->NewStringUTF(""));

  for (int i = 0; i < Total_Feature; i++)
    env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

  return (ret);
}

void Changes(JNIEnv *env, jclass clazz, jobject ctx, jint featNum,
             jstring featName, jint value, jboolean boolean, jstring str) {

  LOGD(OBFUSCATE("Changes: featNum=%d, val=%d, bool=%d"), featNum, value,
       boolean);

  switch (featNum) {
  case 0: // Toggle_Status Panel Overlay
    g_showStatusPanel = boolean;
    break;

  case 1: // Toggle_Auto Count Player & Bot
    g_autoCount = boolean;
    break;

  case 2: // Button_Hitung Player & Bot Sekarang
  {
    EntityStats stats = getEntityStats();
    char msg[512];
    if (stats.inGame) {
      snprintf(msg, sizeof(msg),
               "🎮 [STATUS PANEL - IN GAME]\n\n"
               "👤 Real Player: %d\n"
               "🤖 Bot: %d\n"
               "👥 Total Karakter: %d\n"
               "⚡ Status: Aktif Di Dalam Game",
               stats.realPlayerCount, stats.botCount, stats.totalEntities);
    } else {
      snprintf(msg, sizeof(msg),
               "⚠️ [STATUS PANEL]\n\n"
               "Belum ada pemain atau bot yang terdeteksi di match.\n"
               "Silakan masuk ke dalam pertempuran/match!\n"
               "(Player: 0 | Bot: 0)");
    }
    Toast(env, ctx, msg, ToastLength::LENGTH_LONG);
    break;
  }

  case 3: // Button_Reset Counter
  {
    resetEntityCounters();
    Toast(env, ctx, OBFUSCATE("Counter Player & Bot berhasil di-reset!"),
          ToastLength::LENGTH_SHORT);
    break;
  }
  }
}

// Native getters for Java UI integration
jstring GetPlayerBotStatus(JNIEnv *env, jobject thiz) {
  EntityStats stats = getEntityStats();
  char buf[256];
  snprintf(buf, sizeof(buf), "%s;%d;%d;%d", stats.inGame ? "IN_GAME" : "LOBBY",
           stats.realPlayerCount, stats.botCount, stats.totalEntities);
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

jboolean IsInGame(JNIEnv *env, jobject thiz) { return getEntityStats().inGame; }

void ResetEntityCounters(JNIEnv *env, jobject thiz) { resetEntityCounters(); }

__attribute__((constructor)) void lib_main() {
  pthread_t ptid;
  pthread_create(&ptid, NULL, hack_thread, NULL);
}

int RegisterMenu(JNIEnv *env) {
  JNINativeMethod methods[] = {
      {OBFUSCATE("Icon"), OBFUSCATE("()Ljava/lang/String;"),
       reinterpret_cast<void *>(Icon)},
      {OBFUSCATE("IconWebViewData"), OBFUSCATE("()Ljava/lang/String;"),
       reinterpret_cast<void *>(IconWebViewData)},
      {OBFUSCATE("IsGameLibLoaded"), OBFUSCATE("()Z"),
       reinterpret_cast<void *>(isGameLibLoaded)},
      {OBFUSCATE("Init"),
       OBFUSCATE("(Landroid/content/Context;Landroid/widget/TextView;Landroid/"
                 "widget/TextView;)V"),
       reinterpret_cast<void *>(Init)},
      {OBFUSCATE("SettingsList"), OBFUSCATE("()[Ljava/lang/String;"),
       reinterpret_cast<void *>(SettingsList)},
      {OBFUSCATE("GetFeatureList"), OBFUSCATE("()[Ljava/lang/String;"),
       reinterpret_cast<void *>(GetFeatureList)},
      {OBFUSCATE("GetPlayerBotStatus"), OBFUSCATE("()Ljava/lang/String;"),
       reinterpret_cast<void *>(GetPlayerBotStatus)},
      {OBFUSCATE("GetPlayerCount"), OBFUSCATE("()I"),
       reinterpret_cast<void *>(GetPlayerCount)},
      {OBFUSCATE("GetBotCount"), OBFUSCATE("()I"),
       reinterpret_cast<void *>(GetBotCount)},
      {OBFUSCATE("GetTotalCount"), OBFUSCATE("()I"),
       reinterpret_cast<void *>(GetTotalCount)},
      {OBFUSCATE("IsInGame"), OBFUSCATE("()Z"),
       reinterpret_cast<void *>(IsInGame)},
      {OBFUSCATE("ResetEntityCounters"), OBFUSCATE("()V"),
       reinterpret_cast<void *>(ResetEntityCounters)},
  };

  jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Menu"));
  if (!clazz)
    return JNI_ERR;
  if (env->RegisterNatives(clazz, methods,
                           sizeof(methods) / sizeof(methods[0])) != 0)
    return JNI_ERR;
  return JNI_OK;
}

int RegisterPreferences(JNIEnv *env) {
  JNINativeMethod methods[] = {
      {OBFUSCATE("Changes"),
       OBFUSCATE("(Landroid/content/Context;ILjava/lang/String;IZLjava/lang/"
                 "String;)V"),
       reinterpret_cast<void *>(Changes)},
  };
  jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Preferences"));
  if (!clazz)
    return JNI_ERR;
  if (env->RegisterNatives(clazz, methods,
                           sizeof(methods) / sizeof(methods[0])) != 0)
    return JNI_ERR;
  return JNI_OK;
}

int RegisterMain(JNIEnv *env) {
  JNINativeMethod methods[] = {
      {OBFUSCATE("CheckOverlayPermission"),
       OBFUSCATE("(Landroid/content/Context;)V"),
       reinterpret_cast<void *>(CheckOverlayPermission)},
  };
  jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Main"));
  if (!clazz)
    return JNI_ERR;
  if (env->RegisterNatives(clazz, methods,
                           sizeof(methods) / sizeof(methods[0])) != 0)
    return JNI_ERR;

  return JNI_OK;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  JNIEnv *env;
  vm->GetEnv((void **)&env, JNI_VERSION_1_6);
  if (RegisterMenu(env) != 0)
    return JNI_ERR;
  if (RegisterPreferences(env) != 0)
    return JNI_ERR;
  if (RegisterMain(env) != 0)
    return JNI_ERR;
  return JNI_VERSION_1_6;
}
