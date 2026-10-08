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
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "KittyMemory/MemoryPatch.h"
#include "Menu/Setup.h"

// Target library for GTA SA FPS (il2cpp 64-bit)
#define targetLibName OBFUSCATE("libil2cpp.so")

#include "Includes/Macros.h"

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

static void setLastAction(const char *action) {
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

static void rawWriteToAllLogs(const char *buffer, size_t len) {
    // 1. Android Logcat
    __android_log_write(ANDROID_LOG_INFO, "Mod_GTA_Debug", buffer);

    // 2. Write to each candidate path
    for (size_t i = 0; i < NUM_LOG_PATHS; i++) {
        const char *path = DEBUG_LOG_PATHS[i];
        ensureDirectoryForPath(path);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buffer, len);
            fsync(fd);
            close(fd);
        }
    }
}

static void ModLog(const char *fmt, ...) {
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
        rawWriteToAllLogs(fullLine, (size_t)lineLen);
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
    BacktraceState *state = (BacktraceState *)arg;
    uintptr_t ip = _Unwind_GetIP(context);
    if (ip != 0) {
        if (state->current < state->end) {
            *state->current++ = (void *)ip;
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

static bool g_showStatusPanel = false;
static bool g_autoCount = false;
static bool g_bigHead = false;
static bool g_autoHeadshot = false;
static bool g_fastFireRate = false;

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
        "Toggle States : StatusPanel=%d, AutoCount=%d, BigHead=%d, AutoHeadshot=%d, FastFireRate=%d\n"
        "-----------------------------------------------------------------\n",
        timeStr, sig, sigName, info->si_code, info->si_addr, gettid(),
        g_lastAction, (int)g_showStatusPanel, (int)g_autoCount, (int)g_bigHead, (int)g_autoHeadshot, (int)g_fastFireRate
    );

#if defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucontext;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;

    Dl_info pcInfo, lrInfo;
    const char *pcLib = (dladdr((void*)pc, &pcInfo) && pcInfo.dli_fname) ? pcInfo.dli_fname : "unknown";
    uintptr_t pcOff = pcInfo.dli_fbase ? (pc - (uintptr_t)pcInfo.dli_fbase) : 0;

    const char *lrLib = (dladdr((void*)lr, &lrInfo) && lrInfo.dli_fname) ? lrInfo.dli_fname : "unknown";
    uintptr_t lrOff = lrInfo.dli_fbase ? (lr - (uintptr_t)lrInfo.dli_fbase) : 0;

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
        (unsigned long long)uc->uc_mcontext.regs[0], (unsigned long long)uc->uc_mcontext.regs[1],
        (unsigned long long)uc->uc_mcontext.regs[2], (unsigned long long)uc->uc_mcontext.regs[3],
        (unsigned long long)uc->uc_mcontext.regs[4], (unsigned long long)uc->uc_mcontext.regs[5],
        (unsigned long long)uc->uc_mcontext.regs[6], (unsigned long long)uc->uc_mcontext.regs[7],
        (unsigned long long)uc->uc_mcontext.regs[8], (unsigned long long)uc->uc_mcontext.regs[9],
        (unsigned long long)uc->uc_mcontext.regs[10], (unsigned long long)uc->uc_mcontext.regs[11],
        (unsigned long long)uc->uc_mcontext.regs[12], (unsigned long long)uc->uc_mcontext.regs[13],
        (unsigned long long)uc->uc_mcontext.regs[14], (unsigned long long)uc->uc_mcontext.regs[15],
        (unsigned long long)uc->uc_mcontext.regs[16], (unsigned long long)uc->uc_mcontext.regs[17],
        (unsigned long long)uc->uc_mcontext.regs[18], (unsigned long long)uc->uc_mcontext.regs[19],
        (unsigned long long)uc->uc_mcontext.regs[20], (unsigned long long)uc->uc_mcontext.regs[21],
        (unsigned long long)uc->uc_mcontext.regs[22], (unsigned long long)uc->uc_mcontext.regs[23],
        (unsigned long long)uc->uc_mcontext.regs[24], (unsigned long long)uc->uc_mcontext.regs[25],
        (unsigned long long)uc->uc_mcontext.regs[26], (unsigned long long)uc->uc_mcontext.regs[27],
        (unsigned long long)uc->uc_mcontext.regs[28], (unsigned long long)uc->uc_mcontext.regs[29],
        (unsigned long long)uc->uc_mcontext.regs[30], (unsigned long long)uc->uc_mcontext.sp,
        (unsigned long long)uc->uc_mcontext.pc
    );
#endif

    // Backtrace Frames
    off += snprintf(crashBuf + off, sizeof(crashBuf) - off, "Backtrace:\n");
    void *btBuffer[32];
    size_t count = captureBacktrace(btBuffer, 32);
    for (size_t i = 0; i < count; i++) {
        uintptr_t addr = (uintptr_t)btBuffer[i];
        Dl_info dlinfo;
        if (dladdr((void*)addr, &dlinfo) && dlinfo.dli_fname) {
            uintptr_t libOffset = dlinfo.dli_fbase ? (addr - (uintptr_t)dlinfo.dli_fbase) : 0;
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
// Memory Safety & Safe Pointer Validation
// =========================================================================

static int g_safetyPipe[2] = {-1, -1};

static void initSafetyPipe() {
    if (g_safetyPipe[0] == -1) {
        if (pipe(g_safetyPipe) == 0) {
            fcntl(g_safetyPipe[0], F_SETFL, O_NONBLOCK);
            fcntl(g_safetyPipe[1], F_SETFL, O_NONBLOCK);
            ModLog("[SAFETY] Memory inspection pipe initialized successfully.");
        } else {
            ModLog("[WARN] Failed to initialize safety pipe: errno=%d", errno);
        }
    }
}

static bool isPointerReadable(const void *ptr) {
    if (ptr == nullptr) return false;
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < 0x10000 || addr >= 0x0080000000000000ULL) return false;
    if ((addr & 0x7) != 0) return false; // Pointers must be 8-byte aligned on aarch64

    if (g_safetyPipe[1] != -1) {
        char dummy;
        while (read(g_safetyPipe[0], &dummy, 1) > 0); // Drain pipe
        ssize_t written = write(g_safetyPipe[1], ptr, 1);
        if (written == 1) {
            read(g_safetyPipe[0], &dummy, 1);
            return true;
        }
        return false;
    }
    return true;
}

// =========================================================================
// Unity Vector3 & Pointers
// =========================================================================

struct Vector3 {
    float x;
    float y;
    float z;
    Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

static void *(*get_transform)(void *) = nullptr;
static void (*set_localScale_Injected)(void *, const Vector3 *) = nullptr;
static void *(*GetBoneTransform)(void *, int) = nullptr;

static void initUnityPointers() {
    setLastAction("initUnityPointers");
    if (get_transform == nullptr) {
        get_transform = (void *(*)(void *)) getAbsoluteAddress(targetLibName, 0x8597C20);
        ModLog("[UNITY] get_transform pointer: %p", get_transform);
    }
    if (set_localScale_Injected == nullptr) {
        set_localScale_Injected = (void (*)(void *, const Vector3 *)) getAbsoluteAddress(targetLibName, 0x85B224C);
        ModLog("[UNITY] set_localScale_Injected pointer: %p", set_localScale_Injected);
    }
    if (GetBoneTransform == nullptr) {
        GetBoneTransform = (void *(*)(void *, int)) getAbsoluteAddress(targetLibName, 0x84EBAB0);
        ModLog("[UNITY] GetBoneTransform pointer: %p", GetBoneTransform);
    }
}

static void safeSetLocalScale(void *transformObj, const Vector3 &scale) {
    if (transformObj == nullptr || set_localScale_Injected == nullptr) return;
    if (!isPointerReadable(transformObj)) return;

    // In Unity, managed Component/Transform has native C++ pointer at offset 0x10 (m_CachedPtr)
    void *nativePtr = nullptr;
    if (isPointerReadable((void *)((uintptr_t)transformObj + 0x10))) {
        nativePtr = *(void **)((uintptr_t)transformObj + 0x10);
    }

    if (nativePtr != nullptr && isPointerReadable(nativePtr)) {
        set_localScale_Injected(nativePtr, &scale);
    } else {
        set_localScale_Injected(transformObj, &scale);
    }
}

// =========================================================================
// Auto Headshot Memory Patch System (100% Headshot Override)
// =========================================================================

struct MemoryPatchItem {
    const char *name;
    uintptr_t rva;
    size_t size;
    const char *patchHex;
    uint8_t origBytes[16];
    bool initialized;
};

static MemoryPatchItem g_autoHeadshotPatches[] = {
    // 1. FirstPersonController.HitBodyPartStore (0x40961B4) -> str wzr, [sp, #0x30] (Force calculated bodyPart to Head = 0)
    {"FPC.HitBodyPartStore", 0x40961B4, 4, "FF3300B9", {0}, false},

    // 2. FirstPersonController.DamageDataBodyPart (0x409657C) -> mov w3, #0 (Force DamageData.ctor bodyPart param to Head = 0)
    {"FPC.DamageDataBodyPart", 0x409657C, 4, "03008052", {0}, false},

    // 3. FirstPersonController.ReportHitBodyPart (0x4096690) -> mov w1, #0 (Force damage network report bodyPart param to Head = 0)
    {"FPC.ReportHitBodyPart", 0x4096690, 4, "01008052", {0}, false},

    // 4. FirstPersonController.HeadshotMultiplierBranch (0x40968B8) -> NOP (Unconditionally apply Headshot Damage Multiplier at 0x24)
    {"FPC.HeadshotMultiplierBranch", 0x40968B8, 4, "1F2003D5", {0}, false},

    // 5. VehicleWeaponBehaviour.DamageData (0x4293794) -> mov w3, #0 (Force vehicle weapon damage to Head)
    {"VehicleWeapon.DamageData", 0x4293794, 4, "03008052", {0}, false},

    // 6. DamageData.get_BodyPart (0x4FC8668) -> mov w0, #0; ret
    {"DamageData.get_BodyPart", 0x4FC8668, 8, "00008052C0035FD6", {0}, false},

    // 7. DamageData..ctor (0x4FC8630) -> str wzr, [x0, #0x44]
    {"DamageData..ctor (store wzr)", 0x4FC8630, 4, "1F4400B9", {0}, false},

    // 8. NetworkPlayer.Damage (0x42CF248) -> mov w8, #0
    {"NetworkPlayer.Damage (force head)", 0x42CF248, 4, "08008052", {0}, false},

    // 9. BodyPoint.get_BodyPointTypes (0x3F2FB54) -> mov w0, #0; ret
    {"BodyPoint.get_BodyPointTypes", 0x3F2FB54, 8, "00008052C0035FD6", {0}, false},

    // 10-22. All other 13 BodyPoint getters returning Head (0)
    {"BodyPoint.0x3F2FAD4", 0x3F2FAD4, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FAF4", 0x3F2FAF4, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FB14", 0x3F2FB14, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FB34", 0x3F2FB34, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FB74", 0x3F2FB74, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FB94", 0x3F2FB94, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FBB4", 0x3F2FBB4, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FBD4", 0x3F2FBD4, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FBF4", 0x3F2FBF4, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FC14", 0x3F2FC14, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FC34", 0x3F2FC34, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FC54", 0x3F2FC54, 8, "00008052C0035FD6", {0}, false},
    {"BodyPoint.0x3F2FC74", 0x3F2FC74, 8, "00008052C0035FD6", {0}, false},
};
static const size_t NUM_HEADSHOT_PATCHES = sizeof(g_autoHeadshotPatches) / sizeof(g_autoHeadshotPatches[0]);

static void initAutoHeadshotPatches() {
    setLastAction("initAutoHeadshotPatches");
    size_t initCount = 0;
    for (size_t i = 0; i < NUM_HEADSHOT_PATCHES; i++) {
        MemoryPatchItem &item = g_autoHeadshotPatches[i];
        uintptr_t absAddr = getAbsoluteAddress(targetLibName, item.rva);
        if (absAddr != 0) {
            KittyMemory::memRead(item.origBytes, (const void *)absAddr, item.size);
            item.initialized = true;
            initCount++;
        } else {
            ModLog("[HEADSHOT] Warning: Failed to get address for %s (RVA 0x%lx)", item.name, item.rva);
        }
    }
    ModLog("[HEADSHOT] Auto Headshot memory patches initialized (%zu/%zu targets ready)", initCount, NUM_HEADSHOT_PATCHES);
}

static void applyAutoHeadshotPatch(bool enable) {
    g_autoHeadshot = enable;
    setLastAction(enable ? "applyAutoHeadshotPatch(ON)" : "applyAutoHeadshotPatch(OFF)");
    int successCount = 0;
    for (size_t i = 0; i < NUM_HEADSHOT_PATCHES; i++) {
        MemoryPatchItem &item = g_autoHeadshotPatches[i];
        uintptr_t absAddr = getAbsoluteAddress(targetLibName, item.rva);
        if (absAddr == 0) continue;

        if (enable) {
            uint8_t patchBuf[16];
            KittyUtils::fromHex(item.patchHex, patchBuf);
            if (KittyMemory::memWrite((void *)absAddr, patchBuf, item.size) == KittyMemory::SUCCESS) {
                __builtin___clear_cache((char *)absAddr, (char *)absAddr + item.size);
                successCount++;
            } else {
                ModLog("[HEADSHOT] Error: Failed to write patch for %s at 0x%lx", item.name, absAddr);
            }
        } else {
            if (item.initialized) {
                if (KittyMemory::memWrite((void *)absAddr, item.origBytes, item.size) == KittyMemory::SUCCESS) {
                    __builtin___clear_cache((char *)absAddr, (char *)absAddr + item.size);
                    successCount++;
                } else {
                    ModLog("[HEADSHOT] Error: Failed to restore patch for %s at 0x%lx", item.name, absAddr);
                }
            }
        }
    }
    ModLog("[HEADSHOT] Auto Headshot %s: %d/%zu patches successfully applied/restored.",
           enable ? "ENABLED (Memory Edit)" : "DISABLED (Restored)", successCount, NUM_HEADSHOT_PATCHES);
}

// =========================================================================
// Fast FireRate System (Dynamic Player Weapon Memory & Client-Side Hooks)
// Target Methods & RVAs mapped from dump.cs:
// - FirstPersonController.Update (0x40A1BF4): Local player lifecycle & memory tracker
// - FirstPersonController.GetCurrentWeapon (0x408FC44): Resolves equipped weapon
// - FirstPersonController.GetShooterBehaviour (0x409188C): Resolves shooter instance
// - FirstPersonController.GetShotInterval1 (0x40906CC): Primary shot interval (ObscuredFloat)
// - FirstPersonController.GetShotInterval2 (0x409F2F8): Secondary shot interval (ObscuredFloat)
// - ShootCoroutine.MoveNext (0x40B93A0): Immediate fire coroutine loop
// - WeaponShooterBehaviour.CanShoot (0x405BBF0): Cooldown availability check
// - WeaponShooterBehaviour.SetCooldown (0x405D084): Cooldown timer setter
// - WeaponShooterBehaviour._shootAction (0x150): Auto fire forced via direct memory edit (0x405C8DC getter is 12-byte stub)
// - WeaponShooterBehaviour.SetClipAmmo (0x405A39C): Ammo replenish
// - WeaponParameters.GetFireRate (0x49D0E90): Weapon RPM / firerate getter
// - WeaponProfile.GetFireRate (0x4A451C0): Weapon profile firerate getter
// - ObscuredFloat.op_Implicit (0x3D6E4DC): Anti-Cheat Toolkit genuine encrypted float
// =========================================================================

static uint64_t getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

struct ObscuredFloat {
    int hash;
    int hiddenValue;
    int currentCryptoKey;
    float fakeValue;
    uint32_t hiddenValueOldByte4;
};

// Dynamic Player Weapon Memory Addresses (Client Only)
static void *g_localPlayerFPC = nullptr;
static void *g_localPlayerWeapon = nullptr;
static void *g_localPlayerShooter = nullptr;
static void *g_localWeaponProfile = nullptr;
static void *g_localWeaponParams = nullptr;
static uint64_t g_lastWeaponLogMs = 0;
static int g_origShootAction = -1;
static bool g_hasOrigShootAction = false;

// Function pointers to game methods from dump.cs
static void* (*get_CurrentWeapon)(void *fpc) = nullptr;                       // 0x408FC44
static void* (*get_ShooterBehaviour)(void *fpc, void *item) = nullptr;        // 0x409188C
static void (*set_ClipAmmo)(void *shooter, int ammo) = nullptr;               // 0x405A39C
static ObscuredFloat (*actk_op_Implicit_Float)(float val) = nullptr;          // 0x3D6E4DC

static bool isClientWeapon(void *instance) {
    if (instance == nullptr) return false;
    if (instance == g_localPlayerShooter || instance == g_localPlayerWeapon) return true;
    if (g_localPlayerFPC != nullptr && isPointerReadable(g_localPlayerFPC)) {
        if (isPointerReadable((void *)((uintptr_t)g_localPlayerFPC + 0x50))) {
            void *equipped = *(void **)((uintptr_t)g_localPlayerFPC + 0x50);
            if (equipped == instance) return true;
        }
    }
    return false;
}

static bool isClientWeaponProfile(void *instance) {
    if (instance == nullptr) return false;
    return (instance == g_localWeaponProfile);
}

static bool isClientWeaponParams(void *instance) {
    if (instance == nullptr) return false;
    return (instance == g_localWeaponParams);
}

static void updateLocalPlayerWeapon(void *fpc) {
    if (fpc == nullptr || !isPointerReadable(fpc)) return;
    g_localPlayerFPC = fpc;

    void *currentWeapon = nullptr;
    if (get_CurrentWeapon != nullptr) {
        currentWeapon = get_CurrentWeapon(fpc);
    }
    if (currentWeapon == nullptr && isPointerReadable((void *)((uintptr_t)fpc + 0x50))) {
        currentWeapon = *(void **)((uintptr_t)fpc + 0x50);
    }

    if (currentWeapon != nullptr && isPointerReadable(currentWeapon)) {
        g_localPlayerWeapon = currentWeapon;

        void *shooter = nullptr;
        if (get_ShooterBehaviour != nullptr) {
            shooter = get_ShooterBehaviour(fpc, currentWeapon);
        }
        if (shooter != nullptr && isPointerReadable(shooter)) {
            g_localPlayerShooter = shooter;
        } else {
            g_localPlayerShooter = nullptr;
        }

        // WeaponProfile at offset 0x80 of ItemBehaviour
        if (isPointerReadable((void *)((uintptr_t)currentWeapon + 0x80))) {
            g_localWeaponProfile = *(void **)((uintptr_t)currentWeapon + 0x80);
        }
        // WeaponParameters at offset 0x88 of ItemBehaviour
        if (isPointerReadable((void *)((uintptr_t)currentWeapon + 0x88))) {
            g_localWeaponParams = *(void **)((uintptr_t)currentWeapon + 0x88);
        }
    }

    uint64_t now = getCurrentTimeMs();
    if (now - g_lastWeaponLogMs > 8000 && g_localPlayerShooter != nullptr) {
        g_lastWeaponLogMs = now;
        ModLog("[WEAPON] Client Weapon Memory Found -> FPC: %p | Weapon: %p | Shooter: %p | Profile: %p | Params: %p",
               g_localPlayerFPC, g_localPlayerWeapon, g_localPlayerShooter, g_localWeaponProfile, g_localWeaponParams);
    }
}

static void applyPlayerWeaponMemoryEdits() {
    if (g_localPlayerShooter == nullptr || !isPointerReadable(g_localPlayerShooter)) return;

    // 1. Force Full Auto (_shootAction at offset 0x150: AUTO = 0, BOLT_ACTION = 1)
    if (isPointerReadable((void *)((uintptr_t)g_localPlayerShooter + 0x150))) {
        int *shootActionPtr = (int *)((uintptr_t)g_localPlayerShooter + 0x150);
        if (!g_hasOrigShootAction) {
            g_origShootAction = *shootActionPtr;
            g_hasOrigShootAction = true;
        }
        *shootActionPtr = 0; // AUTO
    }

    // 2. Zero out internal cooldown ObscuredFloat fields at offset 0x18C & 0x1B0
    if (actk_op_Implicit_Float != nullptr) {
        ObscuredFloat zeroVal = actk_op_Implicit_Float(0.0f);
        if (isPointerReadable((void *)((uintptr_t)g_localPlayerShooter + 0x18C))) {
            memcpy((void *)((uintptr_t)g_localPlayerShooter + 0x18C), &zeroVal, sizeof(ObscuredFloat));
        }
        if (isPointerReadable((void *)((uintptr_t)g_localPlayerShooter + 0x1B0))) {
            memcpy((void *)((uintptr_t)g_localPlayerShooter + 0x1B0), &zeroVal, sizeof(ObscuredFloat));
        }
    }

    // 3. Keep clip ammo filled during high-speed firing
    if (set_ClipAmmo != nullptr) {
        set_ClipAmmo(g_localPlayerShooter, 999);
    }
}

static void resetFpcShotTimers(void *fpc) {
    if (fpc == nullptr || !isPointerReadable(fpc)) return;
    // Reset FPC shot delay timers at 0x88, 0xB0, 0x224
    if (isPointerReadable((void *)((uintptr_t)fpc + 0x88))) {
        *(float *)((uintptr_t)fpc + 0x88) = 0.0f;
    }
    if (isPointerReadable((void *)((uintptr_t)fpc + 0xB0))) {
        *(float *)((uintptr_t)fpc + 0xB0) = 0.0f;
    }
    if (isPointerReadable((void *)((uintptr_t)fpc + 0x224))) {
        *(float *)((uintptr_t)fpc + 0x224) = 0.0f;
    }
}

// =========================================================================
// Client-Side Fast FireRate Hooks (Strictly Client-Only)
// =========================================================================

// 1. FirstPersonController.Update: RVA 0x40A1BF4
void (*old_FirstPersonController_Update)(void *instance) = nullptr;
void hook_FirstPersonController_Update(void *instance) {
    if (instance != nullptr) {
        setLastAction("FirstPersonController_Update");
        updateLocalPlayerWeapon(instance);
        if (g_fastFireRate) {
            applyPlayerWeaponMemoryEdits();
            resetFpcShotTimers(instance);
        }
    }
    if (old_FirstPersonController_Update != nullptr) {
        old_FirstPersonController_Update(instance);
    }
}

// 2. WeaponShooterBehaviour.CanShoot: RVA 0x405BBF0
bool (*old_WeaponShooterBehaviour_CanShoot)(void *instance) = nullptr;
bool hook_WeaponShooterBehaviour_CanShoot(void *instance) {
    if (g_fastFireRate && isClientWeapon(instance)) {
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
    if (g_fastFireRate && isClientWeapon(instance)) {
        return; // Zero cooldown for local weapon
    }
    if (old_WeaponShooterBehaviour_SetCooldown != nullptr) {
        old_WeaponShooterBehaviour_SetCooldown(instance);
    }
}


// 5. FirstPersonController.GetShotInterval1: RVA 0x40906CC
ObscuredFloat (*old_FPC_GetShotInterval1)(void *fpc, void *weapon) = nullptr;
ObscuredFloat hook_FPC_GetShotInterval1(void *fpc, void *weapon) {
    if (g_fastFireRate && (fpc == g_localPlayerFPC || isClientWeapon(weapon))) {
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
    if (g_fastFireRate && (fpc == g_localPlayerFPC || isClientWeapon(weapon))) {
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
    if (g_fastFireRate && instance != nullptr && isPointerReadable(instance)) {
        if (isPointerReadable((void *)((uintptr_t)instance + 0x20))) {
            void *fpc = *(void **)((uintptr_t)instance + 0x20);
            if (fpc == g_localPlayerFPC && fpc != nullptr) {
                // Immediate fire without WaitForSeconds delay
                if (isPointerReadable((void *)((uintptr_t)instance + 0x28))) {
                    *(bool *)((uintptr_t)instance + 0x28) = true;
                }
                if (isPointerReadable((void *)((uintptr_t)instance + 0x2C))) {
                    *(float *)((uintptr_t)instance + 0x2C) = 100.0f;
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
    if (g_fastFireRate && isClientWeaponParams(instance)) {
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
    if (g_fastFireRate && isClientWeaponProfile(instance)) {
        return 3000.0f; // High speed firerate (Client only)
    }
    if (old_WeaponProfile_GetFireRate != nullptr) {
        return old_WeaponProfile_GetFireRate(instance);
    }
    return 600.0f;
}

static void initFastFireRateSystem() {
    setLastAction("initFastFireRateSystem");
    get_CurrentWeapon = (void* (*)(void *)) getAbsoluteAddress(targetLibName, 0x408FC44);
    get_ShooterBehaviour = (void* (*)(void *, void *)) getAbsoluteAddress(targetLibName, 0x409188C);
    set_ClipAmmo = (void (*)(void *, int)) getAbsoluteAddress(targetLibName, 0x405A39C);
    actk_op_Implicit_Float = (ObscuredFloat (*)(float)) getAbsoluteAddress(targetLibName, 0x3D6E4DC);

    ModLog("[FIRERATE] Functions Resolved: GetCurrentWeapon=%p, GetShooterBehaviour=%p, SetClipAmmo=%p, ACTk_OpImplicit=%p",
           get_CurrentWeapon, get_ShooterBehaviour, set_ClipAmmo, actk_op_Implicit_Float);
}

static void applyFastFireRateToggle(bool enable) {
    g_fastFireRate = enable;
    setLastAction(enable ? "applyFastFireRateToggle(ON)" : "applyFastFireRateToggle(OFF)");

    if (enable) {
        applyPlayerWeaponMemoryEdits();
        if (g_localPlayerFPC != nullptr) {
            resetFpcShotTimers(g_localPlayerFPC);
        }
        ModLog("[FIRERATE] Fast FireRate ENABLED (Client-Side Weapon Firerate Active, Shooter=%p)", g_localPlayerShooter);
    } else {
        // Restore original shootAction if known
        if (g_hasOrigShootAction && g_localPlayerShooter != nullptr && isPointerReadable(g_localPlayerShooter)) {
            if (isPointerReadable((void *)((uintptr_t)g_localPlayerShooter + 0x150))) {
                *(int *)((uintptr_t)g_localPlayerShooter + 0x150) = g_origShootAction;
            }
        }
        ModLog("[FIRERATE] Fast FireRate DISABLED (Restored Normal Firerate)");
    }
}

// =========================================================================
// Real-time Player & Bot Tracking System
// =========================================================================

static std::mutex g_entityMutex;
static std::unordered_map<void*, uint64_t> g_networkPlayers; // NetworkPlayer* -> last seen ms
static std::unordered_map<void*, uint64_t> g_botPlayers;     // BotPlayer* -> last seen ms
static std::unordered_map<void*, void*> g_botNetPlayers;     // BotPlayer* -> NetworkPlayer*

static bool g_needsBigHeadReset = false;
static int g_bigHeadResetFrames = 0;

static const Vector3 BIG_HEAD_SCALE(3.0f, 3.0f, 3.0f);
static const Vector3 NORMAL_HEAD_SCALE(1.0f, 1.0f, 1.0f);

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

    if (isPointerReadable((void *)((uintptr_t)instance + 0x50))) {
        void *netPlayer = *(void **)((uintptr_t)instance + 0x50);
        if (netPlayer != nullptr) {
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

struct EntityStats {
    int realPlayerCount;
    int botCount;
    int totalEntities;
    bool inGame;
};

static EntityStats getEntityStats() {
    std::lock_guard<std::mutex> lock(g_entityMutex);
    uint64_t now = getCurrentTimeMs();
    const uint64_t TIMEOUT_MS = 2500;

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
    ModLog("[STATS] Entity counters reset manually.");
}

// =========================================================================
// Safe Client-Side Big Head Logic
// =========================================================================

static uint64_t g_lastBigHeadLogMs = 0;

static void applyBigHeadToNetworkPlayer(void *netPlayer, const Vector3 &scale) {
    if (netPlayer == nullptr || !isPointerReadable(netPlayer)) return;

    // 1. DollsManager at offset 0x88
    if (isPointerReadable((void *)((uintptr_t)netPlayer + 0x88))) {
        void *dollsMgr = *(void **)((uintptr_t)netPlayer + 0x88);
        if (dollsMgr != nullptr && isPointerReadable(dollsMgr)) {
            // Remote player: ThirdPersonController at 0x50
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x50))) {
                void *tpCtrl = *(void **)((uintptr_t)dollsMgr + 0x50);
                if (tpCtrl != nullptr && isPointerReadable(tpCtrl)) {
                    if (isPointerReadable((void *)((uintptr_t)tpCtrl + 0x78))) {
                        void *animator = *(void **)((uintptr_t)tpCtrl + 0x78);
                        if (animator != nullptr && isPointerReadable(animator) && GetBoneTransform != nullptr) {
                            void *headBone = GetBoneTransform(animator, 10); // 10 = HumanBodyBones.Head
                            if (headBone != nullptr && isPointerReadable(headBone)) {
                                safeSetLocalScale(headBone, scale);
                            }
                        }
                    }
                }
            }
            // Bot player doll: ThirdPersonController at 0x60
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x60))) {
                void *tpBotCtrl = *(void **)((uintptr_t)dollsMgr + 0x60);
                if (tpBotCtrl != nullptr && isPointerReadable(tpBotCtrl)) {
                    if (isPointerReadable((void *)((uintptr_t)tpBotCtrl + 0x78))) {
                        void *animator = *(void **)((uintptr_t)tpBotCtrl + 0x78);
                        if (animator != nullptr && isPointerReadable(animator) && GetBoneTransform != nullptr) {
                            void *headBone = GetBoneTransform(animator, 10);
                            if (headBone != nullptr && isPointerReadable(headBone)) {
                                safeSetLocalScale(headBone, scale);
                            }
                        }
                    }
                }
            }
            // 0x38: FirstPerson DollView (Local player)
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x38))) {
                void *fpDoll = *(void **)((uintptr_t)dollsMgr + 0x38);
                if (fpDoll != nullptr && isPointerReadable(fpDoll) && get_transform != nullptr) {
                    void *t = get_transform(fpDoll);
                    if (t != nullptr) safeSetLocalScale(t, scale);
                }
            }
            // 0x48: ThirdPerson DollView (Remote player)
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x48))) {
                void *tpDoll = *(void **)((uintptr_t)dollsMgr + 0x48);
                if (tpDoll != nullptr && isPointerReadable(tpDoll) && get_transform != nullptr) {
                    void *t = get_transform(tpDoll);
                    if (t != nullptr) safeSetLocalScale(t, scale);
                }
            }
        }
    }

    // 2. Head Hitbox in BodyPointsManager at offset 0xC8
    if (isPointerReadable((void *)((uintptr_t)netPlayer + 0xC8))) {
        void *bpm = *(void **)((uintptr_t)netPlayer + 0xC8);
        if (bpm != nullptr && isPointerReadable(bpm)) {
            if (isPointerReadable((void *)((uintptr_t)bpm + 0x20))) {
                void *bodyPointsArr = *(void **)((uintptr_t)bpm + 0x20);
                if (bodyPointsArr != nullptr && isPointerReadable(bodyPointsArr)) {
                    if (isPointerReadable((void *)((uintptr_t)bodyPointsArr + 0x18))) {
                        uintptr_t length = *(uintptr_t *)((uintptr_t)bodyPointsArr + 0x18);
                        if (length > 0 && length < 32) {
                            void **items = (void **)((uintptr_t)bodyPointsArr + 0x20);
                            for (uintptr_t i = 0; i < length; i++) {
                                if (isPointerReadable(&items[i])) {
                                    void *bodyPoint = items[i];
                                    if (bodyPoint != nullptr && isPointerReadable(bodyPoint)) {
                                        if (isPointerReadable((void *)((uintptr_t)bodyPoint + 0x10))) {
                                            void *bpView = *(void **)((uintptr_t)bodyPoint + 0x10);
                                            if (bpView != nullptr && isPointerReadable(bpView)) {
                                                if (isPointerReadable((void *)((uintptr_t)bpView + 0x20))) {
                                                    int pointType = *(int *)((uintptr_t)bpView + 0x20);
                                                    if (pointType == 0) { // Head
                                                        if (get_transform != nullptr) {
                                                            void *headHitbox = get_transform(bpView);
                                                            if (headHitbox != nullptr) {
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
    if (botPlayer == nullptr || !isPointerReadable(botPlayer)) return;

    // 0. ThirdPersonController at offset 0x58
    if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x58))) {
        void *tpCtrl = *(void **)((uintptr_t)botPlayer + 0x58);
        if (tpCtrl != nullptr && isPointerReadable(tpCtrl)) {
            if (isPointerReadable((void *)((uintptr_t)tpCtrl + 0x78))) {
                void *animator = *(void **)((uintptr_t)tpCtrl + 0x78);
                if (animator != nullptr && isPointerReadable(animator) && GetBoneTransform != nullptr) {
                    void *headBone = GetBoneTransform(animator, 10);
                    if (headBone != nullptr && isPointerReadable(headBone)) {
                        safeSetLocalScale(headBone, scale);
                    }
                }
            }
        }
    }

    // 1. BotPlayerLook at offset 0x28 -> Transform at offset 0x28 (Head look bone)
    if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x28))) {
        void *botLook = *(void **)((uintptr_t)botPlayer + 0x28);
        if (botLook != nullptr && isPointerReadable(botLook)) {
            if (isPointerReadable((void *)((uintptr_t)botLook + 0x28))) {
                void *headBone = *(void **)((uintptr_t)botLook + 0x28);
                if (headBone != nullptr && isPointerReadable(headBone)) {
                    safeSetLocalScale(headBone, scale);
                }
            }
        }
    }

    // 2. NetworkPlayer at offset 0x50
    if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x50))) {
        void *netPlayer = *(void **)((uintptr_t)botPlayer + 0x50);
        if (netPlayer != nullptr && isPointerReadable(netPlayer)) {
            applyBigHeadToNetworkPlayer(netPlayer, scale);
        }
    }
}

// =========================================================================
// Il2Cpp Lifecycle Hooks
// =========================================================================

// NetworkPlayer.Update: RVA 0x42CC8C8
void (*old_NetworkPlayer_Update)(void *instance) = nullptr;
void hook_NetworkPlayer_Update(void *instance) {
    if (instance != nullptr) {
        setLastAction("NetworkPlayer_Update");
        if (g_autoCount) {
            onNetworkPlayerUpdate(instance);
        }
        if (g_bigHead) {
            applyBigHeadToNetworkPlayer(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset) {
            applyBigHeadToNetworkPlayer(instance, NORMAL_HEAD_SCALE);
            if (g_bigHeadResetFrames > 0) {
                g_bigHeadResetFrames--;
                if (g_bigHeadResetFrames == 0) {
                    g_needsBigHeadReset = false;
                    ModLog("[BIG_HEAD] Reset frames completed. Restored normal head scale.");
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
        setLastAction("NetworkPlayer_OnDestroy");
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
        setLastAction("BotPlayer_Start");
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
        setLastAction("BotPlayer_Update");
        if (g_autoCount) {
            onBotPlayerUpdate(instance);
        }
        if (g_bigHead) {
            applyBigHeadToBotPlayer(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset) {
            applyBigHeadToBotPlayer(instance, NORMAL_HEAD_SCALE);
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
    }
    if (old_BotPlayer_OnDestroy != nullptr) {
        old_BotPlayer_OnDestroy(instance);
    }
}

// Background thread waiting for libil2cpp.so
void *hack_thread(void *) {
    ModLog("[THREAD] hack_thread started for GTA SA FPS. Waiting for %s...", (const char *)targetLibName);
    setLastAction("Waiting for libil2cpp.so");

    do {
        sleep(1);
    } while (!isLibraryLoaded(targetLibName));

    uintptr_t base = findLibrary(targetLibName);
    ModLog("[THREAD] %s loaded successfully at base: 0x%lx", (const char *)targetLibName, base);
    setLastAction("libil2cpp.so loaded");

    initSafetyPipe();
    initUnityPointers();
    initAutoHeadshotPatches();
    if (g_autoHeadshot) {
        applyAutoHeadshotPatch(true);
    }
    initFastFireRateSystem();
    if (g_fastFireRate) {
        applyFastFireRateToggle(true);
    }

#if defined(__aarch64__)
    ModLog("[HOOK] Installing hooks on libil2cpp.so (arm64-v8a)...");

    // Client-Side Fast FireRate & Weapon Hooks (Client Only)
    HOOK("0x40A1BF4", hook_FirstPersonController_Update, old_FirstPersonController_Update);
    ModLog("[HOOK] FirstPersonController.Update (0x40A1BF4): %s", old_FirstPersonController_Update ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x405BBF0", hook_WeaponShooterBehaviour_CanShoot, old_WeaponShooterBehaviour_CanShoot);
    ModLog("[HOOK] WeaponShooterBehaviour.CanShoot (0x405BBF0): %s", old_WeaponShooterBehaviour_CanShoot ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x405D084", hook_WeaponShooterBehaviour_SetCooldown, old_WeaponShooterBehaviour_SetCooldown);
    ModLog("[HOOK] WeaponShooterBehaviour.SetCooldown (0x405D084): %s", old_WeaponShooterBehaviour_SetCooldown ? "SUCCESS" : "FAILED/HOOKED");


    HOOK("0x40906CC", hook_FPC_GetShotInterval1, old_FPC_GetShotInterval1);
    ModLog("[HOOK] FirstPersonController.GetShotInterval1 (0x40906CC): %s", old_FPC_GetShotInterval1 ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x409F2F8", hook_FPC_GetShotInterval2, old_FPC_GetShotInterval2);
    ModLog("[HOOK] FirstPersonController.GetShotInterval2 (0x409F2F8): %s", old_FPC_GetShotInterval2 ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x40B93A0", hook_ShootCoroutine_MoveNext, old_ShootCoroutine_MoveNext);
    ModLog("[HOOK] ShootCoroutine.MoveNext (0x40B93A0): %s", old_ShootCoroutine_MoveNext ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x49D0E90", hook_WeaponParams_GetFireRate, old_WeaponParams_GetFireRate);
    ModLog("[HOOK] WeaponParameters.GetFireRate (0x49D0E90): %s", old_WeaponParams_GetFireRate ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4A451C0", hook_WeaponProfile_GetFireRate, old_WeaponProfile_GetFireRate);
    ModLog("[HOOK] WeaponProfile.GetFireRate (0x4A451C0): %s", old_WeaponProfile_GetFireRate ? "SUCCESS" : "FAILED/HOOKED");

    // NetworkPlayer hooks
    HOOK("0x42CC8C8", hook_NetworkPlayer_Update, old_NetworkPlayer_Update);
    ModLog("[HOOK] NetworkPlayer.Update (0x42CC8C8): %s", old_NetworkPlayer_Update ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x42C9540", hook_NetworkPlayer_OnDestroy, old_NetworkPlayer_OnDestroy);
    ModLog("[HOOK] NetworkPlayer.OnDestroy (0x42C9540): %s", old_NetworkPlayer_OnDestroy ? "SUCCESS" : "FAILED/HOOKED");

    // BotPlayer hooks
    HOOK("0x44493F4", hook_BotPlayer_Start, old_BotPlayer_Start);
    ModLog("[HOOK] BotPlayer.Start (0x44493F4): %s", old_BotPlayer_Start ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x444A310", hook_BotPlayer_Update, old_BotPlayer_Update);
    ModLog("[HOOK] BotPlayer.Update (0x444A310): %s", old_BotPlayer_Update ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4449EB0", hook_BotPlayer_OnDestroy, old_BotPlayer_OnDestroy);
    ModLog("[HOOK] BotPlayer.OnDestroy (0x4449EB0): %s", old_BotPlayer_OnDestroy ? "SUCCESS" : "FAILED/HOOKED");

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
        OBFUSCATE("Toggle_Auto Headshot"),        // featNum 2
        OBFUSCATE("Toggle_Fast FireRate (Client-Side)"), // featNum 3
        OBFUSCATE("Category_📊 STATUS & DEBUG INFO"),
        OBFUSCATE("RichTextView_<div style='background-color:#16222F;padding:10px;border:1px solid #00E5FF;border-radius:6px;'><font color='#00FF7F'><b>[ GTA SA FPS MOD MENU ]</b></font><br><font color='#FFFFFF'>• <b>Status Panel Overlay:</b> HUD real-time counter Player & Bot.<br><br>• <b>Big Head:</b> Memperbesar kepala Player & Bot (Client-Side).<br><br>• <b>Auto Headshot:</b> Memory edit 100% damage langsung tembus Headshot.<br><br>• <b>Fast FireRate (Client-Side):</b> Tembakan senjata berkecepatan tinggi hanya untuk client (player) via dynamic weapon memory & ACTk ObscuredFloat bypass.<br><br>• <b>Debug Logger:</b> Aktif otomatis ke <i>/storage/0/emulated/Document/mod_gta_debug.log</i></font></div>")
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

    const char *stateStr = boolean ? "ON (ACTIVE)" : "OFF (INACTIVE)";

    switch (featNum) {
        case 0: { // Toggle_Status Panel Overlay (Auto Count Player & Bot)
            g_showStatusPanel = boolean;
            g_autoCount = boolean;
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
            g_bigHead = boolean;
            ModLog("[TOGGLE] Feature #1 [Big Head (Client-Side)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Big Head: ON" : "Toggle Big Head: OFF");

            if (!boolean) {
                g_needsBigHeadReset = true;
                g_bigHeadResetFrames = 120;
                Toast(env, ctx, OBFUSCATE("Big Head: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Big Head: ON (Kepala Membesar)"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 2: { // Toggle_Auto Headshot (Memory Edit)
            g_autoHeadshot = boolean;
            ModLog("[TOGGLE] Feature #2 [Auto Headshot (Memory Edit)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Auto Headshot: ON" : "Toggle Auto Headshot: OFF");
            applyAutoHeadshotPatch(boolean);

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Auto Headshot: ON (100% Headshot Memory Edit)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Auto Headshot: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 3: { // Toggle_Fast FireRate (Client-Side)
            g_fastFireRate = boolean;
            ModLog("[TOGGLE] Feature #3 [Fast FireRate (Client-Side)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Fast FireRate: ON" : "Toggle Fast FireRate: OFF");
            applyFastFireRateToggle(boolean);

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Fast FireRate: ON (Tembakan Berkecepatan Tinggi - Client Side)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Fast FireRate: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        default:
            ModLog("[TOGGLE] Unknown Feature #%d changed to: val=%d, bool=%d", featNum, value, (int)boolean);
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
    installCrashHandler();
    ModLog("=================================================================");
    ModLog("        GTA SA FPS MOD MENU LOADED (libModMenu.so)              ");
    ModLog("=================================================================");
    ModLog("[INIT] Native constructor executed. Primary log: %s", DEBUG_LOG_PATHS[0]);

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
