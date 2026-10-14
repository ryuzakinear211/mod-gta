#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

namespace CrashHandler {

    // Installs signal handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP
    void Install();

} // namespace CrashHandler

#endif // CRASH_HANDLER_H
