#include "MemoryHelper.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "../KittyMemory/KittyMemory.h"

#if defined(__aarch64__)
#include "../And64InlineHook/And64InlineHook.hpp"
#else
#include "../Substrate/SubstrateHook.h"
#endif

namespace Memory {

    static uintptr_t s_libBase = 0;

    uintptr_t GetLibBase() {
        if (s_libBase != 0) {
            return s_libBase;
        }

        FILE *fp = fopen("/proc/self/maps", "rt");
        if (fp == nullptr) {
            return 0;
        }

        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, Offsets::TargetLib::Name)) {
                s_libBase = static_cast<uintptr_t>(strtoul(line, nullptr, 16));
                break;
            }
        }
        fclose(fp);

        return s_libBase;
    }

    bool IsLibraryLoaded() {
        return (GetLibBase() != 0);
    }

    bool WriteBytes(uintptr_t absAddr, const uint8_t *bytes, size_t size) {
        if (absAddr == 0 || bytes == nullptr || size == 0) return false;
        if (KittyMemory::memWrite(reinterpret_cast<void*>(absAddr), bytes, size) == KittyMemory::SUCCESS) {
            FlushInstructionCache(reinterpret_cast<void*>(absAddr), size);
            return true;
        }
        return false;
    }

    bool ReadBytes(uintptr_t absAddr, uint8_t *outBuffer, size_t size) {
        if (absAddr == 0 || outBuffer == nullptr || size == 0) return false;
        return (KittyMemory::memRead(outBuffer, reinterpret_cast<const void*>(absAddr), size) == KittyMemory::SUCCESS);
    }

    bool HookRVA(uintptr_t rva, void *hookFunc, void **origFunc) {
        uintptr_t absAddr = GetRVA(rva);
        if (absAddr == 0 || hookFunc == nullptr) return false;

#if defined(__aarch64__)
        A64HookFunction(reinterpret_cast<void*>(absAddr), hookFunc, origFunc);
        return true;
#else
        MSHookFunction(reinterpret_cast<void*>(absAddr), hookFunc, origFunc);
        return true;
#endif
    }

} // namespace Memory
