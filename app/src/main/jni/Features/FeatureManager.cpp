#include "FeatureManager.h"
#include "FastFireRate.h"
#include "NoRecoil.h"
#include "BigHead.h"
#include "StatusPanel.h"
#include "FPSUnlock.h"
#include "ESP.h"
#include "Chams.h"
#include "ResetRewardTime.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include "../Includes/obfuscate.h"
#include "../Menu/Setup.h"

namespace FeatureManager {

    // Central FirstPersonController.Update Hook: RVA 0x40A1BF4
    static void (*old_FPC_Update)(void *instance) = nullptr;
    static uint32_t s_fpsFrameCounter = 0;
    static void *s_lastFpcInstance = nullptr;

    // SceneManager.LoadSceneAsyncNameIndexInternal: RVA 0x85BBA90
    static void* (*old_SceneManager_LoadScene)(void *sceneName, int buildIndex, void *parameters, bool mustCompleteNextFrame) = nullptr;
    static void* hook_SceneManager_LoadScene(void *sceneName, int buildIndex, void *parameters, bool mustCompleteNextFrame) {
        // Otomatis bersihkan shader program cache dan posisi tim saat scene/map berpindah antar match
        Chams::OnMatchFinished();
        if (old_SceneManager_LoadScene) {
            return old_SceneManager_LoadScene(sceneName, buildIndex, parameters, mustCompleteNextFrame);
        }
        return nullptr;
    }

    // Vehicle lifecycle hooks to dynamically filter tank & vehicle meshes in Chams
    static void (*old_RTC_Tank_Update)(void *instance) = nullptr;
    static void hook_RTC_Tank_Update(void *instance) {
        if (instance) {
            Chams::RegisterVehicleInstance(instance);
        }
        if (old_RTC_Tank_Update) old_RTC_Tank_Update(instance);
    }

    static void (*old_RCC_Car_FixedUpdate)(void *instance) = nullptr;
    static void hook_RCC_Car_FixedUpdate(void *instance) {
        if (instance) {
            Chams::RegisterVehicleInstance(instance);
        }
        if (old_RCC_Car_FixedUpdate) old_RCC_Car_FixedUpdate(instance);
    }

    static void (*old_RaidTank_Update)(void *instance) = nullptr;
    static void hook_RaidTank_Update(void *instance) {
        if (instance) {
            Chams::RegisterVehicleInstance(instance);
        }
        if (old_RaidTank_Update) old_RaidTank_Update(instance);
    }

    static void hook_FPC_Update(void *instance) {
        if (instance) {
            // Ketika player baru spawn atau match baru dimulai: bersihkan cache shader match sebelumnya
            if (instance != s_lastFpcInstance) {
                s_lastFpcInstance = instance;
                Chams::ClearProgramCache();
                Chams::ClearTeammatePositions();
                Chams::ClearVehiclePositions();
            }

            FastFireRate::OnLocalPlayerUpdate(instance);
            NoRecoil::OnLocalPlayerUpdate(instance);
            ESP::OnLocalPlayerUpdate(instance);
            StatusPanel::OnLocalPlayerUpdate(instance);

            // Re-enforce 60 FPS across level loads and menu transitions
            if (++s_fpsFrameCounter % 120 == 0) {
                FPSUnlock::ApplyFrameRate();
            }
        }
        if (old_FPC_Update) old_FPC_Update(instance);
    }

    void InitAll() {
        LOG_INFO("=================================================================");
        LOG_INFO("Initializing GTA SA FPS Modular Features from dump.cs...");
        LOG_INFO("=================================================================");

        FastFireRate::Init();
        NoRecoil::Init();
        BigHead::Init();
        StatusPanel::Init();
        FPSUnlock::Init();
        ESP::Init();
        Chams::Init();
        ResetRewardTime::Init();

        // Install local player lifecycle hook
        Memory::HookRVA(Offsets::FPC::Update, reinterpret_cast<void*>(hook_FPC_Update), reinterpret_cast<void**>(&old_FPC_Update));

        // Install scene transition hook for match-to-match cleanup
        Memory::HookRVA(Offsets::SceneManagement::LoadSceneAsync, reinterpret_cast<void*>(hook_SceneManager_LoadScene), reinterpret_cast<void**>(&old_SceneManager_LoadScene));

        // Hook vehicle lifecycle functions to register real-time vehicle positions for Chams filtering
        Memory::HookRVA(Offsets::Vehicles::RTC_TankUpdate, reinterpret_cast<void*>(hook_RTC_Tank_Update), reinterpret_cast<void**>(&old_RTC_Tank_Update));
        Memory::HookRVA(Offsets::Vehicles::RCC_CarFixedUpdate, reinterpret_cast<void*>(hook_RCC_Car_FixedUpdate), reinterpret_cast<void**>(&old_RCC_Car_FixedUpdate));
        Memory::HookRVA(Offsets::Vehicles::RaidTankUpdate, reinterpret_cast<void*>(hook_RaidTank_Update), reinterpret_cast<void**>(&old_RaidTank_Update));

        LOG_INFO("[FEATURE_MGR] All modules and core hooks initialized successfully.");
    }

    jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
        const char *features[] = {
            OBFUSCATE("Category_🎮 FITUR GTA SA FPS"),
            OBFUSCATE("Toggle_Status Panel Overlay"),         // featNum 0
            OBFUSCATE("Toggle_Big Head"),                     // featNum 1
            OBFUSCATE("Toggle_Fast FireRate (Client-Side)"), // featNum 2
            OBFUSCATE("Toggle_No Recoil (Client-Side)"),     // featNum 3
            OBFUSCATE("Toggle_Unlock 60 FPS (Ultra Smooth)"),// featNum 4
            OBFUSCATE("Category_👁️ FITUR ESP ENEMY"),
            OBFUSCATE("Toggle_ESP Master (Box & Line)"),      // featNum 5
            OBFUSCATE("Toggle_True_ESP Enemy Only (Filter Tim)"), // featNum 6
            OBFUSCATE("Toggle_True_ESP Line Tracer"),         // featNum 7
            OBFUSCATE("Toggle_True_ESP Health Bar & Info"),   // featNum 8
            OBFUSCATE("Category_🌈 FITUR CHAMS (RENDER THROUGH WALLS)"),
            OBFUSCATE("Toggle_Chams Master (Tembus Pandang)"), // featNum 9
            OBFUSCATE("Toggle_True_Chams Enemy Only (Filter Tim)"), // featNum 10
            OBFUSCATE("Spinner_Mode Chams_Shading (Wallhack),Default,Wireframe,Glow,Outline,Rainbow"), // featNum 11
            OBFUSCATE("SeekBar_Warna Merah_0_255"),            // featNum 12
            OBFUSCATE("SeekBar_Warna Hijau_0_255"),            // featNum 13
            OBFUSCATE("SeekBar_Warna Biru_0_255"),             // featNum 14
            OBFUSCATE("SeekBar_Tebal Garis_1_10"),             // featNum 15
            OBFUSCATE("Category_🎁 FITUR FREE REWARD & GOLD"),
            OBFUSCATE("Toggle_Reset Reward Time (1h 57m -> 0s)"), // featNum 16
            OBFUSCATE("Category_📊 STATUS & DEBUG INFO"),
            OBFUSCATE("RichTextView_<div style='background-color:#16222F;padding:10px;border:1px solid #00E5FF;border-radius:6px;'>"
                      "<font color='#00FF7F'><b>[ GTA SA FPS MOD MENU (MODULAR) ]</b></font><br>"
                      "<font color='#FFFFFF'>"
                      "• <b>Reset Reward Time:</b> Reset timer cooldown 1h 57m pada Free Reward (Hadiah Gratis/Gold) menjadi 0 detik agar dapat langsung diambil berulang kali tanpa menunggu.<br><br>"
                      "• <b>ESP Master:</b> Deteksi posisi player/bot secara visual 2D box & line.<br><br>"
                      "• <b>Chams:</b> Tembus pandang / render karakter tembus dinding (OpenGL-ES) Anti-Map & Anti-Vehicle.<br><br>"
                      "• <b>Chams Enemy Only:</b> Filter tim aktif: hanya karakter musuh yang tembus pandang.<br><br>"
                      "• <b>Enemy Only ESP:</b> Otomatis memfilter dan menyembunyikan teman/tim sendiri pada ESP.<br><br>"
                      "• <b>Status Panel:</b> HUD real-time counter Player & Bot.<br><br>"
                      "• <b>Big Head:</b> Memperbesar kepala Player & Bot (Client-Side).<br><br>"
                      "• <b>Fast FireRate:</b> Tembakan kecepatan tinggi (Client-Side).<br><br>"
                      "• <b>No Recoil:</b> 0% hentakan senjata (Bidikan lurus stabil).<br><br>"
                      "• <b>Unlock 60 FPS:</b> Buka limit 30 FPS untuk gameplay 60 FPS halus.<br><br>"
                      "• <b>Engine:</b> Modular Architecture berbasis dump.cs."
                      "</font></div>")
        };

        int total = sizeof(features) / sizeof(features[0]);
        jobjectArray ret = env->NewObjectArray(total, env->FindClass(OBFUSCATE("java/lang/String")), env->NewStringUTF(""));

        for (int i = 0; i < total; i++) {
            env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
        }

        return ret;
    }

    void OnToggle(JNIEnv *env, jclass clazz, jobject ctx,
                  jint featNum, jstring featName, jint value,
                  jboolean boolean, jstring str) {

        LOG_INFO("[TOGGLE] Feature #%d changed -> boolean=%d, value=%d", featNum, (int)boolean, value);

        switch (featNum) {
            case 0: { // Status Panel Overlay
                StatusPanel::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "StatusPanel: ON" : "StatusPanel: OFF");
                if (boolean && ctx != nullptr) {
                    Toast(env, ctx, OBFUSCATE("Status Panel & Counter: ON"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 1: { // Big Head
                BigHead::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "BigHead: ON" : "BigHead: OFF");
                if (boolean && ctx != nullptr) {
                    Toast(env, ctx, OBFUSCATE("Big Head: ON (Kepala Membesar)"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 2: { // Fast FireRate
                FastFireRate::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "FastFireRate: ON" : "FastFireRate: OFF");
                if (boolean && ctx != nullptr) {
                    Toast(env, ctx, OBFUSCATE("Fast FireRate: ON (Client-Side)"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 3: { // No Recoil
                NoRecoil::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "NoRecoil: ON" : "NoRecoil: OFF");
                if (boolean && ctx != nullptr) {
                    Toast(env, ctx, OBFUSCATE("No Recoil: ON (Client-Side)"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 4: { // Unlock 60 FPS
                FPSUnlock::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "FPSUnlock: ON (60 FPS)" : "FPSUnlock: OFF (30 FPS)");
                if (boolean && ctx != nullptr) {
                    Toast(env, ctx, OBFUSCATE("Unlock 60 FPS: ON (Ultra Halus)"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 5: { // ESP Master
                ESP::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "ESP: ON" : "ESP: OFF");
                if (boolean && ctx != nullptr) {
                    Toast(env, ctx, OBFUSCATE("ESP Master: ON"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 6: { // ESP Enemy Only (Filter Tim)
                ESP::SetEnemyOnly(boolean);
                ModLogger::SetLastAction(boolean ? "ESP EnemyOnly: ON" : "ESP EnemyOnly: OFF");
                if (ctx != nullptr) {
                    Toast(env, ctx, boolean ? OBFUSCATE("ESP: Hanya Musuh (Tim Difilter)") : OBFUSCATE("ESP: Tampilkan Semua"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 7: { // ESP Line Tracer
                ESP::SetLineEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "ESP Line: ON" : "ESP Line: OFF");
                break;
            }

            case 8: { // ESP Health Bar & Info
                ESP::SetHealthEnabled(boolean);
                ESP::SetDistanceEnabled(boolean);
                ESP::SetNameEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "ESP Info: ON" : "ESP Info: OFF");
                break;
            }

            case 9: { // Chams Master (Tembus Pandang / Wallhack)
                Chams::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "Chams: ON" : "Chams: OFF");
                if (boolean && ctx != nullptr) {
                    Toast(env, ctx, OBFUSCATE("Chams: ON (Tembus Pandang)"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            case 10: { // Chams Enemy Only (Filter Tim)
                Chams::SetEnemyOnly(boolean);
                ModLogger::SetLastAction(boolean ? "Chams EnemyOnly: ON" : "Chams EnemyOnly: OFF");
                break;
            }

            case 11: { // Mode Chams
                Chams::SetMode(value);
                break;
            }

            case 12: { // Warna Merah
                Chams::SetR(value);
                break;
            }

            case 13: { // Warna Hijau
                Chams::SetG(value);
                break;
            }

            case 14: { // Warna Biru
                Chams::SetB(value);
                break;
            }

            case 15: { // Tebal Garis
                Chams::SetLineWidth(value > 0 ? value : 1);
                break;
            }

            case 16: { // Reset Reward Time (Free Reward Cooldown Bypass: 1h 57m -> 0s)
                ResetRewardTime::SetEnabled(boolean);
                ModLogger::SetLastAction(boolean ? "ResetRewardTime: ON" : "ResetRewardTime: OFF");
                if (ctx != nullptr) {
                    Toast(env, ctx, boolean ? OBFUSCATE("Reset Reward Time: ON (Cooldown 1h 57m Direset)") : OBFUSCATE("Reset Reward Time: OFF"), ToastLength::LENGTH_SHORT);
                }
                break;
            }

            default:
                LOG_WARN("[TOGGLE] Unknown feature ID: %d", featNum);
                break;
        }
    }

} // namespace FeatureManager
