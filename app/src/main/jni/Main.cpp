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
static bool g_fastFireRate = false;
static bool g_noRecoil = false;
static bool g_aimAssistBoost = false;
static int g_aimSmoothness = 80; // 0 - 100 slider (default: 80, 100 = 100% sticky lock)
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
        g_lastAction, (int)g_showStatusPanel, (int)g_autoCount, (int)g_bigHead, (int)g_fastFireRate, (int)g_noRecoil, (int)g_aimAssistBoost, g_aimSmoothness
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
static std::mutex g_pipeMutex;

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

    if (g_safetyPipe[1] != -1) {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
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

// Validates whether a managed UnityEngine.Object (Il2CppObject) is alive and has a non-null native C++ pointer
static inline bool isUnityObjectAlive(const void *unityObj) {
    if (unityObj == nullptr || !isPointerReadable(unityObj)) return false;
    uintptr_t addr = (uintptr_t)unityObj;
    if ((addr & 0x7) != 0) return false;

    // Check klass at offset 0x00
    void *klass = *(void **)addr;
    if (klass == nullptr || !isPointerReadable(klass)) return false;

    // Check m_CachedPtr at offset 0x10 (native C++ Unity engine object in libunity.so)
    void *cachedPtr = *(void **)(addr + 0x10);
    if (cachedPtr == nullptr || !isPointerReadable(cachedPtr)) return false;

    return true;
}

// Extracts the native C++ Unity object pointer (m_CachedPtr at offset 0x10) from a managed UnityEngine.Object
static inline void* getNativeUnityPointer(void *unityObj) {
    if (!isUnityObjectAlive(unityObj)) return nullptr;
    return *(void **)((uintptr_t)unityObj + 0x10);
}

// =========================================================================
// Unity Vector2, Vector3 & Pointers
// =========================================================================

struct Vector2 {
    float x;
    float y;
    Vector2() : x(0.0f), y(0.0f) {}
    Vector2(float _x, float _y) : x(_x), y(_y) {}
};

struct Vector3 {
    float x;
    float y;
    float z;
    Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vector3 operator+(const Vector3 &o) const { return Vector3(x + o.x, y + o.y, z + o.z); }
    Vector3 operator-(const Vector3 &o) const { return Vector3(x - o.x, y - o.y, z - o.z); }
    Vector3 operator*(float s) const { return Vector3(x * s, y * s, z * s); }
    Vector3 operator/(float s) const { return (s != 0.0f) ? Vector3(x / s, y / s, z / s) : Vector3(); }
};

struct Quaternion {
    float x;
    float y;
    float z;
    float w;

    Quaternion() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}
    Quaternion(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

    static Quaternion LookRotation(Vector3 forward, Vector3 up = Vector3(0.0f, 1.0f, 0.0f));
};

// =========================================================================
// User-Provided Math Methods: NormalizeAngle, NormalizeAngles & ToEulerRad
// =========================================================================

float NormalizeAngle (float angle){
    while (angle>360)
        angle -= 360;
    while (angle<0)
        angle += 360;
    return angle;
}

Vector3 NormalizeAngles (Vector3 angles){
    angles.x = NormalizeAngle (angles.x);
    angles.y = NormalizeAngle (angles.y);
    angles.z = NormalizeAngle (angles.z);
    return angles;
}

Vector3 ToEulerRad(Quaternion q1){
    float Rad2Deg = 360.0f / ((float)M_PI * 2.0f);

    float sqw = q1.w * q1.w;
    float sqx = q1.x * q1.x;
    float sqy = q1.y * q1.y;
    float sqz = q1.z * q1.z;
    float unit = sqx + sqy + sqz + sqw;
    float test = q1.x * q1.w - q1.y * q1.z;
    Vector3 v;

    if (test>0.4995f*unit) {
        v.y = 2.0f * atan2f (q1.y, q1.x);
        v.x = (float)M_PI / 2.0f;
        v.z = 0;
        return NormalizeAngles(v * Rad2Deg);
    }
    if (test<-0.4995f*unit) {
        v.y = -2.0f * atan2f (q1.y, q1.x);
        v.x = -(float)M_PI / 2.0f;
        v.z = 0;
        return NormalizeAngles (v * Rad2Deg);
    }
    Quaternion q(q1.w, q1.z, q1.x, q1.y);
    v.y = atan2f (2.0f * q.x * q.w + 2.0f * q.y * q.z, 1.0f - 2.0f * (q.z * q.z + q.w * q.w)); // yaw
    v.x = asinf (2.0f * (q.x * q.z - q.w * q.y)); // pitch
    v.z = atan2f (2.0f * q.x * q.y + 2.0f * q.z * q.w, 1.0f - 2.0f * (q.y * q.y + q.z * q.z)); // roll
    return NormalizeAngles (v * Rad2Deg);
}

static void *(*get_transform)(void *) = nullptr;
static void (*set_localScale_Injected)(void *, const Vector3 *) = nullptr;
static void *(*GetBoneTransform)(void *, int) = nullptr;
static void (*get_position_Injected)(void *, Vector3 *) = nullptr;
static void (*GetLocalEulerAngles_Injected)(void *, int, Vector3 *) = nullptr;
static void (*SetLocalEulerAngles_Injected)(void *, const Vector3 *, int) = nullptr;
static void (*WorldToViewportPoint_Injected)(void *, const Vector3 *, int, Vector3 *) = nullptr;
static void (*WorldToScreenPoint_Injected)(void *, const Vector3 *, int, Vector3 *) = nullptr;
static void *(*Camera_get_main)() = nullptr;
static void (*set_rotation_Injected)(void *, const Quaternion *) = nullptr;
static void (*LookRotation_Injected)(const Vector3 *, const Vector3 *, Quaternion *) = nullptr;

static inline Vector3 Vector3Normalize(const Vector3 &v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-6f) {
        float inv = 1.0f / len;
        return Vector3(v.x * inv, v.y * inv, v.z * inv);
    }
    return Vector3(0.0f, 0.0f, 0.0f);
}

static inline Vector3 Vector3Cross(const Vector3 &a, const Vector3 &b) {
    return Vector3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

Quaternion Quaternion::LookRotation(Vector3 forward, Vector3 up) {
    if (LookRotation_Injected != nullptr) {
        Quaternion qOut;
        LookRotation_Injected(&forward, &up, &qOut);
        if (fabsf(qOut.w) > 1e-4f || fabsf(qOut.x) > 1e-4f || fabsf(qOut.y) > 1e-4f || fabsf(qOut.z) > 1e-4f) {
            return qOut;
        }
    }

    Vector3 f = Vector3Normalize(forward);
    if (f.x == 0.0f && f.y == 0.0f && f.z == 0.0f) {
        return Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
    }

    Vector3 r = Vector3Cross(up, f);
    float rLen = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
    if (rLen < 1e-4f) {
        Vector3 fallbackUp = (fabsf(f.y) > 0.9f) ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(0.0f, 1.0f, 0.0f);
        r = Vector3Normalize(Vector3Cross(fallbackUp, f));
    } else {
        r = Vector3(r.x / rLen, r.y / rLen, r.z / rLen);
    }

    Vector3 u = Vector3Cross(f, r);

    float m00 = r.x, m01 = u.x, m02 = f.x;
    float m10 = r.y, m11 = u.y, m12 = f.y;
    float m20 = r.z, m21 = u.z, m22 = f.z;

    float trace = m00 + m11 + m22;
    Quaternion q;
    if (trace > 0.0f) {
        float s = 0.5f / sqrtf(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (m21 - m12) * s;
        q.y = (m02 - m20) * s;
        q.z = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        float s = 2.0f * sqrtf(1.0f + m00 - m11 - m22);
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = 2.0f * sqrtf(1.0f + m11 - m00 - m22);
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = 2.0f * sqrtf(1.0f + m22 - m00 - m11);
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

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
    if (get_position_Injected == nullptr) {
        get_position_Injected = (void (*)(void *, Vector3 *)) getAbsoluteAddress(targetLibName, 0x85B1394);
        ModLog("[UNITY] get_position_Injected pointer: %p", get_position_Injected);
    }
    if (GetLocalEulerAngles_Injected == nullptr) {
        GetLocalEulerAngles_Injected = (void (*)(void *, int, Vector3 *)) getAbsoluteAddress(targetLibName, 0x85B15F4);
        ModLog("[UNITY] GetLocalEulerAngles_Injected pointer: %p", GetLocalEulerAngles_Injected);
    }
    if (SetLocalEulerAngles_Injected == nullptr) {
        SetLocalEulerAngles_Injected = (void (*)(void *, const Vector3 *, int)) getAbsoluteAddress(targetLibName, 0x85B16E8);
        ModLog("[UNITY] SetLocalEulerAngles_Injected pointer: %p", SetLocalEulerAngles_Injected);
    }
    if (WorldToViewportPoint_Injected == nullptr) {
        WorldToViewportPoint_Injected = (void (*)(void *, const Vector3 *, int, Vector3 *)) getAbsoluteAddress(targetLibName, 0x851DF1C);
        ModLog("[UNITY] WorldToViewportPoint_Injected pointer: %p", WorldToViewportPoint_Injected);
    }
    if (WorldToScreenPoint_Injected == nullptr) {
        WorldToScreenPoint_Injected = (void (*)(void *, const Vector3 *, int, Vector3 *)) getAbsoluteAddress(targetLibName, 0x851DE04);
        ModLog("[UNITY] WorldToScreenPoint_Injected pointer: %p", WorldToScreenPoint_Injected);
    }
    if (Camera_get_main == nullptr) {
        Camera_get_main = (void *(*)()) getAbsoluteAddress(targetLibName, 0x851ED38);
        ModLog("[UNITY] Camera_get_main pointer: %p", Camera_get_main);
    }
    if (set_rotation_Injected == nullptr) {
        set_rotation_Injected = (void (*)(void *, const Quaternion *)) getAbsoluteAddress(targetLibName, 0x85B1E5C);
        ModLog("[UNITY] set_rotation_Injected pointer: %p", set_rotation_Injected);
    }
    if (LookRotation_Injected == nullptr) {
        LookRotation_Injected = (void (*)(const Vector3 *, const Vector3 *, Quaternion *)) getAbsoluteAddress(targetLibName, 0x8588584);
        ModLog("[UNITY] LookRotation_Injected pointer: %p", LookRotation_Injected);
    }
}

static void safeSetLocalScale(void *transformObj, const Vector3 &scale) {
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || set_localScale_Injected == nullptr) return;
    void *t = transformObj;
    if (get_transform != nullptr) {
        void *resolved = get_transform(transformObj);
        if (resolved != nullptr && isUnityObjectAlive(resolved)) {
            t = resolved;
        }
    }
    void *nativeTrans = getNativeUnityPointer(t);
    if (nativeTrans == nullptr) return;
    set_localScale_Injected(nativeTrans, &scale);
}

static inline bool getTransformLocalEulerAngles(void *transformObj, Vector3 &eulerOut) {
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || GetLocalEulerAngles_Injected == nullptr) return false;
    void *t = transformObj;
    if (get_transform != nullptr) {
        void *resolved = get_transform(transformObj);
        if (resolved != nullptr && isUnityObjectAlive(resolved)) {
            t = resolved;
        }
    }
    void *nativeTrans = getNativeUnityPointer(t);
    if (nativeTrans == nullptr) return false;
    GetLocalEulerAngles_Injected(nativeTrans, 4, &eulerOut);
    return true;
}

static inline void setTransformLocalEulerAngles(void *transformObj, const Vector3 &euler) {
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || SetLocalEulerAngles_Injected == nullptr) return;
    void *t = transformObj;
    if (get_transform != nullptr) {
        void *resolved = get_transform(transformObj);
        if (resolved != nullptr && isUnityObjectAlive(resolved)) {
            t = resolved;
        }
    }
    void *nativeTrans = getNativeUnityPointer(t);
    if (nativeTrans == nullptr) return;
    SetLocalEulerAngles_Injected(nativeTrans, &euler, 4);
}

static inline void setTransformRotation(void *transformObj, const Quaternion &rot) {
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || set_rotation_Injected == nullptr) return;
    void *t = transformObj;
    if (get_transform != nullptr) {
        void *resolved = get_transform(transformObj);
        if (resolved != nullptr && isUnityObjectAlive(resolved)) {
            t = resolved;
        }
    }
    void *nativeTrans = getNativeUnityPointer(t);
    if (nativeTrans == nullptr) return;
    set_rotation_Injected(nativeTrans, &rot);
}

static void processAimAssistLock(void *aimingControl);

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
static void *g_localPlayerTargetibleObject = nullptr;
static void *g_localPlayerNetPlayer = nullptr;
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

    if (isPointerReadable((void *)((uintptr_t)fpc + 0x120))) {
        void *tObj = *(void **)((uintptr_t)fpc + 0x120);
        if (tObj != nullptr && isUnityObjectAlive(tObj)) {
            g_localPlayerTargetibleObject = tObj;
            if (isPointerReadable((void *)((uintptr_t)tObj + 0xD8))) {
                void *np = *(void **)((uintptr_t)tObj + 0xD8);
                if (np != nullptr && isUnityObjectAlive(np)) {
                    g_localPlayerNetPlayer = np;
                }
            }
        }
    }

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
    static ObscuredFloat s_cachedZeroVal = {0};
    static bool s_hasCachedZero = false;
    if (!s_hasCachedZero && actk_op_Implicit_Float != nullptr) {
        s_cachedZeroVal = actk_op_Implicit_Float(0.0f);
        s_hasCachedZero = true;
    }
    if (s_hasCachedZero) {
        if (isPointerReadable((void *)((uintptr_t)g_localPlayerShooter + 0x18C))) {
            memcpy((void *)((uintptr_t)g_localPlayerShooter + 0x18C), &s_cachedZeroVal, sizeof(ObscuredFloat));
        }
        if (isPointerReadable((void *)((uintptr_t)g_localPlayerShooter + 0x1B0))) {
            memcpy((void *)((uintptr_t)g_localPlayerShooter + 0x1B0), &s_cachedZeroVal, sizeof(ObscuredFloat));
        }
    }

    // 3. Keep clip ammo filled during high-speed firing
    if (set_ClipAmmo != nullptr) {
        set_ClipAmmo(g_localPlayerShooter, 999);
    }
}

// =========================================================================
// Client-Side Fast FireRate Hooks (Strictly Client-Only)
// =========================================================================

static void applyNoRecoilMemoryEdits(void *fpc) {
    if (fpc == nullptr || !isPointerReadable(fpc)) return;

    // 1. AnimController at offset 0xB8
    if (isPointerReadable((void *)((uintptr_t)fpc + 0xB8))) {
        void *animCtrl = *(void **)((uintptr_t)fpc + 0xB8);
        if (animCtrl != nullptr && isPointerReadable(animCtrl)) {
            // AnimEffectList at offset 0x30
            if (isPointerReadable((void *)((uintptr_t)animCtrl + 0x30))) {
                void *animList = *(void **)((uintptr_t)animCtrl + 0x30);
                if (animList != nullptr && isPointerReadable(animList)) {
                    // AttackAnimEffect at offset 0x38
                    if (isPointerReadable((void *)((uintptr_t)animList + 0x38))) {
                        void *attackAnim = *(void **)((uintptr_t)animList + 0x38);
                        if (attackAnim != nullptr && isPointerReadable(attackAnim)) {
                            // Zero current weapon/camera recoil vectors
                            // 0x1F0: _currentWeaponRecoilPosition (Vector3)
                            // 0x1FC: _currentWeaponRecoilRotation (Vector3)
                            // 0x208: _currentCameraRecoilRotation (Vector3)
                            // 0x214: _weaponRotationOutput (Vector3)
                            // 0x220: _cameraRotationOutput (Vector3)
                            if (isPointerReadable((void *)((uintptr_t)attackAnim + 0x1F0))) {
                                memset((void *)((uintptr_t)attackAnim + 0x1F0), 0, 0x22C - 0x1F0);
                            }
                            // 0x244: _sumShift (Vector2)
                            if (isPointerReadable((void *)((uintptr_t)attackAnim + 0x244))) {
                                *(float *)((uintptr_t)attackAnim + 0x244) = 0.0f;
                                *(float *)((uintptr_t)attackAnim + 0x248) = 0.0f;
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
        if (g_fastFireRate) {
            applyPlayerWeaponMemoryEdits();
        } else if (g_hasOrigShootAction && g_localPlayerShooter != nullptr && isPointerReadable(g_localPlayerShooter)) {
            // Restore normal fire mode safely on Unity thread
            if (isPointerReadable((void *)((uintptr_t)g_localPlayerShooter + 0x150))) {
                *(int *)((uintptr_t)g_localPlayerShooter + 0x150) = g_origShootAction;
                g_hasOrigShootAction = false;
            }
        }
        if (g_noRecoil) {
            applyNoRecoilMemoryEdits(instance);
        }
        if (g_aimAssistBoost) {
            void *aimingControl = nullptr;
            if (isPointerReadable((void *)((uintptr_t)instance + 0xF0))) {
                aimingControl = *(void **)((uintptr_t)instance + 0xF0);
            }
            if (aimingControl != nullptr && isUnityObjectAlive(aimingControl)) {
                processAimAssistLock(aimingControl);
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

// =========================================================================
// Client-Side No Recoil Hooks (100% Recoil & Aim Kick Elimination)
// =========================================================================

// 1. FirstPersonController.RecoilShift: RVA 0x40A4420
void (*old_FPC_Recoil)(void *fpc, Vector2 recoil, float smooth, float returnSpeed, float resistance) = nullptr;
void hook_FPC_Recoil(void *fpc, Vector2 recoil, float smooth, float returnSpeed, float resistance) {
    if (g_noRecoil && (fpc == g_localPlayerFPC || fpc != nullptr)) {
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
    if (g_noRecoil) {
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
    if (g_noRecoil) {
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
    if (g_noRecoil) {
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
    ModLog("[FIRERATE] Fast FireRate toggle set to: %s (Player shooter: %p)", enable ? "ON (ACTIVE)" : "OFF (INACTIVE)", g_localPlayerShooter);
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
    if (instance == nullptr || !isUnityObjectAlive(instance)) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_networkPlayers[instance] = getCurrentTimeMs();
}

static void onNetworkPlayerDestroy(void *instance) {
    if (instance == nullptr) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_networkPlayers.erase(instance);
}

static void onBotPlayerUpdate(void *instance) {
    if (instance == nullptr || !isUnityObjectAlive(instance)) return;
    std::lock_guard<std::mutex> lock(g_entityMutex);
    g_botPlayers[instance] = getCurrentTimeMs();

    if (isPointerReadable((void *)((uintptr_t)instance + 0x50))) {
        void *netPlayer = *(void **)((uintptr_t)instance + 0x50);
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
    g_localPlayerTargetibleObject = nullptr;
    g_localPlayerNetPlayer = nullptr;
    ModLog("[STATS] Entity counters reset manually.");
}

// =========================================================================
// Safe Client-Side Big Head Logic
// =========================================================================

static uint64_t g_lastBigHeadLogMs = 0;

static void applyBigHeadToNetworkPlayer(void *netPlayer, const Vector3 &scale) {
    if (netPlayer == nullptr || !isUnityObjectAlive(netPlayer)) return;

    // 1. DollsManager at offset 0x88
    if (isPointerReadable((void *)((uintptr_t)netPlayer + 0x88))) {
        void *dollsMgr = *(void **)((uintptr_t)netPlayer + 0x88);
        if (dollsMgr != nullptr && isUnityObjectAlive(dollsMgr)) {
            // Remote player: ThirdPersonController at 0x50
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x50))) {
                void *tpCtrl = *(void **)((uintptr_t)dollsMgr + 0x50);
                if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                    if (isPointerReadable((void *)((uintptr_t)tpCtrl + 0x78))) {
                        void *animator = *(void **)((uintptr_t)tpCtrl + 0x78);
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
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x60))) {
                void *tpBotCtrl = *(void **)((uintptr_t)dollsMgr + 0x60);
                if (tpBotCtrl != nullptr && isUnityObjectAlive(tpBotCtrl)) {
                    if (isPointerReadable((void *)((uintptr_t)tpBotCtrl + 0x78))) {
                        void *animator = *(void **)((uintptr_t)tpBotCtrl + 0x78);
                        if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                            void *headBone = GetBoneTransform(animator, 10);
                            if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                                safeSetLocalScale(headBone, scale);
                            }
                        }
                    }
                }
            }
            // 0x38: FirstPerson DollView (Local player)
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x38))) {
                void *fpDoll = *(void **)((uintptr_t)dollsMgr + 0x38);
                if (fpDoll != nullptr && isUnityObjectAlive(fpDoll) && get_transform != nullptr) {
                    void *t = get_transform(fpDoll);
                    if (t != nullptr && isUnityObjectAlive(t)) safeSetLocalScale(t, scale);
                }
            }
            // 0x48: ThirdPerson DollView (Remote player)
            if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x48))) {
                void *tpDoll = *(void **)((uintptr_t)dollsMgr + 0x48);
                if (tpDoll != nullptr && isUnityObjectAlive(tpDoll) && get_transform != nullptr) {
                    void *t = get_transform(tpDoll);
                    if (t != nullptr && isUnityObjectAlive(t)) safeSetLocalScale(t, scale);
                }
            }
        }
    }

    // 2. Head Hitbox in BodyPointsManager at offset 0xC8
    if (isPointerReadable((void *)((uintptr_t)netPlayer + 0xC8))) {
        void *bpm = *(void **)((uintptr_t)netPlayer + 0xC8);
        if (bpm != nullptr && isUnityObjectAlive(bpm)) {
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
                                            if (bpView != nullptr && isUnityObjectAlive(bpView)) {
                                                if (isPointerReadable((void *)((uintptr_t)bpView + 0x20))) {
                                                    int pointType = *(int *)((uintptr_t)bpView + 0x20);
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
    if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x58))) {
        void *tpCtrl = *(void **)((uintptr_t)botPlayer + 0x58);
        if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
            if (isPointerReadable((void *)((uintptr_t)tpCtrl + 0x78))) {
                void *animator = *(void **)((uintptr_t)tpCtrl + 0x78);
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
    if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x28))) {
        void *botLook = *(void **)((uintptr_t)botPlayer + 0x28);
        if (botLook != nullptr && isUnityObjectAlive(botLook)) {
            if (isPointerReadable((void *)((uintptr_t)botLook + 0x28))) {
                void *headBone = *(void **)((uintptr_t)botLook + 0x28);
                if (headBone != nullptr && isUnityObjectAlive(headBone)) {
                    safeSetLocalScale(headBone, scale);
                }
            }
        }
    }

    // 2. NetworkPlayer at offset 0x50
    if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x50))) {
        void *netPlayer = *(void **)((uintptr_t)botPlayer + 0x50);
        if (netPlayer != nullptr && isUnityObjectAlive(netPlayer)) {
            applyBigHeadToNetworkPlayer(netPlayer, scale);
        }
    }
}

// =========================================================================
// Advanced Bone-Lock Aim Assist System (dump.cs Hooked & Smoothness Sticky)
// - Filters out teammates/allies so aim assist never targets friendly units
// - Scans active real players (NetworkPlayer) & AI bots (BotPlayer)
// - Detects Head Bone (HumanBodyBones.Head = 10) & Body/Chest Bone (9 / 8)
// - When crosshair is close to a bone, automatically locks/glues to the bone
// - Smoothness Slider (0-100):
//     * Slider = 100 (Mentok): 100% GLUED TO BONE ("selalu lengket") without delay
//     * Slider < 100: Silky-smooth interpolation curve
// - Compatible with Big Head: head bone scale & proximity locks headshots effortlessly
// - Synchronizes with AimingControl nodes (_azimuthNode, _elevationNode, _target)
// =========================================================================

static bool (*get_AllyObjectToogle)(void *) = nullptr;
static bool (*get_IsAutoAimAllowed)(void *) = nullptr;
static void *(*NetworkPlayer_GetTargetibleObject)(void *) = nullptr;
static void *(*BotPlayer_GetTargetibleObject)(void *) = nullptr;
static void (*AimingControl_SetTargetibleObject)(void *, void *) = nullptr;
static void (*AimingControl_AddDelta)(void *, Vector2) = nullptr;
static bool (*NetworkPlayer_IsTeammate)(void *, void *) = nullptr;

static void initAimAssistPointers() {
    if (get_AllyObjectToogle == nullptr) {
        get_AllyObjectToogle = (bool (*)(void *)) getAbsoluteAddress(targetLibName, 0x4DED164);
        ModLog("[AIM_ASSIST] TargetibleObjectCustomSettings.get_AllyObjectToogle pointer: %p", get_AllyObjectToogle);
    }
    if (get_IsAutoAimAllowed == nullptr) {
        get_IsAutoAimAllowed = (bool (*)(void *)) getAbsoluteAddress(targetLibName, 0x4DED124);
        ModLog("[AIM_ASSIST] TargetibleObjectCustomSettings.get_IsAutoAimAllowed pointer: %p", get_IsAutoAimAllowed);
    }
    if (NetworkPlayer_GetTargetibleObject == nullptr) {
        NetworkPlayer_GetTargetibleObject = (void *(*)(void *)) getAbsoluteAddress(targetLibName, 0x42D1FBC);
        ModLog("[AIM_ASSIST] NetworkPlayer.GetTargetibleObject pointer: %p", NetworkPlayer_GetTargetibleObject);
    }
    if (BotPlayer_GetTargetibleObject == nullptr) {
        BotPlayer_GetTargetibleObject = (void *(*)(void *)) getAbsoluteAddress(targetLibName, 0x444A050);
        ModLog("[AIM_ASSIST] BotPlayer.GetTargetibleObject pointer: %p", BotPlayer_GetTargetibleObject);
    }
    if (AimingControl_SetTargetibleObject == nullptr) {
        AimingControl_SetTargetibleObject = (void (*)(void *, void *)) getAbsoluteAddress(targetLibName, 0x4D287F8);
        ModLog("[AIM_ASSIST] AimingControl.SetTargetibleObject pointer: %p", AimingControl_SetTargetibleObject);
    }
    if (AimingControl_AddDelta == nullptr) {
        AimingControl_AddDelta = (void (*)(void *, Vector2)) getAbsoluteAddress(targetLibName, 0x4D281AC);
        ModLog("[AIM_ASSIST] AimingControl.AddDelta pointer: %p", AimingControl_AddDelta);
    }
    if (NetworkPlayer_IsTeammate == nullptr) {
        NetworkPlayer_IsTeammate = (bool (*)(void *, void *)) getAbsoluteAddress(targetLibName, 0x42CE9CC);
        ModLog("[AIM_ASSIST] NetworkPlayer.IsTeammate pointer: %p", NetworkPlayer_IsTeammate);
    }
}

// Check if a TargetibleObject is an ally / teammate or self
static bool isTargetTeammate(void *targetibleObj) {
    if (targetibleObj == nullptr || !isPointerReadable(targetibleObj)) {
        return false;
    }
    if (!isUnityObjectAlive(targetibleObj)) {
        return true; // Destroyed object -> Ignore as aim target!
    }

    // 1. Check TargetibleObjectCustomSettings at offset 0x90
    if (isPointerReadable((void *)((uintptr_t)targetibleObj + 0x90))) {
        void *customSettings = *(void **)((uintptr_t)targetibleObj + 0x90);
        if (customSettings != nullptr && isUnityObjectAlive(customSettings)) {
            // Direct memory check for backing field <AllyObjectToogle>k__BackingField at offset 0x20
            if (isPointerReadable((void *)((uintptr_t)customSettings + 0x20))) {
                bool isAllyField = *(bool *)((uintptr_t)customSettings + 0x20);
                if (isAllyField) {
                    return true; // Marked as ally -> Ignore!
                }
            }

            // Direct check on _enemyData at offset 0x28 -> TargetibleObjectCustomSettingsData.IsAutoAimAllowed (0x11)
            if (isPointerReadable((void *)((uintptr_t)customSettings + 0x28))) {
                void *enemyData = *(void **)((uintptr_t)customSettings + 0x28);
                if (enemyData != nullptr && isPointerReadable(enemyData) &&
                    isPointerReadable((void *)((uintptr_t)enemyData + 0x11))) {
                    bool autoAimAllowed = *(bool *)((uintptr_t)enemyData + 0x11);
                    if (!autoAimAllowed) {
                        return true; // Game explicitly disallows auto aim for this object
                    }
                }
            }
        }
    }

    // 2. Ignore self if targetibleObject belongs to local player
    if (g_localPlayerTargetibleObject != nullptr && targetibleObj == g_localPlayerTargetibleObject) {
        return true;
    }

    // 3. NetworkPlayer teammate check
    if (isPointerReadable((void *)((uintptr_t)targetibleObj + 0xD8))) {
        void *targetNetPlayer = *(void **)((uintptr_t)targetibleObj + 0xD8);
        if (targetNetPlayer != nullptr && isPointerReadable(targetNetPlayer)) {
            if (g_localPlayerNetPlayer != nullptr && targetNetPlayer == g_localPlayerNetPlayer) {
                return true;
            }
            if (g_localPlayerNetPlayer != nullptr && NetworkPlayer_IsTeammate != nullptr &&
                isUnityObjectAlive(targetNetPlayer) && isUnityObjectAlive(g_localPlayerNetPlayer)) {
                if (NetworkPlayer_IsTeammate(targetNetPlayer, g_localPlayerNetPlayer)) {
                    return true;
                }
            }
        }
    }

    return false;
}

static inline Vector3 getTransformPosition(void *transformObj) {
    Vector3 pos(0.0f, 0.0f, 0.0f);
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || get_position_Injected == nullptr) return pos;
    void *t = transformObj;
    if (get_transform != nullptr) {
        void *resolved = get_transform(transformObj);
        if (resolved != nullptr && isUnityObjectAlive(resolved)) {
            t = resolved;
        }
    }
    void *nativeTrans = getNativeUnityPointer(t);
    if (nativeTrans == nullptr) return pos;
    get_position_Injected(nativeTrans, &pos);
    return pos;
}

static inline bool worldToViewport(void *cameraObj, const Vector3 &worldPos, Vector3 &viewportPos) {
    if (cameraObj == nullptr || !isUnityObjectAlive(cameraObj) || WorldToViewportPoint_Injected == nullptr) return false;
    void *nativeCam = getNativeUnityPointer(cameraObj);
    if (nativeCam == nullptr) return false;
    WorldToViewportPoint_Injected(nativeCam, &worldPos, 2, &viewportPos); // 2 = Mono
    return (viewportPos.z > 0.1f); // In front of camera
}

static bool isEntityEnemy(void *entityNetPlayer, void *targetibleObj) {
    if (entityNetPlayer != nullptr && isPointerReadable(entityNetPlayer)) {
        // Ignore self
        if (g_localPlayerNetPlayer != nullptr && entityNetPlayer == g_localPlayerNetPlayer) {
            return false;
        }
        // Teammate check via NetworkPlayer method
        if (g_localPlayerNetPlayer != nullptr && NetworkPlayer_IsTeammate != nullptr &&
            isUnityObjectAlive(entityNetPlayer) && isUnityObjectAlive(g_localPlayerNetPlayer)) {
            if (NetworkPlayer_IsTeammate(entityNetPlayer, g_localPlayerNetPlayer)) {
                return false;
            }
        }
    }
    // TargetibleObject check
    if (targetibleObj != nullptr && isTargetTeammate(targetibleObj)) {
        return false;
    }
    return true;
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

    auto evaluateCandidate = [&](void *headTransform, void *bodyTransform, void *targetibleObj, void *netPlayer, void *fallbackEntity) {
        if (!isEntityEnemy(netPlayer, targetibleObj)) return;

        auto checkBone = [&](void *boneTransform, bool isHead, const Vector3 &offset) {
            if (boneTransform == nullptr || !isPointerReadable(boneTransform) || !isUnityObjectAlive(boneTransform)) return;
            Vector3 rawPos = getTransformPosition(boneTransform);
            if (rawPos.x == 0.0f && rawPos.y == 0.0f && rawPos.z == 0.0f) return;
            Vector3 bonePos = rawPos + offset;

            Vector3 aimDir = bonePos - camPos;
            float dist3D = sqrtf(aimDir.x * aimDir.x + aimDir.y * aimDir.y + aimDir.z * aimDir.z);
            if (dist3D < 0.2f || dist3D > 250.0f) return;

            // User-provided method: Quaternion::LookRotation + ToEulerRad + angle.x normalization
            Quaternion boneLook = Quaternion::LookRotation(aimDir, Vector3(0.0f, 1.0f, 0.0f));
            Vector3 boneAngle = ToEulerRad(boneLook);
            if (boneAngle.x >= 275.0f)
                boneAngle.x -= 360.0f;
            if (boneAngle.x <= -275.0f)
                boneAngle.x += 360.0f;

            float dyaw = boneAngle.y - currentYaw;
            while (dyaw > 180.0f) dyaw -= 360.0f;
            while (dyaw < -180.0f) dyaw += 360.0f;

            float dpitch = boneAngle.x - currentPitch;
            while (dpitch > 180.0f) dpitch -= 360.0f;
            while (dpitch < -180.0f) dpitch += 360.0f;

            float angleOffset = sqrtf(dyaw * dyaw + dpitch * dpitch);

            float allowedAngle = maxFovAngle;
            if (g_bigHead && isHead) {
                allowedAngle *= 1.4f; // More forgiving capture for enlarged head
            }

            if (angleOffset <= allowedAngle) {
                float score = angleOffset;
                if (isHead) score *= 0.70f; // Prioritize headshots

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

        // Fallback to entity root transform with human height offsets
        if (headTransform == nullptr && bodyTransform == nullptr && fallbackEntity != nullptr && isUnityObjectAlive(fallbackEntity) && get_transform != nullptr) {
            void *rootTransform = get_transform(fallbackEntity);
            if (rootTransform != nullptr && isUnityObjectAlive(rootTransform)) {
                checkBone(rootTransform, true, Vector3(0.0f, 1.60f, 0.0f));  // Head offset
                checkBone(rootTransform, false, Vector3(0.0f, 1.25f, 0.0f)); // Chest offset
            }
        }
    };

    // 1. Process Real Players (NetworkPlayer)
    for (void *netPlayer : candidateNetPlayers) {
        if (netPlayer == nullptr || !isPointerReadable(netPlayer) || !isUnityObjectAlive(netPlayer)) continue;
        if (g_localPlayerNetPlayer != nullptr && netPlayer == g_localPlayerNetPlayer) continue;

        void *headBone = nullptr;
        void *bodyBone = nullptr;
        void *targetibleObj = nullptr;

        if (NetworkPlayer_GetTargetibleObject != nullptr) {
            targetibleObj = NetworkPlayer_GetTargetibleObject(netPlayer);
            if (targetibleObj != nullptr && !isUnityObjectAlive(targetibleObj)) {
                targetibleObj = nullptr;
            }
        }

        // Check DollsManager (0x88) -> ThirdPersonController -> Animator
        if (isPointerReadable((void *)((uintptr_t)netPlayer + 0x88))) {
            void *dollsMgr = *(void **)((uintptr_t)netPlayer + 0x88);
            if (dollsMgr != nullptr && isUnityObjectAlive(dollsMgr)) {
                void *tpCtrl = nullptr;
                if (isPointerReadable((void *)((uintptr_t)dollsMgr + 0x50))) {
                    tpCtrl = *(void **)((uintptr_t)dollsMgr + 0x50);
                }
                if (tpCtrl == nullptr && isPointerReadable((void *)((uintptr_t)dollsMgr + 0x60))) {
                    tpCtrl = *(void **)((uintptr_t)dollsMgr + 0x60);
                }
                if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                    if (isPointerReadable((void *)((uintptr_t)tpCtrl + 0x78))) {
                        void *animator = *(void **)((uintptr_t)tpCtrl + 0x78);
                        if (animator != nullptr && isUnityObjectAlive(animator) && GetBoneTransform != nullptr) {
                            void *hb = GetBoneTransform(animator, 10); // HumanBodyBones.Head
                            if (hb != nullptr && isUnityObjectAlive(hb)) headBone = hb;
                            void *bb = GetBoneTransform(animator, 9);  // HumanBodyBones.Chest
                            if (bb == nullptr || !isUnityObjectAlive(bb)) {
                                bb = GetBoneTransform(animator, 8); // Spine
                            }
                            if (bb != nullptr && isUnityObjectAlive(bb)) bodyBone = bb;
                        }
                    }
                }
            }
        }

        // Fallback: BodyPointsManager (0xC8)
        if (headBone == nullptr && isPointerReadable((void *)((uintptr_t)netPlayer + 0xC8))) {
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
                                        void *bp = items[i];
                                        if (bp != nullptr && isPointerReadable((void *)((uintptr_t)bp + 0x10))) {
                                            void *bpView = *(void **)((uintptr_t)bp + 0x10);
                                            if (bpView != nullptr && isUnityObjectAlive(bpView) && isPointerReadable((void *)((uintptr_t)bpView + 0x20))) {
                                                int pType = *(int *)((uintptr_t)bpView + 0x20);
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

        // Fallback: TargetibleObject colliders
        if (targetibleObj != nullptr && isUnityObjectAlive(targetibleObj)) {
            if (headBone == nullptr && isPointerReadable((void *)((uintptr_t)targetibleObj + 0x40))) {
                void *col = *(void **)((uintptr_t)targetibleObj + 0x40);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                }
            }
            if (bodyBone == nullptr && isPointerReadable((void *)((uintptr_t)targetibleObj + 0x48))) {
                void *col = *(void **)((uintptr_t)targetibleObj + 0x48);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
                }
            }
            if (headBone == nullptr && isPointerReadable((void *)((uintptr_t)targetibleObj + 0x38))) {
                void *t = *(void **)((uintptr_t)targetibleObj + 0x38);
                if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
            }
        }

        // Fallback: NetworkPlayer transform
        if (headBone == nullptr && bodyBone == nullptr && get_transform != nullptr) {
            void *t = get_transform(netPlayer);
            if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
        }

        evaluateCandidate(headBone, bodyBone, targetibleObj, netPlayer, netPlayer);
    }

    // 2. Process AI Bots (BotPlayer)
    for (void *botPlayer : candidateBotPlayers) {
        if (botPlayer == nullptr || !isPointerReadable(botPlayer) || !isUnityObjectAlive(botPlayer)) continue;

        void *headBone = nullptr;
        void *bodyBone = nullptr;
        void *targetibleObj = nullptr;
        void *netPlayer = nullptr;

        // BotPlayer -> NetworkPlayer at 0x50
        if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x50))) {
            netPlayer = *(void **)((uintptr_t)botPlayer + 0x50);
            if (netPlayer != nullptr) {
                if (!isUnityObjectAlive(netPlayer)) {
                    netPlayer = nullptr;
                } else if (g_localPlayerNetPlayer != nullptr && netPlayer == g_localPlayerNetPlayer) {
                    continue;
                }
            }
        }

        // BotPlayer -> TargetibleObject at 0x60
        if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x60))) {
            targetibleObj = *(void **)((uintptr_t)botPlayer + 0x60);
        }
        if (targetibleObj == nullptr && BotPlayer_GetTargetibleObject != nullptr) {
            targetibleObj = BotPlayer_GetTargetibleObject(botPlayer);
        }
        if (targetibleObj != nullptr && !isUnityObjectAlive(targetibleObj)) {
            targetibleObj = nullptr;
        }

        // BotPlayer -> ThirdPersonController at 0x58
        if (isPointerReadable((void *)((uintptr_t)botPlayer + 0x58))) {
            void *tpCtrl = *(void **)((uintptr_t)botPlayer + 0x58);
            if (tpCtrl != nullptr && isUnityObjectAlive(tpCtrl)) {
                if (isPointerReadable((void *)((uintptr_t)tpCtrl + 0x78))) {
                    void *animator = *(void **)((uintptr_t)tpCtrl + 0x78);
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

        // BotPlayer -> BotPlayerLook at 0x28 -> Transform at 0x28
        if (headBone == nullptr && isPointerReadable((void *)((uintptr_t)botPlayer + 0x28))) {
            void *botLook = *(void **)((uintptr_t)botPlayer + 0x28);
            if (botLook != nullptr && isUnityObjectAlive(botLook)) {
                if (isPointerReadable((void *)((uintptr_t)botLook + 0x28))) {
                    void *t = *(void **)((uintptr_t)botLook + 0x28);
                    if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                }
            }
        }

        // BotPlayer -> TargetibleObject colliders
        if (targetibleObj != nullptr && isUnityObjectAlive(targetibleObj)) {
            if (headBone == nullptr && isPointerReadable((void *)((uintptr_t)targetibleObj + 0x40))) {
                void *col = *(void **)((uintptr_t)targetibleObj + 0x40);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
                }
            }
            if (bodyBone == nullptr && isPointerReadable((void *)((uintptr_t)targetibleObj + 0x48))) {
                void *col = *(void **)((uintptr_t)targetibleObj + 0x48);
                if (col != nullptr && isUnityObjectAlive(col) && get_transform != nullptr) {
                    void *t = get_transform(col);
                    if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
                }
            }
            if (headBone == nullptr && isPointerReadable((void *)((uintptr_t)targetibleObj + 0x38))) {
                void *t = *(void **)((uintptr_t)targetibleObj + 0x38);
                if (t != nullptr && isUnityObjectAlive(t)) headBone = t;
            }
        }

        // Fallback: BotPlayer transform
        if (headBone == nullptr && bodyBone == nullptr && get_transform != nullptr) {
            void *t = get_transform(botPlayer);
            if (t != nullptr && isUnityObjectAlive(t)) bodyBone = t;
        }

        evaluateCandidate(headBone, bodyBone, targetibleObj, netPlayer, botPlayer);
    }

    return bestTarget;
}

static uint64_t g_lastAimAssistProcessMs = 0;
static uint64_t g_lastAimAssistLogMs = 0;

static void processAimAssistLock(void *aimingControl) {
    if (aimingControl == nullptr || !isPointerReadable(aimingControl) || !isUnityObjectAlive(aimingControl)) return;

    uint64_t nowMs = getCurrentTimeMs();
    if (nowMs - g_lastAimAssistProcessMs < 4) {
        return; // Debounce duplicate calls within the same frame (~16ms)
    }
    g_lastAimAssistProcessMs = nowMs;

    // 1. Resolve Aiming Nodes from AimingControl
    void *azimuthNode = nullptr;
    void *elevationNode = nullptr;
    void *cameraObj = nullptr;

    if (isPointerReadable((void *)((uintptr_t)aimingControl + 0x28))) {
        azimuthNode = *(void **)((uintptr_t)aimingControl + 0x28);
    }
    if (isPointerReadable((void *)((uintptr_t)aimingControl + 0x30))) {
        elevationNode = *(void **)((uintptr_t)aimingControl + 0x30);
    }
    if (isPointerReadable((void *)((uintptr_t)aimingControl + 0x20))) {
        cameraObj = *(void **)((uintptr_t)aimingControl + 0x20);
    }

    if (azimuthNode == nullptr || elevationNode == nullptr) return;
    if (!isUnityObjectAlive(azimuthNode) || !isUnityObjectAlive(elevationNode)) return;

    // 2. Read Current Local Euler Angles
    Vector3 azEuler(0.0f, 0.0f, 0.0f);
    Vector3 elEuler(0.0f, 0.0f, 0.0f);

    if (!getTransformLocalEulerAngles(azimuthNode, azEuler) ||
        !getTransformLocalEulerAngles(elevationNode, elEuler)) {
        return;
    }

    float currentYaw = azEuler.y;
    float currentPitch = (elEuler.x > 180.0f) ? (elEuler.x - 360.0f) : elEuler.x;

    // 3. Resolve Camera Position
    Vector3 camPos = getTransformPosition(elevationNode);
    if (camPos.x == 0.0f && camPos.y == 0.0f && camPos.z == 0.0f) {
        if (cameraObj != nullptr && isUnityObjectAlive(cameraObj)) {
            camPos = getTransformPosition(cameraObj);
        }
    }
    if (camPos.x == 0.0f && camPos.y == 0.0f && camPos.z == 0.0f && g_localPlayerFPC != nullptr && isUnityObjectAlive(g_localPlayerFPC)) {
        camPos = getTransformPosition(g_localPlayerFPC) + Vector3(0.0f, 1.6f, 0.0f);
    }

    // 4. Capture Cone in degrees (more generous when Big Head is active)
    float maxFovAngle = g_bigHead ? 42.0f : 32.0f;

    // 5. Search Best Enemy Bone (Player or Bot)
    TargetBoneInfo targetInfo = findBestTargetBone(camPos, currentYaw, currentPitch, maxFovAngle, cameraObj);
    if (!targetInfo.found) {
        // Clear locked target if no enemy is in cone
        if (isPointerReadable((void *)((uintptr_t)aimingControl + 0xC8))) {
            *(void **)((uintptr_t)aimingControl + 0xC8) = nullptr;
        }
        return;
    }

    // 6. Angular Delta Calculation
    float diffYaw = targetInfo.targetYaw - currentYaw;
    while (diffYaw > 180.0f) diffYaw -= 360.0f;
    while (diffYaw < -180.0f) diffYaw += 360.0f;

    float diffPitch = targetInfo.targetPitch - currentPitch;
    while (diffPitch > 180.0f) diffPitch -= 360.0f;
    while (diffPitch < -180.0f) diffPitch += 360.0f;

    // 7. Apply Smoothness & Sticky Lock
    // Slider mentok (100): 100% GLUED TO BONE ("selalu lengket")
    // Slider < 100: Silky-smooth interpolation curve
    int sliderVal = g_aimSmoothness;
    if (sliderVal < 0) sliderVal = 0;
    if (sliderVal > 100) sliderVal = 100;

    float applyYaw = 0.0f;
    float applyPitch = 0.0f;
    float newYaw = 0.0f;
    float newPitch = 0.0f;

    if (sliderVal >= 100) {
        applyYaw = diffYaw;
        applyPitch = diffPitch;
        newYaw = targetInfo.targetYaw;
        newPitch = targetInfo.targetPitch;
    } else {
        float s = (float)sliderVal / 100.0f;
        float smoothFactor = 0.08f + 0.92f * (s * s);
        applyYaw = diffYaw * smoothFactor;
        applyPitch = diffPitch * smoothFactor;
        newYaw = currentYaw + applyYaw;
        newPitch = currentPitch + applyPitch;
    }

    // Read pitch limits from AimingControl if readable
    float minPitch = -85.0f;
    float maxPitch = 85.0f;
    if (isPointerReadable((void *)((uintptr_t)aimingControl + 0x38))) {
        float pMin = *(float *)((uintptr_t)aimingControl + 0x38);
        if (pMin >= -90.0f && pMin < 0.0f) minPitch = pMin;
    }
    if (isPointerReadable((void *)((uintptr_t)aimingControl + 0x3C))) {
        float pMax = *(float *)((uintptr_t)aimingControl + 0x3C);
        if (pMax > 0.0f && pMax <= 90.0f) maxPitch = pMax;
    }

    if (newPitch < minPitch) newPitch = minPitch;
    if (newPitch > maxPitch) newPitch = maxPitch;

    // 8. Update Euler Angles on Azimuth and Elevation Nodes
    azEuler.y = fmodf(newYaw + 360.0f, 360.0f);
    elEuler.x = (newPitch < 0.0f) ? (newPitch + 360.0f) : newPitch;

    setTransformLocalEulerAngles(azimuthNode, azEuler);
    setTransformLocalEulerAngles(elevationNode, elEuler);

    // 9. When slider >= 95, also apply direct world look rotation if direction is valid
    if (sliderVal >= 95) {
        Vector3 aimDir = targetInfo.bonePos - camPos;
        float magSq = aimDir.x * aimDir.x + aimDir.y * aimDir.y + aimDir.z * aimDir.z;
        if (magSq > 0.0001f) {
            Quaternion fullLook = Quaternion::LookRotation(aimDir, Vector3(0.0f, 1.0f, 0.0f));
            setTransformRotation(elevationNode, fullLook);
        }
    }

    // 10. Sync TargetibleObject in AimingControl
    if (targetInfo.targetibleObj != nullptr && isUnityObjectAlive(targetInfo.targetibleObj)) {
        if (AimingControl_SetTargetibleObject != nullptr) {
            AimingControl_SetTargetibleObject(aimingControl, targetInfo.targetibleObj);
        }
        if (isPointerReadable((void *)((uintptr_t)aimingControl + 0xC8))) {
            *(void **)((uintptr_t)aimingControl + 0xC8) = targetInfo.targetibleObj;
        }
    }

    if (nowMs - g_lastAimAssistLogMs > 6000) {
        g_lastAimAssistLogMs = nowMs;
        ModLog("[AIM_ASSIST] Bone Lock Active -> Target: %s | Dist: %.1fm | Angle: %.1f deg | Smoothness: %d%% | TargetYaw: %.2f | TargetPitch: %.2f",
               targetInfo.isHead ? "HEAD BONE" : "BODY BONE",
               targetInfo.dist3D,
               targetInfo.angleOffset,
               sliderVal,
               targetInfo.targetYaw, targetInfo.targetPitch);
    }
}

// 0. AimingControl.Update: RVA 0x4D27294
void (*old_AimingControl_Update)(void *instance) = nullptr;
void hook_AimingControl_Update(void *instance) {
    if (old_AimingControl_Update != nullptr) {
        old_AimingControl_Update(instance);
    }
    if (g_aimAssistBoost && instance != nullptr && isUnityObjectAlive(instance)) {
        processAimAssistLock(instance);
    }
}

static inline float getAimSensitivityFactor() {
    int val = g_aimSmoothness;
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    return (float)val / 100.0f;
}

// 1. StrafeRotationConfig (0x52A6264, 0x52A6210, 0x52A624C, 0x52A65A4, 0x52A6598)
float (*old_StrafeRotationConfig_get_MaxRotationPower)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_MaxRotationPower(void *instance) {
    float power = old_StrafeRotationConfig_get_MaxRotationPower ? old_StrafeRotationConfig_get_MaxRotationPower(instance) : 0.2f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 0.4f + 2.4f * factor;
    }
    return power;
}

float (*old_StrafeRotationConfig_get_FOVAreaMultiplier)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_FOVAreaMultiplier(void *instance) {
    float fov = old_StrafeRotationConfig_get_FOVAreaMultiplier ? old_StrafeRotationConfig_get_FOVAreaMultiplier(instance) : 1.0f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 1.10f + 0.60f * factor;
    }
    return fov;
}

float (*old_StrafeRotationConfig_get_DefaultStateMaxDistance)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_DefaultStateMaxDistance(void *instance) {
    float dist = old_StrafeRotationConfig_get_DefaultStateMaxDistance ? old_StrafeRotationConfig_get_DefaultStateMaxDistance(instance) : 50.0f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 70.0f + 50.0f * factor;
    }
    return dist;
}

float (*old_StrafeRotationConfig_get_ZoomedStateMaxDistance)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_ZoomedStateMaxDistance(void *instance) {
    float dist = old_StrafeRotationConfig_get_ZoomedStateMaxDistance ? old_StrafeRotationConfig_get_ZoomedStateMaxDistance(instance) : 100.0f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 140.0f + 110.0f * factor;
    }
    return dist;
}

float (*old_StrafeRotationConfig_get_FOVPowerMultiplier)(void *instance) = nullptr;
float hook_StrafeRotationConfig_get_FOVPowerMultiplier(void *instance) {
    float fov = old_StrafeRotationConfig_get_FOVPowerMultiplier ? old_StrafeRotationConfig_get_FOVPowerMultiplier(instance) : 1.0f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 1.15f + 0.85f * factor;
    }
    return fov;
}

// 2. SpinSlowdownConfig (0x4600D68, 0x4600BCC, 0x4600BD8, 0x4600C44, 0x4600C5C)
float (*old_SpinSlowdownConfig_get_MaxSlowdownValue)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_MaxSlowdownValue(void *instance) {
    float val = old_SpinSlowdownConfig_get_MaxSlowdownValue ? old_SpinSlowdownConfig_get_MaxSlowdownValue(instance) : 0.5f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 0.65f + 0.30f * factor;
    }
    return val;
}

float (*old_SpinSlowdownConfig_get_FOVAreaMultiplier)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_FOVAreaMultiplier(void *instance) {
    float fov = old_SpinSlowdownConfig_get_FOVAreaMultiplier ? old_SpinSlowdownConfig_get_FOVAreaMultiplier(instance) : 1.0f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 1.10f + 0.50f * factor;
    }
    return fov;
}

float (*old_SpinSlowdownConfig_get_Radius)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_Radius(void *instance) {
    float r = old_SpinSlowdownConfig_get_Radius ? old_SpinSlowdownConfig_get_Radius(instance) : 1.0f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 1.15f + 0.65f * factor;
    }
    return r;
}

float (*old_SpinSlowdownConfig_get_DefaultStateMaxDistance)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_DefaultStateMaxDistance(void *instance) {
    float dist = old_SpinSlowdownConfig_get_DefaultStateMaxDistance ? old_SpinSlowdownConfig_get_DefaultStateMaxDistance(instance) : 50.0f;
    if (g_aimAssistBoost) {
        float factor = getAimSensitivityFactor();
        return 70.0f + 50.0f * factor;
    }
    return dist;
}

float (*old_SpinSlowdownConfig_get_ZoomedStateMaxDistance)(void *instance) = nullptr;
float hook_SpinSlowdownConfig_get_ZoomedStateMaxDistance(void *instance) {
    float dist = old_SpinSlowdownConfig_get_ZoomedStateMaxDistance ? old_SpinSlowdownConfig_get_ZoomedStateMaxDistance(instance) : 100.0f;
    if (g_aimAssistBoost) {
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
    if (!g_aimAssistBoost) return result;

    float assistX = result.x - currentDelta.x;
    float assistY = result.y - currentDelta.y;
    float assistMag = sqrtf(assistX * assistX + assistY * assistY);

    if (assistMag < 0.0001f) {
        return currentDelta;
    }

    float factor = getAimSensitivityFactor();
    float boost = 1.5f + 3.0f * factor;
    assistX *= boost;
    assistY *= boost;

    float maxStep = 8.0f + 16.0f * factor;
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
    if (g_aimAssistBoost) {
        enabled = true;
    }
    if (old_AimAssistManager_SetEnabled != nullptr) {
        old_AimAssistManager_SetEnabled(instance, enabled);
    }
}

// 5. NewAutoAim.SetEnabled (0x421C470)
void (*old_NewAutoAim_SetEnabled)(void *instance, bool enabled) = nullptr;
void hook_NewAutoAim_SetEnabled(void *instance, bool enabled) {
    if (g_aimAssistBoost) {
        enabled = true;
    }
    if (old_NewAutoAim_SetEnabled != nullptr) {
        old_NewAutoAim_SetEnabled(instance, enabled);
    }
}

// 6. BaseAimAssist.SetEnabled (0x4478A30)
void (*old_BaseAimAssist_SetEnabled)(void *instance, bool enabled) = nullptr;
void hook_BaseAimAssist_SetEnabled(void *instance, bool enabled) {
    if (g_aimAssistBoost) {
        enabled = true;
    }
    if (old_BaseAimAssist_SetEnabled != nullptr) {
        old_BaseAimAssist_SetEnabled(instance, enabled);
    }
}

// 7. BaseAimAssist.IsValidTarget (0x4478A70) & NewAutoAim.IsValidTarget (0x421C254)
bool (*old_BaseAimAssist_IsValidTarget)(void *instance, void *targetibleObject) = nullptr;
bool hook_BaseAimAssist_IsValidTarget(void *instance, void *targetibleObject) {
    if (old_BaseAimAssist_IsValidTarget == nullptr) return false;
    if (targetibleObject == nullptr || !isUnityObjectAlive(targetibleObject)) return false;
    bool valid = old_BaseAimAssist_IsValidTarget(instance, targetibleObject);
    if (!valid) return false;

    if (g_aimAssistBoost) {
        if (isTargetTeammate(targetibleObject)) {
            return false; // Ignore teammates!
        }
    }
    return true;
}

bool (*old_NewAutoAim_IsValidTarget)(void *instance, void *targetibleObject) = nullptr;
bool hook_NewAutoAim_IsValidTarget(void *instance, void *targetibleObject) {
    if (old_NewAutoAim_IsValidTarget == nullptr) return false;
    if (targetibleObject == nullptr || !isUnityObjectAlive(targetibleObject)) return false;
    bool valid = old_NewAutoAim_IsValidTarget(instance, targetibleObject);
    if (!valid) return false;

    if (g_aimAssistBoost) {
        if (isTargetTeammate(targetibleObject)) {
            return false; // Ignore teammates!
        }
    }
    return true;
}

// =========================================================================
// Il2Cpp Lifecycle Hooks
// =========================================================================

// NetworkPlayer.Update: RVA 0x42CC8C8
void (*old_NetworkPlayer_Update)(void *instance) = nullptr;
void hook_NetworkPlayer_Update(void *instance) {
    if (instance != nullptr && isUnityObjectAlive(instance)) {
        setLastAction("NetworkPlayer_Update");
        // Always track active network players unconditionally
        onNetworkPlayerUpdate(instance);

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
    } else if (instance != nullptr) {
        onNetworkPlayerDestroy(instance);
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
    if (instance != nullptr && isUnityObjectAlive(instance)) {
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
    if (instance != nullptr && isUnityObjectAlive(instance)) {
        setLastAction("BotPlayer_Update");
        // Always track active bot players unconditionally
        onBotPlayerUpdate(instance);

        if (g_bigHead) {
            applyBigHeadToBotPlayer(instance, BIG_HEAD_SCALE);
        } else if (g_needsBigHeadReset) {
            applyBigHeadToBotPlayer(instance, NORMAL_HEAD_SCALE);
        }
    } else if (instance != nullptr) {
        onBotPlayerDestroy(instance);
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
    initAimAssistPointers();
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

    // Client-Side No Recoil Hooks (Client Only)
    HOOK("0x40A4420", hook_FPC_Recoil, old_FPC_Recoil);
    ModLog("[HOOK] FirstPersonController.RecoilShift (0x40A4420): %s", old_FPC_Recoil ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4D27AB8", hook_AimingControl_Recoil1, old_AimingControl_Recoil1);
    ModLog("[HOOK] AimingControl.RecoilShift1 (0x4D27AB8): %s", old_AimingControl_Recoil1 ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4D27E20", hook_AimingControl_Recoil2, old_AimingControl_Recoil2);
    ModLog("[HOOK] AimingControl.RecoilShift2 (0x4D27E20): %s", old_AimingControl_Recoil2 ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4D284B0", hook_AimingControl_Recoil3, old_AimingControl_Recoil3);
    ModLog("[HOOK] AimingControl.RecoilShift3 (0x4D284B0): %s", old_AimingControl_Recoil3 ? "SUCCESS" : "FAILED/HOOKED");

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

    // Native Aim Assist & Sensitivity Reinforcement Hooks
    HOOK("0x52A6264", hook_StrafeRotationConfig_get_MaxRotationPower, old_StrafeRotationConfig_get_MaxRotationPower);
    ModLog("[HOOK] StrafeRotationConfig.get_MaxRotationPower (0x52A6264): %s", old_StrafeRotationConfig_get_MaxRotationPower ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x52A6210", hook_StrafeRotationConfig_get_FOVAreaMultiplier, old_StrafeRotationConfig_get_FOVAreaMultiplier);
    ModLog("[HOOK] StrafeRotationConfig.get_FOVAreaMultiplier (0x52A6210): %s", old_StrafeRotationConfig_get_FOVAreaMultiplier ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x52A624C", hook_StrafeRotationConfig_get_DefaultStateMaxDistance, old_StrafeRotationConfig_get_DefaultStateMaxDistance);
    ModLog("[HOOK] StrafeRotationConfig.get_DefaultStateMaxDistance (0x52A624C): %s", old_StrafeRotationConfig_get_DefaultStateMaxDistance ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x52A65A4", hook_StrafeRotationConfig_get_ZoomedStateMaxDistance, old_StrafeRotationConfig_get_ZoomedStateMaxDistance);
    ModLog("[HOOK] StrafeRotationConfig.get_ZoomedStateMaxDistance (0x52A65A4): %s", old_StrafeRotationConfig_get_ZoomedStateMaxDistance ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x52A6598", hook_StrafeRotationConfig_get_FOVPowerMultiplier, old_StrafeRotationConfig_get_FOVPowerMultiplier);
    ModLog("[HOOK] StrafeRotationConfig.get_FOVPowerMultiplier (0x52A6598): %s", old_StrafeRotationConfig_get_FOVPowerMultiplier ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4600D68", hook_SpinSlowdownConfig_get_MaxSlowdownValue, old_SpinSlowdownConfig_get_MaxSlowdownValue);
    ModLog("[HOOK] SpinSlowdownConfig.get_MaxSlowdownValue (0x4600D68): %s", old_SpinSlowdownConfig_get_MaxSlowdownValue ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4600BCC", hook_SpinSlowdownConfig_get_FOVAreaMultiplier, old_SpinSlowdownConfig_get_FOVAreaMultiplier);
    ModLog("[HOOK] SpinSlowdownConfig.get_FOVAreaMultiplier (0x4600BCC): %s", old_SpinSlowdownConfig_get_FOVAreaMultiplier ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4600BD8", hook_SpinSlowdownConfig_get_Radius, old_SpinSlowdownConfig_get_Radius);
    ModLog("[HOOK] SpinSlowdownConfig.get_Radius (0x4600BD8): %s", old_SpinSlowdownConfig_get_Radius ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4600C44", hook_SpinSlowdownConfig_get_DefaultStateMaxDistance, old_SpinSlowdownConfig_get_DefaultStateMaxDistance);
    ModLog("[HOOK] SpinSlowdownConfig.get_DefaultStateMaxDistance (0x4600C44): %s", old_SpinSlowdownConfig_get_DefaultStateMaxDistance ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4600C5C", hook_SpinSlowdownConfig_get_ZoomedStateMaxDistance, old_SpinSlowdownConfig_get_ZoomedStateMaxDistance);
    ModLog("[HOOK] SpinSlowdownConfig.get_ZoomedStateMaxDistance (0x4600C5C): %s", old_SpinSlowdownConfig_get_ZoomedStateMaxDistance ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4F0E254", hook_StrafeRotation_Calculate, old_StrafeRotation_Calculate);
    ModLog("[HOOK] StrafeRotationAimAssist.Calculate (0x4F0E254): %s", old_StrafeRotation_Calculate ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x529060C", hook_AimAssistManager_SetEnabled, old_AimAssistManager_SetEnabled);
    ModLog("[HOOK] AimAssistManager.SetEnabled (0x529060C): %s", old_AimAssistManager_SetEnabled ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4D27294", hook_AimingControl_Update, old_AimingControl_Update);
    ModLog("[HOOK] AimingControl.Update (0x4D27294): %s", old_AimingControl_Update ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x421C470", hook_NewAutoAim_SetEnabled, old_NewAutoAim_SetEnabled);
    ModLog("[HOOK] NewAutoAim.SetEnabled (0x421C470): %s", old_NewAutoAim_SetEnabled ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x4478A30", hook_BaseAimAssist_SetEnabled, old_BaseAimAssist_SetEnabled);
    ModLog("[HOOK] BaseAimAssist.SetEnabled (0x4478A30): %s", old_BaseAimAssist_SetEnabled ? "SUCCESS" : "FAILED/HOOKED");

    // Target Filtering Hooks (Ignore Teammates & Allies)
    HOOK("0x4478A70", hook_BaseAimAssist_IsValidTarget, old_BaseAimAssist_IsValidTarget);
    ModLog("[HOOK] BaseAimAssist.IsValidTarget (0x4478A70): %s", old_BaseAimAssist_IsValidTarget ? "SUCCESS" : "FAILED/HOOKED");

    HOOK("0x421C254", hook_NewAutoAim_IsValidTarget, old_NewAutoAim_IsValidTarget);
    ModLog("[HOOK] NewAutoAim.IsValidTarget (0x421C254): %s", old_NewAutoAim_IsValidTarget ? "SUCCESS" : "FAILED/HOOKED");

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
        OBFUSCATE("Category_📊 STATUS & DEBUG INFO"),
        OBFUSCATE("RichTextView_<div style='background-color:#16222F;padding:10px;border:1px solid #00E5FF;border-radius:6px;'><font color='#00FF7F'><b>[ GTA SA FPS MOD MENU ]</b></font><br><font color='#FFFFFF'>• <b>Status Panel Overlay:</b> HUD real-time counter Player & Bot.<br><br>• <b>Big Head:</b> Memperbesar kepala Player & Bot (Client-Side).<br><br>• <b>Fast FireRate (Client-Side):</b> Tembakan senjata berkecepatan tinggi hanya untuk client (player) via dynamic weapon memory & ACTk ObscuredFloat bypass.<br><br>• <b>No Recoil (Client-Side):</b> Menghilangkan hentakan/recoil senjata player 100% (Bidikan lurus tanpa getaran).<br><br>• <b>Aim Assist (Auto-Lock):</b> Hook AimingControl & Camera dari dump.cs. Otomatis mengabaikan rekan tim & langsung mengunci (lock) target saat arah bidikan dekat dengan bone player/bot.<br><br>• <b>Aim Smoothness Slider (0-100):</b> Menyesuaikan kehalusan kuncian. Saat nilai di-mentokkan (100), bidikan akan selalu lengket (100% magnetic sticky lock) pada bone body atau head musuh (seperti Big Head). Nilai rendah memberikan assist natural.<br><br>• <b>Debug Logger:</b> Aktif otomatis ke <i>/storage/0/emulated/Document/mod_gta_debug.log</i></font></div>")
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

        case 2: { // Toggle_Fast FireRate (Client-Side)
            g_fastFireRate = boolean;
            ModLog("[TOGGLE] Feature #2 [Fast FireRate (Client-Side)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Fast FireRate: ON" : "Toggle Fast FireRate: OFF");
            applyFastFireRateToggle(boolean);

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Fast FireRate: ON (Tembakan Berkecepatan Tinggi - Client Side)"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Fast FireRate: OFF (Normal)"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 3: { // Toggle_No Recoil (Client-Side)
            g_noRecoil = boolean;
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
            g_aimAssistBoost = boolean;
            ModLog("[TOGGLE] Feature #4 [Aim Assist (Auto-Lock Radius)] set to: %s", stateStr);
            setLastAction(boolean ? "Toggle Aim Assist Boost: ON" : "Toggle Aim Assist Boost: OFF");

            if (boolean) {
                Toast(env, ctx, OBFUSCATE("Aim Assist (Auto-Lock): ON"), ToastLength::LENGTH_SHORT);
            } else {
                Toast(env, ctx, OBFUSCATE("Aim Assist (Auto-Lock): OFF"), ToastLength::LENGTH_SHORT);
            }
            break;
        }

        case 5: { // SeekBar_Aim Smoothness (Sticky Lock)_0_100
            g_aimSmoothness = value;
            ModLog("[SLIDER] Feature #5 [Aim Smoothness] set to: %d", value);
            setLastAction("Slider Aim Smoothness changed");
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
