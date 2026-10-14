#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include "Core/Logger.h"
#include "Core/CrashHandler.h"
#include "Core/MemoryHelper.h"
#include "Features/FeatureManager.h"
#include "Features/StatusPanel.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "Menu/Setup.h"

// =========================================================================
// Background Worker Thread
// Waits for libil2cpp.so to load and initializes modular game features
// =========================================================================

static void* hack_thread(void *) {
    LOG_INFO("[THREAD] Background thread started. Waiting for %s...", Offsets::TargetLib::Name);
    ModLogger::SetLastAction("Waiting for libil2cpp.so");

    while (!Memory::IsLibraryLoaded()) {
        sleep(1);
    }

    uintptr_t base = Memory::GetLibBase();
    LOG_INFO("[THREAD] %s loaded successfully at base: 0x%lx", Offsets::TargetLib::Name, base);
    ModLogger::SetLastAction("libil2cpp.so loaded");

    // Initialize all modular game features
    FeatureManager::InitAll();

    ModLogger::SetLastAction("Features loaded and active");
    return nullptr;
}

// =========================================================================
// Native Library Constructor (Runs upon dlopen / System.loadLibrary)
// =========================================================================

__attribute__((constructor))
void lib_main() {
    ModLogger::InitLogger();
    CrashHandler::Install();

    LOG_INFO("=================================================================");
    LOG_INFO("        GTA SA FPS MOD MENU LOADED (MODULAR ENGINE)             ");
    LOG_INFO("=================================================================");
    LOG_INFO("[INIT] Active Log Path: %s", ModLogger::GetActiveLogPath());

    pthread_t ptid;
    pthread_create(&ptid, nullptr, hack_thread, nullptr);
}

// =========================================================================
// Native Bridge Getters for Java UI Integration
// =========================================================================

static jstring Native_GetPlayerBotStatus(JNIEnv *env, jobject thiz) {
    return env->NewStringUTF(StatusPanel::GetFormattedStatus().c_str());
}

static jint Native_GetPlayerCount(JNIEnv *env, jobject thiz) {
    return StatusPanel::GetStats().realPlayerCount;
}

static jint Native_GetBotCount(JNIEnv *env, jobject thiz) {
    return StatusPanel::GetStats().botCount;
}

static jint Native_GetTotalCount(JNIEnv *env, jobject thiz) {
    return StatusPanel::GetStats().totalEntities;
}

static jboolean Native_IsInGame(JNIEnv *env, jobject thiz) {
    return StatusPanel::GetStats().inGame;
}

static void Native_ResetEntityCounters(JNIEnv *env, jobject thiz) {
    StatusPanel::ResetCounters();
}

// =========================================================================
// JNI Native Registrations
// =========================================================================

static int RegisterMenu(JNIEnv *env) {
    JNINativeMethod methods[] = {
        {OBFUSCATE("Icon"),                OBFUSCATE("()Ljava/lang/String;"),                                                                 reinterpret_cast<void *>(Icon)},
        {OBFUSCATE("IconWebViewData"),     OBFUSCATE("()Ljava/lang/String;"),                                                                 reinterpret_cast<void *>(IconWebViewData)},
        {OBFUSCATE("IsGameLibLoaded"),     OBFUSCATE("()Z"),                                                                                 reinterpret_cast<void *>(isGameLibLoaded)},
        {OBFUSCATE("Init"),                OBFUSCATE("(Landroid/content/Context;Landroid/widget/TextView;Landroid/widget/TextView;)V"),       reinterpret_cast<void *>(Init)},
        {OBFUSCATE("SettingsList"),        OBFUSCATE("()[Ljava/lang/String;"),                                                               reinterpret_cast<void *>(SettingsList)},
        {OBFUSCATE("GetFeatureList"),      OBFUSCATE("()[Ljava/lang/String;"),                                                               reinterpret_cast<void *>(FeatureManager::GetFeatureList)},
        {OBFUSCATE("GetPlayerBotStatus"),  OBFUSCATE("()Ljava/lang/String;"),                                                               reinterpret_cast<void *>(Native_GetPlayerBotStatus)},
        {OBFUSCATE("GetPlayerCount"),      OBFUSCATE("()I"),                                                                                 reinterpret_cast<void *>(Native_GetPlayerCount)},
        {OBFUSCATE("GetBotCount"),         OBFUSCATE("()I"),                                                                                 reinterpret_cast<void *>(Native_GetBotCount)},
        {OBFUSCATE("GetTotalCount"),       OBFUSCATE("()I"),                                                                                 reinterpret_cast<void *>(Native_GetTotalCount)},
        {OBFUSCATE("IsInGame"),            OBFUSCATE("()Z"),                                                                                 reinterpret_cast<void *>(Native_IsInGame)},
        {OBFUSCATE("ResetEntityCounters"), OBFUSCATE("()V"),                                                                                 reinterpret_cast<void *>(Native_ResetEntityCounters)},
    };

    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Menu"));
    if (!clazz) return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0) return JNI_ERR;
    return JNI_OK;
}

static int RegisterPreferences(JNIEnv *env) {
    JNINativeMethod methods[] = {
        {OBFUSCATE("Changes"), OBFUSCATE("(Landroid/content/Context;ILjava/lang/String;IZLjava/lang/String;)V"), reinterpret_cast<void *>(FeatureManager::OnToggle)},
    };

    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Preferences"));
    if (!clazz) return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0) return JNI_ERR;
    return JNI_OK;
}

static int RegisterMain(JNIEnv *env) {
    JNINativeMethod methods[] = {
        {OBFUSCATE("CheckOverlayPermission"), OBFUSCATE("(Landroid/content/Context;)V"), reinterpret_cast<void *>(CheckOverlayPermission)},
    };

    jclass clazz = env->FindClass(OBFUSCATE("com/android/support/Main"));
    if (!clazz) return JNI_ERR;
    if (env->RegisterNatives(clazz, methods, sizeof(methods) / sizeof(methods[0])) != 0) return JNI_ERR;
    return JNI_OK;
}

extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    if (RegisterMenu(env) != 0) return JNI_ERR;
    if (RegisterPreferences(env) != 0) return JNI_ERR;
    if (RegisterMain(env) != 0) return JNI_ERR;

    return JNI_VERSION_1_6;
}
