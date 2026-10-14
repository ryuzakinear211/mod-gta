#include "CrashHandler.h"
#include "Logger.h"
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <dlfcn.h>
#include <unwind.h>
#include <ucontext.h>

namespace CrashHandler {

    struct BacktraceState {
        void **current;
        void **end;
    };

    static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context *context, void *arg) {
        auto *state = static_cast<BacktraceState*>(arg);
        uintptr_t ip = _Unwind_GetIP(context);
        if (ip != 0) {
            if (state->current < state->end) {
                *state->current++ = reinterpret_cast<void*>(ip);
            } else {
                return _URC_END_OF_STACK;
            }
        }
        return _URC_NO_REASON;
    }

    static size_t captureBacktrace(void **buffer, size_t max) {
        BacktraceState state = { buffer, buffer + max };
        _Unwind_Backtrace(unwindCallback, &state);
        return static_cast<size_t>(state.current - buffer);
    }

    static struct sigaction old_sa_segv;
    static struct sigaction old_sa_abrt;
    static struct sigaction old_sa_bus;
    static struct sigaction old_sa_fpe;
    static struct sigaction old_sa_ill;
    static struct sigaction old_sa_trap;

    static void crashSignalHandler(int sig, siginfo_t *info, void *ucontext) {
        char crashBuf[8192];
        size_t off = 0;

        const char *sigName = "UNKNOWN";
        switch (sig) {
            case SIGSEGV: sigName = "SIGSEGV (Segmentation Fault - Invalid Memory Access)"; break;
            case SIGABRT: sigName = "SIGABRT (Abort)"; break;
            case SIGBUS:  sigName = "SIGBUS (Bus Error)"; break;
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
            "-----------------------------------------------------------------\n",
            timeStr, sig, sigName, info->si_code, info->si_addr, gettid(),
            ModLogger::GetLastAction()
        );

#if defined(__aarch64__)
        auto *uc = static_cast<ucontext_t*>(ucontext);
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

        // Stack backtrace frames
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

        ModLogger::LogRaw(crashBuf, off);

        // Chain to previous handler
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

    void Install() {
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

        LOG_INFO("[CRASH_HANDLER] Native ARM64 Crash Handler installed.");
    }

} // namespace CrashHandler
