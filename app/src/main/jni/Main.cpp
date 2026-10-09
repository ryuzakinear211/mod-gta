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
#include <cmath>
#include <dlfcn.h>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <set>
#include <chrono>
#include <cctype>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include "hook.h"
#include "KittyMemory/MemoryPatch.h"
#include "Menu/Setup.h"
#include "espmanager.h"

// =========================================================================
// Advanced Debug Logger & Crash Handler
// Target Log Paths: /storage/0/emulated/Document/mod_gta_debug.log, etc.
// =========================================================================

static const char *const DEBUG_LOG_PATHS[] = {
    "/storage/0/emulated/Document/mod_gta_debug.log",
    "/storage/0/emulated/Document/crash.log",
    "/storage/emulated/0/Documents/mod_gta_debug.log",
    "/storage/emulated/0/Document/mod_gta_debug.log",
    "/sdcard/Documents/mod_gta_debug.log",
    "/sdcard/mod_gta_debug.log",
    "/data/data/com.gamedevltd.wwh/files/mod_gta_debug.log"
};
static const size_t NUM_LOG_PATHS = sizeof(DEBUG_LOG_PATHS) / sizeof(DEBUG_LOG_PATHS[0]);

static std::mutex g_logMutex;
static char g_lastAction[256] = "Initialized";

void setLastAction(const char *action) {
    if (action != nullptr) {
        strncpy(g_lastAction, action, sizeof(g_lastAction) - 1);
        g_lastAction[sizeof(g_lastAction) - 1] = '\0';
    }
}

static void ensureDirectoryForPath(const char *filePath) {
    char dir[512];
    strncpy(dir, filePath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash != nullptr) {
        *slash = '\0';
        for (char *p = dir + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(dir, 0777);
                *p = '/';
            }
        }
        mkdir(dir, 0777);
    }
}

static void preCreateLogDirectories() {
    for (size_t i = 0; i < NUM_LOG_PATHS; i++) {
        ensureDirectoryForPath(DEBUG_LOG_PATHS[i]);
    }
}

static void rawWriteToAllLogs(const char *buffer, size_t len) {
    // 1. Android Logcat
    __android_log_write(ANDROID_LOG_INFO, "Mod_GTA_Debug", buffer);

    // 2. Write to each candidate path directly
    for (size_t i = 0; i < NUM_LOG_PATHS; i++) {
        const char *path = DEBUG_LOG_PATHS[i];
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buffer, len);
            fsync(fd);
            close(fd);
        }
    }
}

void ModLog(const char *fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    char timeStr[64];
    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo = localtime(&rawtime);
    if (timeinfo) {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
    } else {
        snprintf(timeStr, sizeof(timeStr), "UnknownTime");
    }

    char msgBuf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    char fullLine[2560];
    int lineLen = snprintf(fullLine, sizeof(fullLine), "[%s] %s\n", timeStr, msgBuf);
    if (lineLen > 0) {
        rawWriteToAllLogs(fullLine, static_cast<size_t>(lineLen));
    }
}

// =========================================================================
// Native Crash Handler (Signal Interceptor with Tombstone Backtrace)
// =========================================================================

struct BacktraceState {
    void **current;
    void **end;
};

static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context *context, void *arg) {
    BacktraceState *state = static_cast<BacktraceState *>(arg);
    uintptr_t ip = _Unwind_GetIP(context);
    if (ip != 0) {
        if (state->current < state->end) {
            *state->current++ = reinterpret_cast<void *>(ip);
        } else {
            return _URC_END_OF_STACK;
        }
    }
    return _URC_NO_REASON;
}

static size_t captureBacktrace(void **buffer, size_t max) {
    BacktraceState state = { buffer, buffer + max };
    _Unwind_Backtrace(unwindCallback, &state);
    return state.current - buffer;
}

static struct sigaction old_sa_segv;
static struct sigaction old_sa_abrt;
static struct sigaction old_sa_bus;
static struct sigaction old_sa_fpe;
static struct sigaction old_sa_ill;
static struct sigaction old_sa_trap;

// Thread-safe atomic toggle states (Solves in-game toggle visibility bug)
static std::atomic<bool> g_showStatusPanel{false};
static std::atomic<bool> g_autoCount{false};
static std::atomic<bool> g_bigHead{false};
static std::atomic<bool> g_fastFireRate{false};
static std::atomic<bool> g_noRecoil{false};
static std::atomic<bool> g_aimAssistBoost{false};
static std::atomic<int>  g_aimSmoothness{80}; // 0 - 100 slider (default: 80, 100 = sticky lock)
#define g_aimSensitivity g_aimSmoothness

static void crashSignalHandler(int sig, siginfo_t *info, void *ucontext) {
    char crashBuf[8192];
    size_t off = 0;

    const char *sigName = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: sigName = "SIGSEGV (Segmentation Fault - Invalid Memory Access)"; break;
        case SIGABRT: sigName = "SIGABRT (Abort)"; break;
        case SIGBUS:  sigName = "SIGBUS (Bus Error - Alignment or Bad Phys Address)"; break;
        case SIGFPE:  sigName = "SIGFPE (Floating Point Exception)"; break;
        case SIGILL:  sigName = "SIGILL (Illegal Instruction)"; break;
        case SIGTRAP: sigName = "SIGTRAP (Trace/Breakpoint Trap)"; break;
    }

    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo = localtime(&rawtime);
    char timeStr[64];
    if (timeinfo) {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
    } else {
        snprintf(timeStr, sizeof(timeStr), "UnknownTime");
    }

    off += snprintf(crashBuf + off, sizeof(crashBuf) - off,
        "\n"
        "=================================================================\n"
        "                  [CRASH REPORT] GTA SA FPS MOD                  \n"
        "=================================================================\n"
        "Time of Crash : %s\n"
        "Signal        : %d (%s)\n"
        "Signal Code   : %d\n"
        "Fault Address : %p\n"
        "Thread TID    : %d\n"
        "Last Action   : %s\n"
        "Toggle States : StatusPanel=%d, AutoCount=%d, BigHead=%d, FastFireRate=%d, NoRecoil=%d, AimAssistBoost=%d, AimSmooth=%d\n"
        "-----------------------------------------------------------------\n",
        timeStr, sig, sigName, info->si_code, info->si_addr, gettid(),
        g_lastAction,
        static_cast<int>(g_showStatusPanel.load()),
        static_cast<int>(g_autoCount.load()),
        static_cast<int>(g_bigHead.load()),
        static_cast<int>(g_fastFireRate.load()),
        static_cast<int>(g_noRecoil.load()),
        static_cast<int>(g_aimAssistBoost.load()),
        g_aimSmoothness.load()
    );

#if defined(__aarch64__)
    ucontext_t *uc = static_cast<ucontext_t *>(ucontext);
    uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
    uintptr_t lr = static_cast<uintptr_t>(uc->uc_mcontext.regs[30]);
    uintptr_t sp = static_cast<uintptr_t>(uc->uc_mcontext.sp);

    Dl_info pcInfo, lrInfo;
    const char *pcLib = (dladdr(reinterpret_cast<void*>(pc), &pcInfo) && pcInfo.dli_fname) ? pcInfo.dli_fname : "unknown";
    uintptr_t pcOff = pcInfo.dli_fbase ? (pc - reinterpret_cast<uintptr_t>(pcInfo.dli_fbase)) : 0;

    const char *lrLib = (dladdr(reinterpret_cast<void*>(lr), &lrInfo) && lrInfo.dli_fname) ? lrInfo.dli_fname : "unknown";
    uintptr_t lrOff = lrInfo.dli_fbase ? (lr - reinterpret_cast<uintptr_t>(lrInfo.dli_fbase)) : 0;

    off += snprintf(crashBuf + off, sizeof(crashBuf) - off,
        "PC: 0x%016lx (%s + 0x%lx)\n"
        "LR: 0x%016lx (%s + 0x%lx)\n"
        "SP: 0x%016lx\n\n"
        "ARM64 Registers:\n"
        "  x0:  0x%016llx  x1:  0x%016llx\n"
        "  x2:  0x%016llx  x3:  0x%016llx\n"
        "  x4:  0x%016llx  x5:  0x%016llx\n"
        "  x6:  0x%016llx  x7:  0x%016llx\n"
        "  x8:  0x%016llx  x9:  0x%016llx\n"
        "  x10: 0x%016llx  x11: 0x%016llx\n"
        "  x12: 0x%016llx  x13: 0x%016llx\n"
        "  x14: 0x%016llx  x15: 0x%016llx\n"
        "  x16: 0x%016llx  x17: 0x%016llx\n"
        "  x18: 0x%016llx  x19: 0x%016llx\n"
        "  x20: 0x%016llx  x21: 0x%016llx\n"
        "  x22: 0x%016llx  x23: 0x%016llx\n"
        "  x24: 0x%016llx  x25: 0x%016llx\n"
        "  x26: 0x%016llx  x27: 0x%016llx\n"
        "  x28: 0x%016llx  x29: 0x%016llx\n"
        "  lr:  0x%016llx  sp:  0x%016llx\n"
        "  pc:  0x%016llx\n"
        "-----------------------------------------------------------------\n",
        pc, pcLib, pcOff,
        lr, lrLib, lrOff,
        sp,
        static_cast<unsigned long long>(uc->uc_mcontext.regs[0]), static_cast<unsigned long long>(uc->uc_mcontext.regs[1]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[2]), static_cast<unsigned long long>(uc->uc_mcontext.regs[3]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[4]), static_cast<unsigned long long>(uc->uc_mcontext.regs[5]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[6]), static_cast<unsigned long long>(uc->uc_mcontext.regs[7]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[8]), static_cast<unsigned long long>(uc->uc_mcontext.regs[9]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[10]), static_cast<unsigned long long>(uc->uc_mcontext.regs[11]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[12]), static_cast<unsigned long long>(uc->uc_mcontext.regs[13]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[14]), static_cast<unsigned long long>(uc->uc_mcontext.regs[15]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[16]), static_cast<unsigned long long>(uc->uc_mcontext.regs[17]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[18]), static_cast<unsigned long long>(uc->uc_mcontext.regs[19]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[20]), static_cast<unsigned long long>(uc->uc_mcontext.regs[21]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[22]), static_cast<unsigned long long>(uc->uc_mcontext.regs[23]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[24]), static_cast<unsigned long long>(uc->uc_mcontext.regs[25]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[26]), static_cast<unsigned long long>(uc->uc_mcontext.regs[27]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[28]), static_cast<unsigned long long>(uc->uc_mcontext.regs[29]),
        static_cast<unsigned long long>(uc->uc_mcontext.regs[30]), static_cast<unsigned long long>(uc->uc_mcontext.sp),
        static_cast<unsigned long long>(uc->uc_mcontext.pc)
    );
#endif

    // Backtrace Frames
    off += snprintf(crashBuf + off, sizeof(crashBuf) - off, "Backtrace:\n");
    void *btBuffer[32];
    size_t count = captureBacktrace(btBuffer, 32);
    for (size_t i = 0; i < count; i++) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(btBuffer[i]);
        Dl_info dlinfo;
        if (dladdr(reinterpret_cast<void*>(addr), &dlinfo) && dlinfo.dli_fname) {
            uintptr_t libOffset = dlinfo.dli_fbase ? (addr - reinterpret_cast<uintptr_t>(dlinfo.dli_fbase)) : 0;
            const char *sym = dlinfo.dli_sname ? dlinfo.dli_sname : "";
            off += snprintf(crashBuf + off, sizeof(crashBuf) - off,
                "  #%02zu pc 0x%016lx  %s (%s+0x%lx)\n",
                i, addr, dlinfo.dli_fname, sym, libOffset);
        } else {
            off += snprintf(crashBuf + off, sizeof(crashBuf) - off,
                "  #%02zu pc 0x%016lx\n", i, addr);
        }
        if (off >= sizeof(crashBuf) - 256) break;
    }

    off += snprintf(crashBuf + off, sizeof(crashBuf) - off,
        "=================================================================\n\n");

    // Write crash dump to all log files immediately
    rawWriteToAllLogs(crashBuf, off);

    // Call default handler or previous handler
    struct sigaction *oldSa = nullptr;
    switch (sig) {
        case SIGSEGV: oldSa = &old_sa_segv; break;
        case SIGABRT: oldSa = &old_sa_abrt; break;
        case SIGBUS:  oldSa = &old_sa_bus; break;
        case SIGFPE:  oldSa = &old_sa_fpe; break;
        case SIGILL:  oldSa = &old_sa_ill; break;
        case SIGTRAP: oldSa = &old_sa_trap; break;
    }

    if (oldSa && oldSa->sa_sigaction && oldSa->sa_sigaction != crashSignalHandler) {
        oldSa->sa_sigaction(sig, info, ucontext);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void installCrashHandler() {
    preCreateLogDirectories();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

    sigaction(SIGSEGV, &sa, &old_sa_segv);
    sigaction(SIGABRT, &sa, &old_sa_abrt);
    sigaction(SIGBUS,  &sa, &old_sa_bus);
    sigaction(SIGFPE,  &sa, &old_sa_fpe);
    sigaction(SIGILL,  &sa, &old_sa_ill);
    sigaction(SIGTRAP, &sa, &old_sa_trap);

    ModLog("[CRASH_HANDLER] Native Crash Handler installed successfully for SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP.");
}

// =========================================================================
// Fast FireRate System (Dynamic Player Weapon Memory & Client-Side Hooks)
// =========================================================================

static uint64_t getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// Dynamic Player Weapon Memory Addresses (Atomic for cross-thread safety)
static std::atomic<void*> g_localPlayerFPC{nullptr};
static std::atomic<void*> g_localPlayerTargetibleObject{nullptr};
std::atomic<void*> g_localPlayerNetPlayer{nullptr};
std::atomic<void*> g_activeAimingCamera{nullptr};
static std::atomic<bool>  g_espLine{false};
static std::atomic<bool>  g_espBox{false};
static std::atomic<void*> g_localPlayerWeapon{nullptr};
static std::atomic<void*> g_localPlayerShooter{nullptr};
static std::atomic<void*> g_localWeaponProfile{nullptr};
static std::atomic<void*> g_localWeaponParams{nullptr};
static std::atomic<void*> g_activeAimingControl{nullptr};
static std::atomic<void*> g_activeAimAssistManager{nullptr};
static std::atomic<void*> g_activeNewAutoAim{nullptr};
static std::atomic<void*> g_activeBaseAimAssist{nullptr};

static uint64_t g_lastWeaponLogMs = 0;
static int g_origShootAction = -1;
static bool g_hasOrigShootAction = false;

static bool isClientWeapon(void *instance) {
    if (instance == nullptr) return false;
    void *curShooter = g_localPlayerShooter.load();
    void *curWeapon = g_localPlayerWeapon.load();
    if (instance == curShooter || instance == curWeapon) return true;

    void *fpc = g_localPlayerFPC.load();
    if (fpc != nullptr && isPointerReadable(fpc)) {
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(fpc) + 0x50))) {
            void *equipped = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(fpc) + 0x50);
            if (equipped == instance) return true;
        }
    }
    return false;
}

static bool isClientWeaponProfile(void *instance) {
    if (instance == nullptr) return false;
    return (instance == g_localWeaponProfile.load());
}

static bool isClientWeaponParams(void *instance) {
    if (instance == nullptr) return false;
    return (instance == g_localWeaponParams.load());
}

static void updateLocalPlayerWeapon(void *fpc) {
    if (fpc == nullptr || !isPointerReadable(fpc)) return;
    g_localPlayerFPC.store(fpc);

    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(fpc) + 0x120))) {
        void *tObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(fpc) + 0x120);
        if (tObj != nullptr && isUnityObjectAlive(tObj)) {
            g_localPlayerTargetibleObject.store(tObj);
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tObj) + 0xD8))) {
                void *np = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tObj) + 0xD8);
                if (np != nullptr && isUnityObjectAlive(np)) {
                    g_localPlayerNetPlayer.store(np);
                }
            }
        }
    }

    void *currentWeapon = nullptr;
    if (get_CurrentWeapon != nullptr) {
        currentWeapon = get_CurrentWeapon(fpc);
    }
    if (currentWeapon == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(fpc) + 0x50))) {
        currentWeapon = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(fpc) + 0x50);
    }

    if (currentWeapon != nullptr && isPointerReadable(currentWeapon)) {
        g_localPlayerWeapon.store(currentWeapon);

        void *shooter = nullptr;
        if (get_ShooterBehaviour != nullptr) {
            shooter = get_ShooterBehaviour(fpc, currentWeapon);
        }
        if (shooter != nullptr && isPointerReadable(shooter)) {
            g_localPlayerShooter.store(shooter);
        } else {
            g_localPlayerShooter.store(currentWeapon); // WeaponShooterBehaviour inherits from ItemWeaponBehaviour
        }

        // WeaponProfile at offset 0x80 of ItemBehaviour
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(currentWeapon) + 0x80))) {
            g_localWeaponProfile.store(*reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(currentWeapon) + 0x80));
        }
        // WeaponParameters at offset 0x88 of ItemBehaviour
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(currentWeapon) + 0x88))) {
            g_localWeaponParams.store(*reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(currentWeapon) + 0x88));
        }
    }

    uint64_t now = getCurrentTimeMs();
    void *s = g_localPlayerShooter.load();
    if (now - g_lastWeaponLogMs > 8000 && s != nullptr) {
        g_lastWeaponLogMs = now;
        ModLog("[WEAPON] Client Weapon Memory Found -> FPC: %p | Weapon: %p | Shooter: %p | Profile: %p | Params: %p",
               g_localPlayerFPC.load(), g_localPlayerWeapon.load(), s, g_localWeaponProfile.load(), g_localWeaponParams.load());
    }
}

static void applyPlayerWeaponMemoryEdits() {
    void *shooter = g_localPlayerShooter.load();
    if (shooter == nullptr || !isPointerReadable(shooter)) return;

    // 1. Force Full Auto (_shootAction at offset 0x150: AUTO = 0, BOLT_ACTION = 1)
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(shooter) + 0x150))) {
        int *shootActionPtr = reinterpret_cast<int *>(reinterpret_cast<uintptr_t>(shooter) + 0x150);
        if (!g_hasOrigShootAction) {
            g_origShootAction = *shootActionPtr;
            g_hasOrigShootAction = true;
        }
        *shootActionPtr = 0; // AUTO
    }

    // 2. Zero out internal cooldown ObscuredFloat fields at offset 0x18C & 0x1B0
    static ObscuredFloat s_cachedZeroVal = {0};
    static bool s_hasCachedZero = false;
    if (!s_hasCachedZero && actk_op_Implicit_Float != nullptr) {
        s_cachedZeroVal = actk_op_Implicit_Float(0.0f);
        s_hasCachedZero = true;
    }
    if (s_hasCachedZero) {
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(shooter) + 0x18C))) {
            memcpy(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(shooter) + 0x18C), &s_cachedZeroVal, sizeof(ObscuredFloat));
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(shooter) + 0x1B0))) {
            memcpy(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(shooter) + 0x1B0), &s_cachedZeroVal, sizeof(ObscuredFloat));
        }
    }

    // 3. Keep clip ammo filled during high-speed firing
    if (set_ClipAmmo != nullptr) {
        set_ClipAmmo(shooter, 999);
    }
}

static void applyNoRecoilMemoryEdits(void *fpc) {
    if (fpc == nullptr || !isPointerReadable(fpc)) return;

    // 1. AnimController at offset 0xB8
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(fpc) + 0xB8))) {
        void *animCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(fpc) + 0xB8);
        if (animCtrl != nullptr && isPointerReadable(animCtrl)) {
            // AnimEffectList at offset 0x30
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(animCtrl) + 0x30))) {
                void *animList = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(animCtrl) + 0x30);
                if (animList != nullptr && isPointerReadable(animList)) {
                    // AttackAnimEffect at offset 0x38
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(animList) + 0x38))) {
                        void *attackAnim = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(animList) + 0x38);
                        if (attackAnim != nullptr && isPointerReadable(attackAnim)) {
                            // Zero current weapon/camera recoil vectors
                            // 0x1F0: _currentWeaponRecoilPosition (Vector3)
                            // 0x1FC: _currentWeaponRecoilRotation (Vector3)
                            // 0x208: _currentCameraRecoilRotation (Vector3)
                            // 0x214: _weaponRotationOutput (Vector3)
                            // 0x220: _cameraRotationOutput (Vector3)
                            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(attackAnim) + 0x1F0))) {
                                memset(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(attackAnim) + 0x1F0), 0, 0x22C - 0x1F0);
                            }
                            // 0x244: _sumShift (Vector2)
                            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(attackAnim) + 0x244))) {
                                *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(attackAnim) + 0x244) = 0.0f;
                                *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(attackAnim) + 0x248) = 0.0f;
                            }
                        }
                    }
                }
            }
        }
    }
}

// 1. FirstPersonController.Update: RVA 0x40A1BF4
void (*old_FirstPersonController_Update)(void *instance) = nullptr;
void hook_FirstPersonController_Update(void *instance) {
    if (instance != nullptr && isUnityObjectAlive(instance)) {
        setLastAction("FirstPersonController_Update");
        updateLocalPlayerWeapon(instance);
        if (g_fastFireRate.load()) {
            applyPlayerWeaponMemoryEdits();
        } else if (g_hasOrigShootAction) {
            void *shooter = g_localPlayerShooter.load();
            if (shooter != nullptr && isPointerReadable(shooter)) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(shooter) + 0x150))) {
                    *reinterpret_cast<int *>(reinterpret_cast<uintptr_t>(shooter) + 0x150) = g_origShootAction;
                    g_hasOrigShootAction = false;
                }
            }
        }
        if (g_noRecoil.load()) {
            applyNoRecoilMemoryEdits(instance);
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0xF0))) {
            void *aimingControl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(instance) + 0xF0);
            if (aimingControl != nullptr && isPointerReadable(aimingControl)) {
                g_activeAimingControl.store(aimingControl);
            }
        }
    }
    if (old_FirstPersonController_Update != nullptr) {
        old_FirstPersonController_Update(instance);
    }
}

// 2. WeaponShooterBehaviour.CanShoot: RVA 0x405BBF0
bool (*old_WeaponShooterBehaviour_CanShoot)(void *instance) = nullptr;
bool hook_WeaponShooterBehaviour_CanShoot(void *instance) {
    if (g_fastFireRate.load() && isClientWeapon(instance)) {
        return true; // Local weapon can always shoot instantly
    }
    if (old_WeaponShooterBehaviour_CanShoot != nullptr) {
        return old_WeaponShooterBehaviour_CanShoot(instance);
    }
    return true;
}

// 3. WeaponShooterBehaviour.SetCooldown: RVA 0x405D084
void (*old_WeaponShooterBehaviour_SetCooldown)(void *instance) = nullptr;
void hook_WeaponShooterBehaviour_SetCooldown(void *instance) {
    if (g_fastFireRate.load() && isClientWeapon(instance)) {
        return; // Zero cooldown for local weapon
    }
    if (old_WeaponShooterBehaviour_SetCooldown != nullptr) {
        old_WeaponShooterBehaviour_SetCooldown(instance);
    }
}

// 5. FirstPersonController.GetShotInterval1: RVA 0x40906CC
ObscuredFloat (*old_FPC_GetShotInterval1)(void *fpc, void *weapon) = nullptr;
ObscuredFloat hook_FPC_GetShotInterval1(void *fpc, void *weapon) {
    if (g_fastFireRate.load() && (fpc == g_localPlayerFPC.load() || isClientWeapon(weapon))) {
        if (actk_op_Implicit_Float != nullptr) {
            return actk_op_Implicit_Float(0.001f); // 0.001s shot interval
        }
    }
    if (old_FPC_GetShotInterval1 != nullptr) {
        return old_FPC_GetShotInterval1(fpc, weapon);
    }
    if (actk_op_Implicit_Float != nullptr) {
        return actk_op_Implicit_Float(0.1f);
    }
    ObscuredFloat defVal = {0};
    return defVal;
}

// 6. FirstPersonController.GetShotInterval2: RVA 0x409F2F8
ObscuredFloat (*old_FPC_GetShotInterval2)(void *fpc, void *weapon) = nullptr;
ObscuredFloat hook_FPC_GetShotInterval2(void *fpc, void *weapon) {
    if (g_fastFireRate.load() && (fpc == g_localPlayerFPC.load() || isClientWeapon(weapon))) {
        if (actk_op_Implicit_Float != nullptr) {
            return actk_op_Implicit_Float(0.001f);
        }
    }
    if (old_FPC_GetShotInterval2 != nullptr) {
        return old_FPC_GetShotInterval2(fpc, weapon);
    }
    if (actk_op_Implicit_Float != nullptr) {
        return actk_op_Implicit_Float(0.1f);
    }
    ObscuredFloat defVal = {0};
    return defVal;
}

// 7. ShootCoroutine.MoveNext: RVA 0x40B93A0
bool (*old_ShootCoroutine_MoveNext)(void *instance) = nullptr;
bool hook_ShootCoroutine_MoveNext(void *instance) {
    if (g_fastFireRate.load() && instance != nullptr && isPointerReadable(instance)) {
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0x20))) {
            void *fpc = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(instance) + 0x20);
            if (fpc == g_localPlayerFPC.load() && fpc != nullptr) {
                // Immediate fire without WaitForSeconds delay
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0x28))) {
                    *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(instance) + 0x28) = true;
                }
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0x2C))) {
                    *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(instance) + 0x2C) = 100.0f;
                }
            }
        }
    }
    if (old_ShootCoroutine_MoveNext != nullptr) {
        return old_ShootCoroutine_MoveNext(instance);
    }
    return false;
}

// 8. WeaponParameters.GetFireRate: RVA 0x49D0E90
float (*old_WeaponParams_GetFireRate)(void *instance) = nullptr;
float hook_WeaponParams_GetFireRate(void *instance) {
    if (g_fastFireRate.load() && isClientWeaponParams(instance)) {
        return 3000.0f; // High speed firerate (Client only)
    }
    if (old_WeaponParams_GetFireRate != nullptr) {
        return old_WeaponParams_GetFireRate(instance);
    }
    return 600.0f;
}

// 9. WeaponProfile.GetFireRate: RVA 0x4A451C0
float (*old_WeaponProfile_GetFireRate)(void *instance) = nullptr;
float hook_WeaponProfile_GetFireRate(void *instance) {
    if (g_fastFireRate.load() && isClientWeaponProfile(instance)) {
        return 3000.0f; // High speed firerate (Client only)
    }
    if (old_WeaponProfile_GetFireRate != nullptr) {
        return old_WeaponProfile_GetFireRate(instance);
    }
    return 600.0f;
}

// =========================================================================
// Client-Side No Recoil Hooks (100% Recoil & Aim Kick Elimination)
// =========================================================================

// 1. FirstPersonController.RecoilShift: RVA 0x40A4420
void (*old_FPC_Recoil)(void *fpc, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
void hook_FPC_Recoil(void *fpc, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
    if (g_noRecoil.load() && (fpc == g_localPlayerFPC.load() || fpc != nullptr)) {
        Vector2 zeroRecoil(0.0f, 0.0f);
        if (old_FPC_Recoil != nullptr) {
            old_FPC_Recoil(fpc, zeroRecoil, smooth, returnSpeed, resistance);
        }
        return;
    }
    if (old_FPC_Recoil != nullptr) {
        old_FPC_Recoil(fpc, recoil, smooth, returnSpeed, resistance);
    }
}

// 2. AimingControl.RecoilShift1: RVA 0x4D27AB8
void (*old_AimingControl_Recoil1)(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
void hook_AimingControl_Recoil1(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
    if (g_noRecoil.load()) {
        Vector2 zeroRecoil(0.0f, 0.0f);
        if (old_AimingControl_Recoil1 != nullptr) {
            old_AimingControl_Recoil1(instance, zeroRecoil, smooth, returnSpeed, resistance);
        }
        return;
    }
    if (old_AimingControl_Recoil1 != nullptr) {
        old_AimingControl_Recoil1(instance, recoil, smooth, returnSpeed, resistance);
    }
}

// 3. AimingControl.RecoilShift2: RVA 0x4D27E20
void (*old_AimingControl_Recoil2)(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
void hook_AimingControl_Recoil2(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
    if (g_noRecoil.load()) {
        Vector2 zeroRecoil(0.0f, 0.0f);
        if (old_AimingControl_Recoil2 != nullptr) {
            old_AimingControl_Recoil2(instance, zeroRecoil, smooth, returnSpeed, resistance);
        }
        return;
    }
    if (old_AimingControl_Recoil2 != nullptr) {
        old_AimingControl_Recoil2(instance, recoil, smooth, returnSpeed, resistance);
    }
}

// 4. AimingControl.RecoilShift3: RVA 0x4D284B0
void (*old_AimingControl_Recoil3)(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
void hook_AimingControl_Recoil3(void *instance, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
    if (g_noRecoil.load()) {
        Vector2 zeroRecoil(0.0f, 0.0f);
        if (old_AimingControl_Recoil3 != nullptr) {
            old_AimingControl_Recoil3(instance, zeroRecoil, smooth, returnSpeed, resistance);
        }
        return;
    }
    if (old_AimingControl_Recoil3 != nullptr) {
        old_AimingControl_Recoil3(instance, recoil, smooth, returnSpeed, resistance);
    }
}

// =========================================================================
// Real-time Player & Bot Tracking System
// =========================================================================

static std::mutex g_entityMutex;
static std::unordered_map<void*, uint64_t> g_networkPlayers; // NetworkPlayer* -> last seen ms
static std::unordered_map<void*, uint64_t> g_botPlayers;     // BotPlayer* -> last seen ms
static std::unordered_map<void*, void*> g_botNetPlayers;     // BotPlayer* -> NetworkPlayer*

static std::atomic<bool> g_needsBigHeadReset{false};
static std::atomic<int> g_bigHeadResetFrames{0};

static const Vector3 BIG_HEAD_SCALE(3.0f, 3.0f, 3.0f);
static const Vector3 NORMAL_HEAD_SCALE(1.0f, 1.0f, 1.0f);

static void onNetworkPlayerUpdate(void *instance) {
    if (instance == nullptr || !isUnityObjectAlive(instance)) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_networkPlayers[instance] = getCurrentTimeMs();

    // Dynamically detect local player if TargetType == 1 (LocalPlayer)
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0xC0))) {
        void *targetInfo = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(instance) + 0xC0);
        if (targetInfo != nullptr && isPointerReadable(targetInfo)) {
            int tType = 0;
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30))) {
                tType = *reinterpret_cast<int *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x30);
            }
            if (tType == 0 && get_TargetType != nullptr) {
                tType = get_TargetType(targetInfo);
            }
            if (tType == 1) { // LocalPlayer
                g_localPlayerNetPlayer.store(instance);
            }
        }
    }
}

static void onNetworkPlayerDestroy(void *instance) {
    if (instance == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_networkPlayers.erase(instance);
    if (instance == g_localPlayerNetPlayer.load()) {
        g_localPlayerNetPlayer.store(nullptr);
    }
}

static void onBotPlayerUpdate(void *instance) {
    if (instance == nullptr || !isUnityObjectAlive(instance)) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_botPlayers[instance] = getCurrentTimeMs();

    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0x50))) {
        void *netPlayer = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(instance) + 0x50);
        if (netPlayer != nullptr && isUnityObjectAlive(netPlayer)) {
            g_botNetPlayers[instance] = netPlayer;
        }
    }
}

static void onBotPlayerDestroy(void *instance) {
    if (instance == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_botPlayers.erase(instance);
    g_botNetPlayers.erase(instance);
}

static EntityStats getEntityStats() {
    std::lock_guard<std::mutex> lock(g_entityMutex);
    uint64_t now = getCurrentTimeMs();
    const uint64_t TIMEOUT_MS = 2500;

    std::set<void*> activeBotNets;
    for (auto it = g_botPlayers.begin(); it != g_botPlayers.end(); ) {
        if (now - it->second > TIMEOUT_MS || !isUnityObjectAlive(it->first)) {
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

    int totalNetPlayers = 0;
    int realCount = 0;
    for (auto it = g_networkPlayers.begin(); it != g_networkPlayers.end(); ) {
        if (now - it->second > TIMEOUT_MS || !isUnityObjectAlive(it->first)) {
            it = g_networkPlayers.erase(it);
        } else {
            totalNetPlayers++;
            if (activeBotNets.find(it->first) == activeBotNets.end()) {
                realCount++;
            }
            ++it;
        }
    }

    int botCount = static_cast<int>(g_botPlayers.size());
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
    g_localPlayerTargetibleObject.store(nullptr);
    g_localPlayerNetPlayer.store(nullptr);
    ModLog("[STATS] Entity counters reset manually.");
}

// =========================================================================
// Safe Client-Side Big Head Logic
// =========================================================================

static uint64_t g_lastBigHeadLogMs = 0;

static void applyBigHeadToNetworkPlayer(void *netPlayer, const Vector3 &scale) {
    if (netPlayer == nullptr || !isUnityObjectAlive(netPlayer)) return;

    // 1. DollsManager at offset 0x88
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(netPlayer) + 0x88))) {
        void *dollsMgr = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(netPlayer) + 0x88);
        if (dollsMgr != nullptr && isUnityObjectAlive(dollsMgr)) {
            // Remote player: ThirdPersonController at 0x50
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x50))) {
                void *tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x50);
                if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78))) {
                        void *animator = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78);
                        if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                            void *headBone = GetBoneTransform(animator, 10); // 10 = HumanBodyBones.Head
                            if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                                safeSetLocalScale(headBone, scale);
                            }
                        }
                    }
                }
            }
            // Bot player doll: ThirdPersonController at 0x60
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x60))) {
                void *tpBotCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(dollsMgr) + 0x60);
                if (tpBotCtrl != nullptr && isUnityObjectAlive(tpBotCtrl)) {
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tpBotCtrl) + 0x78))) {
                        void *animator = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tpBotCtrl) + 0x78);
                        if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                            void *headBone = GetBoneTransform(animator, 10);
                            if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                                safeSetLocalScale(headBone, scale);
                            }
                        }
                    }
                }
            }
        }
    }

    // 2. Head Hitbox in BodyPointsManager at offset 0xC8
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC8))) {
        void *bpm = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC8);
        if (bpm != nullptr && isUnityObjectAlive(bpm)) {
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bpm) + 0x20))) {
                void *bodyPointsArr = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(bpm) + 0x20);
                if (bodyPointsArr != nullptr && isPointerReadable(bodyPointsArr)) {
                    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bodyPointsArr) + 0x18))) {
                        uintptr_t length = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(bodyPointsArr) + 0x18);
                        if (length > 0 && length < 32) {
                            void **items = reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(bodyPointsArr) + 0x20);
                            for (uintptr_t i = 0; i < length; i++) {
                                if (isPointerReadable(&items[i])) {
                                    void *bodyPoint = items[i];
                                    if (bodyPoint != nullptr && isPointerReadable(bodyPoint)) {
                                        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bodyPoint) + 0x10))) {
                                            void *bpView = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(bodyPoint) + 0x10);
                                            if (bpView != nullptr && isUnityObjectAlive(bpView)) {
                                                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bpView) + 0x20))) {
                                                    int pointType = *reinterpret_cast<int *>(reinterpret_cast<uintptr_t>(bpView) + 0x20);
                                                    if (pointType == 0) { // Head
                                                        if (get_transform != nullptr) {
                                                            void *headHitbox = get_transform(bpView);
                                                            if (headHitbox != nullptr && isUnityObjectAlive(headHitbox)) {
                                                                safeSetLocalScale(headHitbox, scale);
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
            }
        }
    }

    uint64_t now = getCurrentTimeMs();
    if (scale.x > 1.5f && now - g_lastBigHeadLogMs > 10000) {
        g_lastBigHeadLogMs = now;
        ModLog("[BIG_HEAD] Active: Bone scaling applied to player entities (scale=%.1f)", scale.x);
    }
}

static void applyBigHeadToBotPlayer(void *botPlayer, const Vector3 &scale) {
    if (botPlayer == nullptr || !isUnityObjectAlive(botPlayer)) return;

    // 0. ThirdPersonController at offset 0x58
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x58))) {
        void *tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x58);
        if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78))) {
                void *animator = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(tpCtrl) + 0x78);
                if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                    void *headBone = GetBoneTransform(animator, 10);
                    if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                        safeSetLocalScale(headBone, scale);
                    }
                }
            }
        }
    }

    // 1. BotPlayerLook at offset 0x28 -> Transform at offset 0x28 (Head look bone)
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x28))) {
        void *botLook = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x28);
        if (botLook != nullptr && isUnityObjectAlive(botLook)) {
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botLook) + 0x28))) {
                void *headBone = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botLook) + 0x28);
                if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                    safeSetLocalScale(headBone, scale);
                }
            }
        }
    }

    // 2. NetworkPlayer at offset 0x50
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x50))) {
        void *netPlayer = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x50);
        if (netPlayer != nullptr && isUnityObjectAlive(netPlayer)) {
            applyBigHeadToNetworkPlayer(netPlayer, scale);
        }
    }
}

// =========================================================================
// Advanced Bone-Lock Aim Assist System
// =========================================================================

bool isNetworkPlayerTeammate(void *netPlayer) {
    if (netPlayer == nullptr || !isPointerReadable(netPlayer)) return true;
    if (!isUnityObjectAlive(netPlayer)) return true; // Destroyed -> ignore

    // Ignore self
    void *localNet = g_localPlayerNetPlayer.load();
    if (localNet != nullptr && netPlayer == localNet) {
        return true;
    }

    // 1. Check targetInfo at offset 0xC0
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

            if (tType != 0) {
                if (tType == 1) { // LocalPlayer
                    g_localPlayerNetPlayer.store(netPlayer);
                    return true;
                }
                // Bitmask: LocalPlayer (1) | OtherPlayerAlly (2) | VehicleAlly (8) | BotAlly (32) = 43
                if ((tType & (1 | 2 | 8 | 32)) != 0) {
                    return true; // Marked as ally by game engine!
                }
                // Bitmask: OtherPlayerEnemy (4) | VehicleEnemy (16) | BotEnemy (64) | BotDeathmatch (128) = 212
                if ((tType & (4 | 16 | 64 | 128)) != 0) {
                    return false; // Confirmed enemy!
                }
            }
        }
    }

    // 2. Check NetworkPlayer.IsTeammate if local player is known
    if (localNet != nullptr && NetworkPlayer_IsTeammate != nullptr && isUnityObjectAlive(localNet)) {
        if (NetworkPlayer_IsTeammate(netPlayer, localNet)) {
            return true;
        }
    }

    return false;
}

static bool isTargetibleObjectTeammate(void *targetibleObj) {
    if (targetibleObj == nullptr || !isPointerReadable(targetibleObj)) return true;
    if (!isUnityObjectAlive(targetibleObj)) return true;

    // Ignore self if targetibleObject belongs to local player
    void *localTObj = g_localPlayerTargetibleObject.load();
    if (localTObj != nullptr && targetibleObj == localTObj) {
        return true;
    }

    // 1. Check associated NetworkPlayer at offset 0xD8
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0xD8))) {
        void *targetNetPlayer = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0xD8);
        if (targetNetPlayer != nullptr && isPointerReadable(targetNetPlayer) && isUnityObjectAlive(targetNetPlayer)) {
            if (isNetworkPlayerTeammate(targetNetPlayer)) {
                return true;
            }
        }
    }

    // 2. Check TargetibleObjectCustomSettings at offset 0x90
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90))) {
        void *customSettings = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90);
        if (customSettings != nullptr && isPointerReadable(customSettings) && isUnityObjectAlive(customSettings)) {
            // A. Call get_AllyObjectToogle (RVA 0x4DED164)
            if (get_AllyObjectToogle != nullptr && get_AllyObjectToogle(customSettings)) {
                return true;
            }
            // B. Direct check of <AllyObjectToogle>k__BackingField at offset 0x20
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(customSettings) + 0x20))) {
                if (*reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(customSettings) + 0x20)) {
                    return true;
                }
            }
            // C. Call get_IsAutoAimAllowed (RVA 0x4DED124)
            if (get_IsAutoAimAllowed != nullptr && !get_IsAutoAimAllowed(customSettings)) {
                return true;
            }
            // D. Direct check on _allyData at offset 0x30
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(customSettings) + 0x30))) {
                void *allyData = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(customSettings) + 0x30);
                if (allyData != nullptr && isPointerReadable(allyData) &&
                    isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(allyData) + 0x10))) {
                    if (*reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(allyData) + 0x10)) {
                        return true;
                    }
                }
            }
            // E. Direct check on _enemyData at offset 0x28 -> IsAutoAimAllowed (0x11)
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(customSettings) + 0x28))) {
                void *enemyData = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(customSettings) + 0x28);
                if (enemyData != nullptr && isPointerReadable(enemyData) &&
                    isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(enemyData) + 0x11))) {
                    bool autoAimAllowed = *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(enemyData) + 0x11);
                    if (!autoAimAllowed) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool isEntityDeadOrCorpse(void *netPlayer, void *botPlayer, void *targetibleObj) {
    // 1. If TargetibleObject is present, verify active state and enabled combat colliders
    if (targetibleObj != nullptr && isPointerReadable(targetibleObj) && isUnityObjectAlive(targetibleObj)) {
        if (Behaviour_get_isActiveAndEnabled != nullptr && !Behaviour_get_isActiveAndEnabled(targetibleObj)) {
            return true;
        }

        if (Collider_get_enabled != nullptr) {
            void *headCol = nullptr;
            void *bodyCol = nullptr;
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40))) {
                headCol = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40);
            }
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x48))) {
                bodyCol = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x48);
            }
            bool headAlive = (headCol != nullptr && isUnityObjectAlive(headCol) && Collider_get_enabled(headCol));
            bool bodyAlive = (bodyCol != nullptr && isUnityObjectAlive(bodyCol) && Collider_get_enabled(bodyCol));
            if (!headAlive && !bodyAlive && (headCol != nullptr || bodyCol != nullptr)) {
                return true;
            }
        }

        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90))) {
            void *customSettings = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x90);
            if (customSettings != nullptr && isPointerReadable(customSettings) && isUnityObjectAlive(customSettings)) {
                if (get_IsAutoAimAllowed != nullptr && !get_IsAutoAimAllowed(customSettings)) {
                    return true;
                }
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(customSettings) + 0x28))) {
                    void *enemyData = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(customSettings) + 0x28);
                    if (enemyData != nullptr && isPointerReadable(enemyData) &&
                        isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(enemyData) + 0x11))) {
                        if (!(*reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(enemyData) + 0x11))) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    // 2. Check BotPlayer health (BotPlayerHealth at 0x30)
    if (botPlayer != nullptr && isPointerReadable(botPlayer) && isUnityObjectAlive(botPlayer)) {
        if (Behaviour_get_isActiveAndEnabled != nullptr && !Behaviour_get_isActiveAndEnabled(botPlayer)) {
            return true;
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x30))) {
            void *botHealth = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x30);
            if (botHealth != nullptr && isPointerReadable(botHealth) && isUnityObjectAlive(botHealth)) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C))) {
                    float hp = *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(botHealth) + 0x2C);
                    if (hp <= 0.0f) {
                        return true;
                    }
                }
                if (BotPlayerHealth_GetHealth != nullptr) {
                    float hp = BotPlayerHealth_GetHealth(botHealth);
                    if (hp <= 0.0f) {
                        return true;
                    }
                }
            }
        }
    }

    // 3. Check NetworkPlayer
    if (netPlayer != nullptr && isPointerReadable(netPlayer) && isUnityObjectAlive(netPlayer)) {
        if (Behaviour_get_isActiveAndEnabled != nullptr && !Behaviour_get_isActiveAndEnabled(netPlayer)) {
            return true;
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC0))) {
            void *targetInfo = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC0);
            if (targetInfo != nullptr && isPointerReadable(targetInfo)) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x34))) {
                    bool isAlive = *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(targetInfo) + 0x34);
                    if (!isAlive) {
                        return true;
                    }
                }
            }
        }
        if (Collider_get_enabled != nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(netPlayer) + 0x90))) {
            void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(netPlayer) + 0x90);
            if (col != nullptr && isUnityObjectAlive(col)) {
                if (!Collider_get_enabled(col)) {
                    return true;
                }
            }
        }
    }

    return false;
}

static bool isEntityEnemy(void *entityNetPlayer, void *targetibleObj, void *botPlayer = nullptr) {
    if (entityNetPlayer != nullptr && isPointerReadable(entityNetPlayer)) {
        if (isNetworkPlayerTeammate(entityNetPlayer)) return false;
    }
    if (targetibleObj != nullptr && isPointerReadable(targetibleObj)) {
        if (isTargetibleObjectTeammate(targetibleObj)) return false;
    }
    if (botPlayer != nullptr && isPointerReadable(botPlayer) && isUnityObjectAlive(botPlayer)) {
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x50))) {
            void *botNet = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x50);
            if (botNet != nullptr && isPointerReadable(botNet) && isUnityObjectAlive(botNet)) {
                if (isNetworkPlayerTeammate(botNet)) return false;
            }
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x60))) {
            void *botTObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x60);
            if (botTObj != nullptr && isPointerReadable(botTObj) && isUnityObjectAlive(botTObj)) {
                if (isTargetibleObjectTeammate(botTObj)) return false;
            }
        }
    }
    return true;
}

static bool isTargetObstructedByWall(const Vector3 &camPos, const Vector3 &bonePos) {
    if (Internal_RaycastTest_Injected == nullptr) return false;

    Vector3 diff = bonePos - camPos;
    float dist3D = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
    if (dist3D <= 0.8f) {
        return false;
    }

    Vector3 dirNorm = diff / dist3D;
    Ray ray;
    ray.origin = camPos + dirNorm * 0.45f;
    ray.direction = dirNorm;

    float checkDist = dist3D - 0.80f;
    if (checkDist <= 0.1f) checkDist = 0.1f;

    PhysicsScene scene(0, 0);
    if (Physics_get_defaultPhysicsScene != nullptr) {
        scene = Physics_get_defaultPhysicsScene();
    }

    bool isBlocked = Internal_RaycastTest_Injected(&scene, &ray, checkDist, -5, 1);
    return isBlocked;
}

struct TargetBoneInfo {
    bool found;
    Vector3 bonePos;
    float score;
    float angleOffset;
    float dist3D;
    bool isHead;
    float targetYaw;
    float targetPitch;
    void *targetibleObj;
};

static TargetBoneInfo findBestTargetBone(const Vector3 &camPos, float currentYaw, float currentPitch, float maxFovAngle, void *cameraObj) {
    TargetBoneInfo bestTarget;
    bestTarget.found = false;
    bestTarget.score = 9999.0f;
    bestTarget.angleOffset = 999.0f;
    bestTarget.dist3D = 999.0f;
    bestTarget.isHead = false;
    bestTarget.targetYaw = currentYaw;
    bestTarget.targetPitch = currentPitch;
    bestTarget.targetibleObj = nullptr;

    std::vector<void*> candidateNetPlayers;
    std::vector<void*> candidateBotPlayers;
    {
        std::lock_guard<std::mutex> lock(g_entityMutex);
        uint64_t now = getCurrentTimeMs();
        const uint64_t TIMEOUT_MS = 2500;
        for (const auto &pair : g_networkPlayers) {
            if (pair.first != nullptr && now - pair.second <= TIMEOUT_MS) {
                candidateNetPlayers.push_back(pair.first);
            }
        }
        for (const auto &pair : g_botPlayers) {
            if (pair.first != nullptr && now - pair.second <= TIMEOUT_MS) {
                candidateBotPlayers.push_back(pair.first);
            }
        }
    }

    auto evaluateCandidate = [&](void *headTransform, void *bodyTransform, void *targetibleObj, void *netPlayer, void *fallbackEntity, void *botPlayer = nullptr) {
        if (!isEntityEnemy(netPlayer, targetibleObj, botPlayer)) return;
        if (isEntityDeadOrCorpse(netPlayer, botPlayer, targetibleObj)) return;

        auto checkBone = [&](void *boneTransform, bool isHead, const Vector3 &offset) {
            if (boneTransform == nullptr || !isPointerReadable(boneTransform) || !isUnityObjectAlive(boneTransform)) return;
            Vector3 rawPos = getTransformPosition(boneTransform);
            if (rawPos.x == 0.0f && rawPos.y == 0.0f && rawPos.z == 0.0f) return;
            Vector3 bonePos = rawPos + offset;

            Vector3 aimDir = bonePos - camPos;
            float dist3D = sqrtf(aimDir.x * aimDir.x + aimDir.y * aimDir.y + aimDir.z * aimDir.z);
            if (dist3D < 0.2f || dist3D > 250.0f) return;

            if (isTargetObstructedByWall(camPos, bonePos)) {
                return;
            }

            Quaternion boneLook = Quaternion::LookRotation(aimDir, Vector3(0.0f, 1.0f, 0.0f));
            Vector3 boneAngle = ToEulerRad(boneLook);
            if (boneAngle.x >= 275.0f) boneAngle.x -= 360.0f;
            if (boneAngle.x <= -275.0f) boneAngle.x += 360.0f;

            float dyaw = boneAngle.y - currentYaw;
            while (dyaw > 180.0f) dyaw -= 360.0f;
            while (dyaw < -180.0f) dyaw += 360.0f;

            float dpitch = boneAngle.x - currentPitch;
            while (dpitch > 180.0f) dpitch -= 360.0f;
            while (dpitch < -180.0f) dpitch += 360.0f;

            float angleOffset = sqrtf(dyaw * dyaw + dpitch * dpitch);

            float allowedAngle = maxFovAngle;
            if (g_bigHead.load() && isHead) {
                allowedAngle *= 1.35f;
            }

            if (angleOffset <= allowedAngle) {
                float score = angleOffset;
                if (isHead) score *= 0.70f;

                if (score < bestTarget.score) {
                    bestTarget.found = true;
                    bestTarget.score = score;
                    bestTarget.angleOffset = angleOffset;
                    bestTarget.dist3D = dist3D;
                    bestTarget.bonePos = bonePos;
                    bestTarget.isHead = isHead;
                    bestTarget.targetYaw = boneAngle.y;
                    bestTarget.targetPitch = boneAngle.x;
                    bestTarget.targetibleObj = targetibleObj;
                }
            }
        };

        if (headTransform != nullptr && isUnityObjectAlive(headTransform)) {
            checkBone(headTransform, true, Vector3(0.0f, 0.0f, 0.0f));
        }
        if (bodyTransform != nullptr && isUnityObjectAlive(bodyTransform)) {
            checkBone(bodyTransform, false, Vector3(0.0f, 0.0f, 0.0f));
        }

        if (headTransform == nullptr && bodyTransform == nullptr && fallbackEntity != nullptr && isUnityObjectAlive(fallbackEntity) && get_transform != nullptr) {
            void *rootTransform = get_transform(fallbackEntity);
            if (rootTransform != nullptr && isUnityObjectAlive(rootTransform)) {
                checkBone(rootTransform, true, Vector3(0.0f, 1.60f, 0.0f));
                checkBone(rootTransform, false, Vector3(0.0f, 1.25f, 0.0f));
            }
        }
    };

    // 1. Process Real Players (NetworkPlayer)
    for (void *netPlayer : candidateNetPlayers) {
        if (netPlayer == nullptr || !isPointerReadable(netPlayer) || !isUnityObjectAlive(netPlayer)) continue;
        if (isNetworkPlayerTeammate(netPlayer)) continue;

        void *headBone = nullptr;
        void *bodyBone = nullptr;
        void *targetibleObj = nullptr;

        if (NetworkPlayer_GetTargetibleObject != nullptr) {
            targetibleObj = NetworkPlayer_GetTargetibleObject(netPlayer);
            if (targetibleObj != nullptr && !isUnityObjectAlive(targetibleObj)) {
                targetibleObj = nullptr;
            }
        }

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

        if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC8))) {
            void *bpm = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(netPlayer) + 0xC8);
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
                                            if (bpView != nullptr && isUnityObjectAlive(bpView) && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(bpView) + 0x20))) {
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

        if (targetibleObj != nullptr && isUnityObjectAlive(targetibleObj)) {
            if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40))) {
                void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                }
            }
            if (bodyBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x48))) {
                void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x48);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
                }
            }
            if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x38))) {
                void *t = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x38);
                if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
            }
        }

        if (headBone == nullptr && bodyBone == nullptr && get_transform != nullptr) {
            void *t = get_transform(netPlayer);
            if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
        }

        evaluateCandidate(headBone, bodyBone, targetibleObj, netPlayer, netPlayer, nullptr);
    }

    // 2. Process AI Bots (BotPlayer)
    for (void *botPlayer : candidateBotPlayers) {
        if (botPlayer == nullptr || !isPointerReadable(botPlayer) || !isUnityObjectAlive(botPlayer)) continue;
        if (!isEntityEnemy(nullptr, nullptr, botPlayer)) continue;

        void *headBone = nullptr;
        void *bodyBone = nullptr;
        void *targetibleObj = nullptr;
        void *netPlayer = nullptr;

        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x50))) {
            netPlayer = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x50);
            if (netPlayer != nullptr) {
                if (!isUnityObjectAlive(netPlayer)) {
                    netPlayer = nullptr;
                } else if (isNetworkPlayerTeammate(netPlayer)) {
                    continue;
                }
            }
        }

        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x60))) {
            targetibleObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x60);
        }
        if (targetibleObj == nullptr && BotPlayer_GetTargetibleObject != nullptr) {
            targetibleObj = BotPlayer_GetTargetibleObject(botPlayer);
        }
        if (targetibleObj != nullptr) {
            if (!isUnityObjectAlive(targetibleObj)) {
                targetibleObj = nullptr;
            } else if (isTargetibleObjectTeammate(targetibleObj)) {
                continue;
            }
        }

        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x58))) {
            void *tpCtrl = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x58);
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

        if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botPlayer) + 0x28))) {
            void *botLook = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botPlayer) + 0x28);
            if (botLook != nullptr && isUnityObjectAlive(botLook)) {
                if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(botLook) + 0x28))) {
                    void *t = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(botLook) + 0x28);
                    if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                }
            }
        }

        if (targetibleObj != nullptr && isUnityObjectAlive(targetibleObj)) {
            if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40))) {
                void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x40);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                }
            }
            if (bodyBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x48))) {
                void *col = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x48);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
                }
            }
            if (headBone == nullptr && isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x38))) {
                void *t = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(targetibleObj) + 0x38);
                if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
            }
        }

        if (headBone == nullptr && bodyBone == nullptr && get_transform != nullptr) {
            void *t = get_transform(botPlayer);
            if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
        }

        evaluateCandidate(headBone, bodyBone, targetibleObj, netPlayer, botPlayer, botPlayer);
    }

    return bestTarget;
}

static uint64_t g_lastAimAssistProcessMs = 0;
static uint64_t g_lastAimAssistLogMs = 0;
static float g_lastFrameYaw = 0.0f;
static float g_lastFramePitch = 0.0f;
static bool g_hasLastFrameAngles = false;

static void processAimAssistLock(void *aimingControl) {
    if (aimingControl == nullptr || !isPointerReadable(aimingControl) || !isUnityObjectAlive(aimingControl)) return;

    uint64_t nowMs = getCurrentTimeMs();
    if (nowMs - g_lastAimAssistProcessMs < 4) {
        return; // Debounce duplicate calls within the same frame (~16ms)
    }
    g_lastAimAssistProcessMs = nowMs;

    void *azimuthNode = nullptr;
    void *elevationNode = nullptr;
    void *cameraObj = nullptr;

    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0x28))) {
        azimuthNode = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(aimingControl) + 0x28);
    }
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0x30))) {
        elevationNode = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(aimingControl) + 0x30);
    }
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0x20))) {
        cameraObj = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(aimingControl) + 0x20);
        if (cameraObj != nullptr && isUnityObjectAlive(cameraObj)) {
            g_activeAimingCamera.store(cameraObj);
        }
    }

    if (azimuthNode == nullptr || elevationNode == nullptr) return;
    if (!isUnityObjectAlive(azimuthNode) || !isUnityObjectAlive(elevationNode)) return;

    Vector3 azEuler(0.0f, 0.0f, 0.0f);
    Vector3 elEuler(0.0f, 0.0f, 0.0f);

    if (!getTransformLocalEulerAngles(azimuthNode, azEuler) ||
        !getTransformLocalEulerAngles(elevationNode, elEuler)) {
        return;
    }

    float currentYaw = azEuler.y;
    float currentPitch = (elEuler.x > 180.0f) ? (elEuler.x - 360.0f) : elEuler.x;

    float userDeltaYaw = 0.0f;
    float userDeltaPitch = 0.0f;
    if (g_hasLastFrameAngles) {
        userDeltaYaw = currentYaw - g_lastFrameYaw;
        while (userDeltaYaw > 180.0f) userDeltaYaw -= 360.0f;
        while (userDeltaYaw < -180.0f) userDeltaYaw += 360.0f;

        userDeltaPitch = currentPitch - g_lastFramePitch;
        while (userDeltaPitch > 180.0f) userDeltaPitch -= 360.0f;
        while (userDeltaPitch < -180.0f) userDeltaPitch += 360.0f;
    }
    g_lastFrameYaw = currentYaw;
    g_lastFramePitch = currentPitch;
    g_hasLastFrameAngles = true;

    Vector3 camPos = getTransformPosition(elevationNode);
    if (camPos.x == 0.0f && camPos.y == 0.0f && camPos.z == 0.0f) {
        if (cameraObj != nullptr && isUnityObjectAlive(cameraObj)) {
            camPos = getTransformPosition(cameraObj);
        }
    }
    void *localFpc = g_localPlayerFPC.load();
    if (camPos.x == 0.0f && camPos.y == 0.0f && camPos.z == 0.0f && localFpc != nullptr && isUnityObjectAlive(localFpc)) {
        camPos = getTransformPosition(localFpc) + Vector3(0.0f, 1.6f, 0.0f);
    }

    int sliderVal = g_aimSmoothness.load();
    if (sliderVal < 0) sliderVal = 0;
    if (sliderVal > 100) sliderVal = 100;
    float s = static_cast<float>(sliderVal) / 100.0f;

    float maxFovAngle = g_bigHead.load() ? (24.0f + 6.0f * s) : (18.0f + 6.0f * s);

    TargetBoneInfo targetInfo = findBestTargetBone(camPos, currentYaw, currentPitch, maxFovAngle, cameraObj);
    if (!targetInfo.found) {
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0xC8))) {
            *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(aimingControl) + 0xC8) = nullptr;
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0xD0))) {
            *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(aimingControl) + 0xD0) = false;
        }
        return;
    }

    float diffYaw = targetInfo.targetYaw - currentYaw;
    while (diffYaw > 180.0f) diffYaw -= 360.0f;
    while (diffYaw < -180.0f) diffYaw += 360.0f;

    float diffPitch = targetInfo.targetPitch - currentPitch;
    while (diffPitch > 180.0f) diffPitch -= 360.0f;
    while (diffPitch < -180.0f) diffPitch += 360.0f;

    float proximity = 1.0f - (targetInfo.angleOffset / (maxFovAngle + 0.001f));
    if (proximity < 0.0f) proximity = 0.0f;
    if (proximity > 1.0f) proximity = 1.0f;
    float weight = proximity * proximity;

    float assistRate = (0.03f + 0.15f * s) * weight;

    if ((userDeltaYaw > 0.3f && diffYaw < -0.2f) || (userDeltaYaw < -0.3f && diffYaw > 0.2f)) {
        assistRate *= 0.15f;
    }
    if ((userDeltaPitch > 0.3f && diffPitch < -0.2f) || (userDeltaPitch < -0.3f && diffPitch > 0.2f)) {
        assistRate *= 0.15f;
    }

    float stepYaw = diffYaw * assistRate;
    float stepPitch = diffPitch * assistRate;

    float maxStepPerFrame = 0.4f + 1.2f * s;
    float stepLen = sqrtf(stepYaw * stepYaw + stepPitch * stepPitch);
    if (stepLen > maxStepPerFrame) {
        float scale = maxStepPerFrame / stepLen;
        stepYaw *= scale;
        stepPitch *= scale;
    }

    float newYaw = currentYaw + stepYaw;
    float newPitch = currentPitch + stepPitch;

    float minPitch = -85.0f;
    float maxPitch = 85.0f;
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0x38))) {
        float pMin = *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(aimingControl) + 0x38);
        if (pMin >= -90.0f && pMin < 0.0f) minPitch = pMin;
    }
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0x3C))) {
        float pMax = *reinterpret_cast<float *>(reinterpret_cast<uintptr_t>(aimingControl) + 0x3C);
        if (pMax > 0.0f && pMax <= 90.0f) maxPitch = pMax;
    }

    if (newPitch < minPitch) newPitch = minPitch;
    if (newPitch > maxPitch) newPitch = maxPitch;

    azEuler.y = fmodf(newYaw + 360.0f, 360.0f);
    elEuler.x = (newPitch < 0.0f) ? (newPitch + 360.0f) : newPitch;

    setTransformLocalEulerAngles(azimuthNode, azEuler);
    setTransformLocalEulerAngles(elevationNode, elEuler);

    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0xC8))) {
        *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(aimingControl) + 0xC8) = nullptr;
    }
    if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(aimingControl) + 0xD0))) {
        *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(aimingControl) + 0xD0) = false;
    }

    if (nowMs - g_lastAimAssistLogMs > 6000) {
        g_lastAimAssistLogMs = nowMs;
        ModLog("[AIM_ASSIST] Smooth Assist Active -> Target: %s | Dist: %.1fm | Angle: %.1f deg | Smoothness: %d%% | Step: (%.2f, %.2f)",
               targetInfo.isHead ? "HEAD BONE" : "BODY BONE",
               targetInfo.dist3D,
               targetInfo.angleOffset,
               sliderVal,
               stepYaw, stepPitch);
    }
}

// 0. AimingControl.Update: RVA 0x4D27294
void (*old_AimingControl_Update)(void *instance) = nullptr;
void hook_AimingControl_Update(void *instance) {
    if (instance != nullptr && isUnityObjectAlive(instance)) {
        g_activeAimingControl.store(instance);

        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0xC8))) {
            *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(instance) + 0xC8) = nullptr;
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0xD0))) {
            *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(instance) + 0xD0) = false;
        }
    }

    if (old_AimingControl_Update != nullptr) {
        old_AimingControl_Update(instance);
    }

    if (!g_aimAssistBoost.load()) {
        if (instance != nullptr && isPointerReadable(instance)) {
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0xC8))) {
                *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(instance) + 0xC8) = nullptr;
            }
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(instance) + 0xD0))) {
                *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(instance) + 0xD0) = false;
            }
        }
        g_hasLastFrameAngles = false;
        return;
    }

    if (instance != nullptr && isUnityObjectAlive(instance)) {
        processAimAssistLock(instance);
    }
}

static inline float getAimSensitivityFactor() {
    int val = g_aimSmoothness.load();
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    return static_cast<float>(val) / 100.0f;
}

// 1. StrafeRotationConfig (0x52A6264, 0x52A6210, 0x52A624C, 0x52A65A4, 0x52A6598)
float (*old_StrafeRotationConfig_get_MaxRotationPower)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_MaxRotationPower(void *instance) {
    float power = old_StrafeRotationConfig_get_MaxRotationPower ? old_StrafeRotationConfig_get_MaxRotationPower(instance) : 0.2f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 0.25f + 0.45f * factor;
    }
    return power;
}

float (*old_StrafeRotationConfig_get_FOVAreaMultiplier)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_FOVAreaMultiplier(void *instance) {
    float fov = old_StrafeRotationConfig_get_FOVAreaMultiplier ? old_StrafeRotationConfig_get_FOVAreaMultiplier(instance) : 1.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 1.05f + 0.25f * factor;
    }
    return fov;
}

float (*old_StrafeRotationConfig_get_DefaultStateMaxDistance)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_DefaultStateMaxDistance(void *instance) {
    float dist = old_StrafeRotationConfig_get_DefaultStateMaxDistance ? old_StrafeRotationConfig_get_DefaultStateMaxDistance(instance) : 50.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 70.0f + 50.0f * factor;
    }
    return dist;
}

float (*old_StrafeRotationConfig_get_ZoomedStateMaxDistance)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_ZoomedStateMaxDistance(void *instance) {
    float dist = old_StrafeRotationConfig_get_ZoomedStateMaxDistance ? old_StrafeRotationConfig_get_ZoomedStateMaxDistance(instance) : 100.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 140.0f + 110.0f * factor;
    }
    return dist;
}

float (*old_StrafeRotationConfig_get_FOVPowerMultiplier)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_FOVPowerMultiplier(void *instance) {
    float fov = old_StrafeRotationConfig_get_FOVPowerMultiplier ? old_StrafeRotationConfig_get_FOVPowerMultiplier(instance) : 1.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 1.10f + 0.40f * factor;
    }
    return fov;
}

// 2. SpinSlowdownConfig (0x4600D68, 0x4600BCC, 0x4600BD8, 0x4600C44, 0x4600C5C)
float (*old_SpinSlowdownConfig_get_MaxSlowdownValue)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_MaxSlowdownValue(void *instance) {
    float val = old_SpinSlowdownConfig_get_MaxSlowdownValue ? old_SpinSlowdownConfig_get_MaxSlowdownValue(instance) : 0.3f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 0.15f + 0.20f * factor;
    }
    return val;
}

float (*old_SpinSlowdownConfig_get_FOVAreaMultiplier)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_FOVAreaMultiplier(void *instance) {
    float fov = old_SpinSlowdownConfig_get_FOVAreaMultiplier ? old_SpinSlowdownConfig_get_FOVAreaMultiplier(instance) : 1.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 1.05f + 0.20f * factor;
    }
    return fov;
}

float (*old_SpinSlowdownConfig_get_Radius)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_Radius(void *instance) {
    float r = old_SpinSlowdownConfig_get_Radius ? old_SpinSlowdownConfig_get_Radius(instance) : 1.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 1.05f + 0.25f * factor;
    }
    return r;
}

float (*old_SpinSlowdownConfig_get_DefaultStateMaxDistance)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_DefaultStateMaxDistance(void *instance) {
    float dist = old_SpinSlowdownConfig_get_DefaultStateMaxDistance ? old_SpinSlowdownConfig_get_DefaultStateMaxDistance(instance) : 50.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 70.0f + 50.0f * factor;
    }
    return dist;
}

float (*old_SpinSlowdownConfig_get_ZoomedStateMaxDistance)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_ZoomedStateMaxDistance(void *instance) {
    float dist = old_SpinSlowdownConfig_get_ZoomedStateMaxDistance ? old_SpinSlowdownConfig_get_ZoomedStateMaxDistance(instance) : 100.0f;
    if (g_aimAssistBoost.load()) {
        float factor = getAimSensitivityFactor();
        return 140.0f + 110.0f * factor;
    }
    return dist;
}

// 3. StrafeRotationAimAssist.Calculate (0x4F0E254)
Vector2 (*old_StrafeRotation_Calculate)(void *instance, Vector2 currentDelta, Vector2 smoothed) = nullptr;
Vector2 hook_StrafeRotation_Calculate(void *instance, Vector2 currentDelta, Vector2 smoothed) {
    if (old_StrafeRotation_Calculate == nullptr) return currentDelta;
    Vector2 result = old_StrafeRotation_Calculate(instance, currentDelta, smoothed);
    if (!g_aimAssistBoost.load()) return result;

    float assistX = result.x - currentDelta.x;
    float assistY = result.y - currentDelta.y;
    float assistMag = sqrtf(assistX * assistX + assistY * assistY);

    if (assistMag < 0.0001f) {
        return currentDelta;
    }

    float factor = getAimSensitivityFactor();
    float boost = 1.15f + 0.85f * factor;
    assistX *= boost;
    assistY *= boost;

    float maxStep = 2.0f + 2.5f * factor;
    float boostedMag = sqrtf(assistX * assistX + assistY * assistY);
    if (boostedMag > maxStep) {
        float scale = maxStep / boostedMag;
        assistX *= scale;
        assistY *= scale;
    }

    result.x = currentDelta.x + assistX;
    result.y = currentDelta.y + assistY;
    return result;
}

// 4. AimAssistManager.SetEnabled (0x529060C)
void (*old_AimAssistManager_SetEnabled)(void *instance, bool enabled) = nullptr;
void hook_AimAssistManager_SetEnabled(void *instance, bool enabled) {
    g_activeAimAssistManager.store(instance);
    if (old_AimAssistManager_SetEnabled != nullptr) {
        old_AimAssistManager_SetEnabled(instance, g_aimAssistBoost.load());
    }
}

// 5. NewAutoAim.SetEnabled (0x421C470)
void (*old_NewAutoAim_SetEnabled)(void *instance, bool enabled) = nullptr;
void hook_NewAutoAim_SetEnabled(void *instance, bool enabled) {
    g_activeNewAutoAim.store(instance);
    if (old_NewAutoAim_SetEnabled != nullptr) {
        old_NewAutoAim_SetEnabled(instance, g_aimAssistBoost.load());
    }
}

// 6. BaseAimAssist.SetEnabled (0x4478A30)
void (*old_BaseAimAssist_SetEnabled)(void *instance, bool enabled) = nullptr;
void hook_BaseAimAssist_SetEnabled(void *instance, bool enabled) {
    g_activeBaseAimAssist.store(instance);
    if (old_BaseAimAssist_SetEnabled != nullptr) {
        old_BaseAimAssist_SetEnabled(instance, g_aimAssistBoost.load());
    }
}

// 7. BaseAimAssist.IsValidTarget (0x4478A70) & NewAutoAim.IsValidTarget (0x421C254)
bool (*old_BaseAimAssist_IsValidTarget)(void *instance, void *targetibleObject) = nullptr;
bool hook_BaseAimAssist_IsValidTarget(void *instance, void *targetibleObject) {
    if (!g_aimAssistBoost.load()) return false;
    if (old_BaseAimAssist_IsValidTarget == nullptr) return false;
    if (targetibleObject == nullptr || !isUnityObjectAlive(targetibleObject)) return false;
    if (isTargetibleObjectTeammate(targetibleObject)) {
        return false;
    }
    if (isEntityDeadOrCorpse(nullptr, nullptr, targetibleObject)) {
        return false;
    }
    return old_BaseAimAssist_IsValidTarget(instance, targetibleObject);
}

bool (*old_NewAutoAim_IsValidTarget)(void *instance, void *targetibleObject) = nullptr;
bool hook_NewAutoAim_IsValidTarget(void *instance, void *targetibleObject) {
    if (!g_aimAssistBoost.load()) return false;
    if (old_NewAutoAim_IsValidTarget == nullptr) return false;
    if (targetibleObject == nullptr || !isUnityObjectAlive(targetibleObject)) return false;
    if (isTargetibleObjectTeammate(targetibleObject)) {
        return false;
    }
    if (isEntityDeadOrCorpse(nullptr, nullptr, targetibleObject)) {
        return false;
    }
    return old_NewAutoAim_IsValidTarget(instance, targetibleObject);
}

static void resetAimAssistState(void *aimingControlInstance = nullptr) {
    void *ac = aimingControlInstance ? aimingControlInstance : g_activeAimingControl.load();
    if (ac != nullptr && isPointerReadable(ac)) {
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ac) + 0xC8))) {
            *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(ac) + 0xC8) = nullptr;
        }
        if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ac) + 0xD0))) {
            *reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(ac) + 0xD0) = false;
        }
        if (AimingControl_ClearTargetibleObject != nullptr) {
            AimingControl_ClearTargetibleObject(ac);
        }
    }
    void *mgr = g_activeAimAssistManager.load();
    if (old_AimAssistManager_SetEnabled != nullptr && mgr != nullptr && isPointerReadable(mgr)) {
        old_AimAssistManager_SetEnabled(mgr, false);
    }
    void *na = g_activeNewAutoAim.load();
    if (old_NewAutoAim_SetEnabled != nullptr && na != nullptr && isPointerReadable(na)) {
        old_NewAutoAim_SetEnabled(na, false);
    }
    void *ba = g_activeBaseAimAssist.load();
    if (old_BaseAimAssist_SetEnabled != nullptr && ba != nullptr && isPointerReadable(ba)) {
        old_BaseAimAssist_SetEnabled(ba, false);
    }
}

// =========================================================================
// Il2Cpp Lifecycle Hooks
// =========================================================================

// NetworkPlayer.Update: RVA 0x42CC8C8
void (*old_NetworkPlayer_Update)(void *instance) = nullptr;
void hook_NetworkPlayer_Update(void *instance) {
    if (instance != nullptr && isUnityObjectAlive(instance)) {
        setLastAction("NetworkPlayer_Update");
        onNetworkPlayerUpdate(instance);

        if (espManager != nullptr && (g_espLine.load() || g_espBox.load())) {
            espManager->tryAddEnemy(instance);
        }

        if (g_bigHead.load()) {
            applyBigHeadToNetworkPlayer(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset.load()) {
            applyBigHeadToNetworkPlayer(instance, NORMAL_HEAD_SCALE);
            int curFrames = g_bigHeadResetFrames.load();
            if (curFrames > 0) {
                g_bigHeadResetFrames.store(curFrames - 1);
                if (curFrames <= 1) {
                    g_needsBigHeadReset.store(false);
                    ModLog("[BIG_HEAD] Reset frames completed. Restored normal head scale.");
                }
            }
        }
    } else if (instance != nullptr) {
        onNetworkPlayerDestroy(instance);
        if (espManager != nullptr) {
            espManager->removeEnemyGivenObject(instance);
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
        setLastAction("NetworkPlayer_OnDestroy");
        onNetworkPlayerDestroy(instance);
        if (espManager != nullptr) {
            espManager->removeEnemyGivenObject(instance);
        }
    }
    if (old_NetworkPlayer_OnDestroy != nullptr) {
        old_NetworkPlayer_OnDestroy(instance);
    }
}

// BotPlayer.Start: RVA 0x44493F4
void (*old_BotPlayer_Start)(void *instance) = nullptr;
void hook_BotPlayer_Start(void *instance) {
    if (instance != nullptr && isUnityObjectAlive(instance)) {
        setLastAction("BotPlayer_Start");
        onBotPlayerUpdate(instance);

        if (espManager != nullptr && (g_espLine.load() || g_espBox.load())) {
            espManager->tryAddEnemy(instance);
        }
    }
    if (old_BotPlayer_Start != nullptr) {
        old_BotPlayer_Start(instance);
    }
}

// BotPlayer.Update: RVA 0x444A310
void (*old_BotPlayer_Update)(void *instance) = nullptr;
void hook_BotPlayer_Update(void *instance) {
    if (instance != nullptr && isUnityObjectAlive(instance)) {
        setLastAction("BotPlayer_Update");
        onBotPlayerUpdate(instance);

        if (espManager != nullptr && (g_espLine.load() || g_espBox.load())) {
            espManager->tryAddEnemy(instance);
        }

        if (g_bigHead.load()) {
            applyBigHeadToBotPlayer(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset.load()) {
            applyBigHeadToBotPlayer(instance, NORMAL_HEAD_SCALE);
        }
    } else if (instance != nullptr) {
        onBotPlayerDestroy(instance);
        if (espManager != nullptr) {
            espManager->removeEnemyGivenObject(instance);
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
        setLastAction("BotPlayer_OnDestroy");
        onBotPlayerDestroy(instance);
        if (espManager != nullptr) {
            espManager->removeEnemyGivenObject(instance);
        }
    }
    if (old_BotPlayer_OnDestroy != nullptr) {
        old_BotPlayer_OnDestroy(instance);
    }
}

// Registers all hooks in a clean, extensible registry table using HookManager
static void registerAllHooks() {
    auto &mgr = HookManager::getInstance();
    mgr.clear();

    // 1. Client-Side Fast FireRate & Weapon Hooks
    mgr.registerHook(Offsets::FPC_UPDATE,                    reinterpret_cast<void*>(hook_FirstPersonController_Update),        reinterpret_cast<void**>(&old_FirstPersonController_Update),        "FirstPersonController.Update");
    mgr.registerHook(Offsets::WEAPON_CAN_SHOOT,              reinterpret_cast<void*>(hook_WeaponShooterBehaviour_CanShoot),     reinterpret_cast<void**>(&old_WeaponShooterBehaviour_CanShoot),     "WeaponShooterBehaviour.CanShoot");
    mgr.registerHook(Offsets::WEAPON_SET_COOLDOWN,           reinterpret_cast<void*>(hook_WeaponShooterBehaviour_SetCooldown),  reinterpret_cast<void**>(&old_WeaponShooterBehaviour_SetCooldown),  "WeaponShooterBehaviour.SetCooldown");
    mgr.registerHook(Offsets::FPC_GET_SHOT_INTERVAL1,        reinterpret_cast<void*>(hook_FPC_GetShotInterval1),                reinterpret_cast<void**>(&old_FPC_GetShotInterval1),                "FirstPersonController.GetShotInterval1");
    mgr.registerHook(Offsets::FPC_GET_SHOT_INTERVAL2,        reinterpret_cast<void*>(hook_FPC_GetShotInterval2),                reinterpret_cast<void**>(&old_FPC_GetShotInterval2),                "FirstPersonController.GetShotInterval2");
    mgr.registerHook(Offsets::SHOOT_COROUTINE_MOVE_NEXT,     reinterpret_cast<void*>(hook_ShootCoroutine_MoveNext),             reinterpret_cast<void**>(&old_ShootCoroutine_MoveNext),             "ShootCoroutine.MoveNext");
    mgr.registerHook(Offsets::WEAPON_PARAMS_GET_FIRE_RATE,   reinterpret_cast<void*>(hook_WeaponParams_GetFireRate),            reinterpret_cast<void**>(&old_WeaponParams_GetFireRate),            "WeaponParameters.GetFireRate");
    mgr.registerHook(Offsets::WEAPON_PROFILE_GET_FIRE_RATE,  reinterpret_cast<void*>(hook_WeaponProfile_GetFireRate),           reinterpret_cast<void**>(&old_WeaponProfile_GetFireRate),           "WeaponProfile.GetFireRate");

    // 2. Client-Side No Recoil Hooks
    mgr.registerHook(Offsets::FPC_RECOIL_SHIFT,              reinterpret_cast<void*>(hook_FPC_Recoil),                          reinterpret_cast<void**>(&old_FPC_Recoil),                          "FirstPersonController.RecoilShift");
    mgr.registerHook(Offsets::AIMING_CONTROL_RECOIL1,        reinterpret_cast<void*>(hook_AimingControl_Recoil1),               reinterpret_cast<void**>(&old_AimingControl_Recoil1),               "AimingControl.RecoilShift1");
    mgr.registerHook(Offsets::AIMING_CONTROL_RECOIL2,        reinterpret_cast<void*>(hook_AimingControl_Recoil2),               reinterpret_cast<void**>(&old_AimingControl_Recoil2),               "AimingControl.RecoilShift2");
    mgr.registerHook(Offsets::AIMING_CONTROL_RECOIL3,        reinterpret_cast<void*>(hook_AimingControl_Recoil3),               reinterpret_cast<void**>(&old_AimingControl_Recoil3),               "AimingControl.RecoilShift3");

    // 3. Entity Lifecycle Hooks
    mgr.registerHook(Offsets::NETWORK_PLAYER_UPDATE,         reinterpret_cast<void*>(hook_NetworkPlayer_Update),                reinterpret_cast<void**>(&old_NetworkPlayer_Update),                "NetworkPlayer.Update");
    mgr.registerHook(Offsets::NETWORK_PLAYER_ON_DESTROY,     reinterpret_cast<void*>(hook_NetworkPlayer_OnDestroy),            reinterpret_cast<void**>(&old_NetworkPlayer_OnDestroy),            "NetworkPlayer.OnDestroy");
    mgr.registerHook(Offsets::BOT_PLAYER_START,              reinterpret_cast<void*>(hook_BotPlayer_Start),                     reinterpret_cast<void**>(&old_BotPlayer_Start),                     "BotPlayer.Start");
    mgr.registerHook(Offsets::BOT_PLAYER_UPDATE,             reinterpret_cast<void*>(hook_BotPlayer_Update),                    reinterpret_cast<void**>(&old_BotPlayer_Update),                    "BotPlayer.Update");
    mgr.registerHook(Offsets::BOT_PLAYER_ON_DESTROY,         reinterpret_cast<void*>(hook_BotPlayer_OnDestroy),                reinterpret_cast<void**>(&old_BotPlayer_OnDestroy),                "BotPlayer.OnDestroy");

    // 4. Native Aim Assist & Sensitivity Reinforcement Hooks
    mgr.registerHook(Offsets::STRAFE_ROT_GET_MAX_ROTATION_POWER,   reinterpret_cast<void*>(hook_StrafeRotationConfig_get_MaxRotationPower),   reinterpret_cast<void**>(&old_StrafeRotationConfig_get_MaxRotationPower),   "StrafeRotationConfig.get_MaxRotationPower");
    mgr.registerHook(Offsets::STRAFE_ROT_GET_FOV_AREA_MULTIPLIER,  reinterpret_cast<void*>(hook_StrafeRotationConfig_get_FOVAreaMultiplier),  reinterpret_cast<void**>(&old_StrafeRotationConfig_get_FOVAreaMultiplier),  "StrafeRotationConfig.get_FOVAreaMultiplier");
    mgr.registerHook(Offsets::STRAFE_ROT_GET_DEFAULT_MAX_DISTANCE, reinterpret_cast<void*>(hook_StrafeRotationConfig_get_DefaultStateMaxDistance), reinterpret_cast<void**>(&old_StrafeRotationConfig_get_DefaultStateMaxDistance), "StrafeRotationConfig.get_DefaultStateMaxDistance");
    mgr.registerHook(Offsets::STRAFE_ROT_GET_ZOOMED_MAX_DISTANCE,  reinterpret_cast<void*>(hook_StrafeRotationConfig_get_ZoomedStateMaxDistance),  reinterpret_cast<void**>(&old_StrafeRotationConfig_get_ZoomedStateMaxDistance),  "StrafeRotationConfig.get_ZoomedStateMaxDistance");
    mgr.registerHook(Offsets::STRAFE_ROT_GET_FOV_POWER_MULTIPLIER, reinterpret_cast<void*>(hook_StrafeRotationConfig_get_FOVPowerMultiplier), reinterpret_cast<void**>(&old_StrafeRotationConfig_get_FOVPowerMultiplier), "StrafeRotationConfig.get_FOVPowerMultiplier");

    mgr.registerHook(Offsets::SPIN_SLOWDOWN_GET_MAX_SLOWDOWN,      reinterpret_cast<void*>(hook_SpinSlowdownConfig_get_MaxSlowdownValue),      reinterpret_cast<void**>(&old_SpinSlowdownConfig_get_MaxSlowdownValue),      "SpinSlowdownConfig.get_MaxSlowdownValue");
    mgr.registerHook(Offsets::SPIN_SLOWDOWN_GET_FOV_AREA_MULT,     reinterpret_cast<void*>(hook_SpinSlowdownConfig_get_FOVAreaMultiplier),     reinterpret_cast<void**>(&old_SpinSlowdownConfig_get_FOVAreaMultiplier),     "SpinSlowdownConfig.get_FOVAreaMultiplier");
    mgr.registerHook(Offsets::SPIN_SLOWDOWN_GET_RADIUS,            reinterpret_cast<void*>(hook_SpinSlowdownConfig_get_Radius),                reinterpret_cast<void**>(&old_SpinSlowdownConfig_get_Radius),                "SpinSlowdownConfig.get_Radius");
    mgr.registerHook(Offsets::SPIN_SLOWDOWN_GET_DEFAULT_MAX_DIST,  reinterpret_cast<void*>(hook_SpinSlowdownConfig_get_DefaultStateMaxDistance),  reinterpret_cast<void**>(&old_SpinSlowdownConfig_get_DefaultStateMaxDistance),  "SpinSlowdownConfig.get_DefaultStateMaxDistance");
    mgr.registerHook(Offsets::SPIN_SLOWDOWN_GET_ZOOMED_MAX_DIST,   reinterpret_cast<void*>(hook_SpinSlowdownConfig_get_ZoomedStateMaxDistance),   reinterpret_cast<void**>(&old_SpinSlowdownConfig_get_ZoomedStateMaxDistance),   "SpinSlowdownConfig.get_ZoomedStateMaxDistance");

    mgr.registerHook(Offsets::STRAFE_ROTATION_CALCULATE,           reinterpret_cast<void*>(hook_StrafeRotation_Calculate),                     reinterpret_cast<void**>(&old_StrafeRotation_Calculate),                     "StrafeRotationAimAssist.Calculate");
    mgr.registerHook(Offsets::AIM_ASSIST_MGR_SET_ENABLED,          reinterpret_cast<void*>(hook_AimAssistManager_SetEnabled),                  reinterpret_cast<void**>(&old_AimAssistManager_SetEnabled),                  "AimAssistManager.SetEnabled");
    mgr.registerHook(Offsets::AIMING_CONTROL_UPDATE,               reinterpret_cast<void*>(hook_AimingControl_Update),                         reinterpret_cast<void**>(&old_AimingControl_Update),                         "AimingControl.Update");
    mgr.registerHook(Offsets::NEW_AUTO_AIM_SET_ENABLED,            reinterpret_cast<void*>(hook_NewAutoAim_SetEnabled),                        reinterpret_cast<void**>(&old_NewAutoAim_SetEnabled),                        "NewAutoAim.SetEnabled");
    mgr.registerHook(Offsets::BASE_AIM_ASSIST_SET_ENABLED,         reinterpret_cast<void*>(hook_BaseAimAssist_SetEnabled),                     reinterpret_cast<void**>(&old_BaseAimAssist_SetEnabled),                     "BaseAimAssist.SetEnabled");

    // 5. Target Filtering Hooks (Ignore Teammates & Allies)
    mgr.registerHook(Offsets::BASE_AIM_ASSIST_IS_VALID_TARGET,     reinterpret_cast<void*>(hook_BaseAimAssist_IsValidTarget),                  reinterpret_cast<void**>(&old_BaseAimAssist_IsValidTarget),                  "BaseAimAssist.IsValidTarget");
    mgr.registerHook(Offsets::NEW_AUTO_AIM_IS_VALID_TARGET,        reinterpret_cast<void*>(hook_NewAutoAim_IsValidTarget),                     reinterpret_cast<void**>(&old_NewAutoAim_IsValidTarget),                     "NewAutoAim.IsValidTarget");
}

// Background thread waiting for libil2cpp.so
void *hack_thread(void *) {
    ModLog("[THREAD] hack_thread started for GTA SA FPS. Waiting for %s...", static_cast<const char *>(targetLibName));
    setLastAction("Waiting for libil2cpp.so");

    do {
        sleep(1);
    } while (!isLibraryLoaded(targetLibName));

    uintptr_t base = findLibrary(targetLibName);
    g_cachedLibBase = base;
    ModLog("[THREAD] %s loaded successfully at base: 0x%lx", static_cast<const char *>(targetLibName), base);
    setLastAction("libil2cpp.so loaded");

    initSafetyPipe();
    initAllFunctionPointers(base);

#if defined(__aarch64__)
    ModLog("[HOOK] Installing hooks on libil2cpp.so via HookManager (arm64-v8a)...");
    registerAllHooks();
    HookManager::getInstance().installAll(base);

    if (espManager == nullptr) {
        espManager = new ESPManager();
        ModLog("[ESP] ESPManager initialized successfully.");
    }

    ModLog("[THREAD] All core hooks installed successfully!");
    setLastAction("Hooks installed and ready");
#else
    ModLog("[WARN] Target is arm64-v8a only. 32-bit not supported.");
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
        OBFUSCATE("Toggle_Status Panel Overlay"), // featNum 0
        OBFUSCATE("Toggle_Big Head"),             // featNum 1
        OBFUSCATE("Toggle_Fast FireRate (Client-Side)"), // featNum 2
        OBFUSCATE("Toggle_No Recoil (Client-Side)"),     // featNum 3
        OBFUSCATE("Toggle_Aim Assist (Auto-Lock Radius)"), // featNum 4
        OBFUSCATE("SeekBar_Aim Smoothness (Sticky Lock)_0_100"), // featNum 5
        OBFUSCATE("Collapse_🎯 TAB ESP OVERLAY_True"),
        OBFUSCATE("CollapseAdd_6_Toggle_ESP Line (Kuning)"),
        OBFUSCATE("CollapseAdd_7_Toggle_ESP Box"),
        OBFUSCATE("Category_📊 STATUS & DEBUG INFO"),
        OBFUSCATE("RichTextView_<div style='background-color:#16222F;padding:10px;border:1px solid #00E5FF;border-radius:6px;'><font color='#00FF7F'><b>[ GTA SA FPS MOD MENU ]</b></font><br><font color='#FFFFFF'>• <b>Status Panel Overlay:</b> HUD real-time counter Player & Bot.<br><br>• <b>ESP Line (Kuning):</b> Garis pelacak berwarna kuning dari atas layar langsung ke posisi kepala musuh & bot secara real-time.<br><br>• <b>ESP Box:</b> Kotak 2D bounding box berwarna kuning di sekeliling musuh & bot dengan indikator jarak.<br><br>• <b>Big Head:</b> Memperbesar kepala Player & Bot (Client-Side).<br><br>• <b>Fast FireRate (Client-Side):</b> Tembakan senjata berkecepatan tinggi hanya untuk client (player) via dynamic weapon memory & ACTk ObscuredFloat bypass.<br><br>• <b>No Recoil (Client-Side):</b> Menghilangkan hentakan/recoil senjata player 100% (Bidikan lurus tanpa getaran).<br><br>• <b>Aim Assist (Auto-Lock):</b> Hook AimingControl & Camera dari dump.cs. Otomatis mengabaikan rekan tim & langsung mengunci target saat arah bidikan dekat dengan bone musuh.<br><br>• <b>Aim Smoothness Slider (0-100):</b> Menyesuaikan kehalusan kuncian. Saat nilai di-mentokkan (100), bidikan akan selalu lengket (100% magnetic sticky lock) pada bone body atau head musuh.<br><br>• <b>Debug Logger:</b> Aktif otomatis ke <i>/storage/0/emulated/Document/mod_gta_debug.log</i></font></div>")
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = static_cast<jobjectArray>(
        env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                            env->NewStringUTF(""))
    );

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return ret;
}

void Changes(JNIEnv *env, jclass clazz, jobject ctx,
             jint featNum, jstring featName, jint value,
             jboolean boolean, jstring str) {

    LOGD(OBFUSCATE("Changes: featNum=%d, val=%d, bool=%d"), featNum, value, boolean);

    const char *stateStr = boolean ? "ON (ACTIVE)" : "OFF (INACTIVE)";

    switch (featNum) {
        case 0: { // Toggle_Status Panel Overlay (Auto Count Player & Bot)
            g_showStatusPanel.store(boolean);
            g_autoCount.store(boolean);
            ModLog("[TOGGLE] Feature #0 [Status Panel Overlay & Auto Count] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Status Panel: ON" : "Toggle Status Panel: OFF");

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Status Panel & Auto Counter: ON"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Status Panel & Auto Counter: OFF"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 1: { // Toggle_Big Head
            g_bigHead.store(boolean);
            ModLog("[TOGGLE] Feature #1 [Big Head (Client-Side)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Big Head: ON" : "Toggle Big Head: OFF");

            if (!boolean) {
                g_needsBigHeadReset.store(true);
                g_bigHeadResetFrames.store(120);
                Toast(env, ctx, OBFUSCATE("Big Head: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Big Head: ON (Kepala Membesar)"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 2: { // Toggle_Fast FireRate (Client-Side)
            g_fastFireRate.store(boolean);
            ModLog("[TOGGLE] Feature #2 [Fast FireRate (Client-Side)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Fast FireRate: ON" : "Toggle Fast FireRate: OFF");

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Fast FireRate: ON (Tembakan Berkecepatan Tinggi - Client Side)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Fast FireRate: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 3: { // Toggle_No Recoil (Client-Side)
            g_noRecoil.store(boolean);
            ModLog("[TOGGLE] Feature #3 [No Recoil (Client-Side)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle No Recoil: ON" : "Toggle No Recoil: OFF");

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("No Recoil: ON (Senjata Tanpa Recoil - Client Side)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("No Recoil: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 4: { // Toggle_Aim Assist (Auto-Lock Radius)
            g_aimAssistBoost.store(boolean);
            ModLog("[TOGGLE] Feature #4 [Aim Assist (Auto-Lock Radius)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Aim Assist Boost: ON" : "Toggle Aim Assist Boost: OFF");

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Aim Assist (Auto-Lock): ON"), ToastLength::LENGTH_SHORT);
                void *mgr = g_activeAimAssistManager.load();
                if (old_AimAssistManager_SetEnabled != nullptr && mgr != nullptr && isPointerReadable(mgr)) {
                    old_AimAssistManager_SetEnabled(mgr, true);
                }
                void *na = g_activeNewAutoAim.load();
                if (old_NewAutoAim_SetEnabled != nullptr && na != nullptr && isPointerReadable(na)) {
                    old_NewAutoAim_SetEnabled(na, true);
                }
                void *ba = g_activeBaseAimAssist.load();
                if (old_BaseAimAssist_SetEnabled != nullptr && ba != nullptr && isPointerReadable(ba)) {
                    old_BaseAimAssist_SetEnabled(ba, true);
                }
            } else {
                Toast(env, ctx, OBFUSCATE("Aim Assist (Auto-Lock): OFF"), ToastLength::LENGTH_SHORT);
                resetAimAssistState();
            }
            break;
        }

        case 5: { // SeekBar_Aim Smoothness (Sticky Lock)_0_100
            g_aimSmoothness.store(value);
            ModLog("[SLIDER] Feature #5 [Aim Smoothness] set to: %d", value);
            setLastAction("Slider Aim Smoothness changed");
            break;
        }

        case 6: { // Toggle_ESP Line (Kuning)
            g_espLine.store(boolean);
            ModLog("[TOGGLE] Feature #6 [ESP Line (Kuning)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle ESP Line: ON" : "Toggle ESP Line: OFF");

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("ESP Line (Kuning): ON"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("ESP Line (Kuning): OFF"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 7: { // Toggle_ESP Box
            g_espBox.store(boolean);
            ModLog("[TOGGLE] Feature #7 [ESP Box] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle ESP Box: ON" : "Toggle ESP Box: OFF");

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("ESP Box: ON"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("ESP Box: OFF"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        default:
            ModLog("[TOGGLE] Unknown Feature #%d changed to: val=%d, bool=%d", featNum, value, static_cast<int>(boolean));
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
    return static_cast<jboolean>(getEntityStats().inGame);
}

void ResetEntityCounters(JNIEnv *env, jobject thiz) {
    resetEntityCounters();
}

__attribute__((constructor))
void lib_main() {
    installCrashHandler();
    ModLog("=================================================================");
    ModLog("        GTA SA FPS MOD MENU LOADED (libModMenu.so)              ");
    ModLog("=================================================================");
    ModLog("[INIT] Native constructor executed. Primary log: %s", DEBUG_LOG_PATHS[0]);

    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}

jfloatArray GetEspData(JNIEnv *env, jclass clazz, jint screenWidth, jint screenHeight) {
    if (!g_espLine.load() && !g_espBox.load()) {
        return nullptr;
    }

    if (espManager == nullptr || espManager->enemies == nullptr) {
        return nullptr;
    }

    void *cam = get_camera();
    if (cam == nullptr || !isUnityObjectAlive(cam)) {
        return nullptr;
    }

    std::vector<float> drawList;
    drawList.reserve(64);

    {
        std::lock_guard<std::mutex> lock(espManager->getMutex());
        auto &enemies = *espManager->enemies;
        void *localPlayer = g_localPlayerNetPlayer.load();

        for (size_t i = 0; i < enemies.size(); i++) {
            enemy_t *enemy = enemies[i];
            if (enemy == nullptr || enemy->object == nullptr) continue;
            void *playerObj = enemy->object;

            if (!isUnityObjectAlive(playerObj)) continue;
            if (localPlayer != nullptr && playerObj == localPlayer) continue;
            if (isNetworkPlayerTeammate(playerObj)) continue;
            if (IsPlayerDead(playerObj)) continue;

            Vector3 rootPos = GetPlayerLocation(playerObj);
            if (rootPos.x == 0.0f && rootPos.y == 0.0f && rootPos.z == 0.0f) continue;

            Vector3 headWorldPos = rootPos + Vector3(0.0f, 1.80f, 0.0f);
            Vector3 feetWorldPos = rootPos;

            void *headBone = nullptr;
            if (isPointerReadable(reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(playerObj) + 0x88))) {
                void *dollsMgr = *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(playerObj) + 0x88);
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

            if (headBone != nullptr) {
                Vector3 hp = getTransformPosition(headBone);
                if (hp.x != 0.0f || hp.y != 0.0f || hp.z != 0.0f) {
                    headWorldPos = hp;
                    feetWorldPos = Vector3(hp.x, rootPos.y, hp.z);
                }
            }

            Vector3 screenHead = WorldToScreenPoint(cam, headWorldPos);
            Vector3 screenFeet = WorldToScreenPoint(cam, feetWorldPos);

            if (screenHead.z < 0.5f) continue; // In front of camera check

            float canvasHeadX = screenHead.x;
            float canvasHeadY = static_cast<float>(screenHeight) - screenHead.y;
            float canvasFeetY = static_cast<float>(screenHeight) - screenFeet.y;

            float topY = canvasHeadY;
            float bottomY = canvasFeetY;
            if (topY > bottomY) std::swap(topY, bottomY);

            float boxHeight = bottomY - topY;
            if (boxHeight < 15.0f) boxHeight = 15.0f;
            topY -= (boxHeight * 0.12f);
            boxHeight = bottomY - topY;
            float boxWidth = boxHeight * 0.55f;

            float boxLeft = canvasHeadX - (boxWidth * 0.5f);
            float boxRight = canvasHeadX + (boxWidth * 0.5f);

            if (boxRight < -200.0f || boxLeft > screenWidth + 200.0f ||
                bottomY < -200.0f || topY > screenHeight + 200.0f) {
                continue;
            }

            float distance = screenHead.z;

            drawList.push_back(canvasHeadX);
            drawList.push_back(topY);
            drawList.push_back(boxLeft);
            drawList.push_back(topY);
            drawList.push_back(boxRight);
            drawList.push_back(bottomY);
            drawList.push_back(distance);
        }
    }

    if (drawList.empty()) {
        return nullptr;
    }

    jfloatArray result = env->NewFloatArray(static_cast<jsize>(drawList.size()));
    if (result != nullptr) {
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(drawList.size()), drawList.data());
    }
    return result;
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
            {OBFUSCATE("GetEspData"), OBFUSCATE("(II)[F"), reinterpret_cast<void *>(GetEspData)},
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
    vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (RegisterMenu(env) != 0)
        return JNI_ERR;
    if (RegisterPreferences(env) != 0)
        return JNI_ERR;
    if (RegisterMain(env) != 0)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}
