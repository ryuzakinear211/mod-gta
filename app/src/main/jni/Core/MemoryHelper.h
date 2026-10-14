#ifndef MEMORY_HELPER_H
#define MEMORY_HELPER_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include "../GameOffsets.h"

namespace Memory {

    // Retrieves and caches base address of the target library (libil2cpp.so)
    uintptr_t GetLibBase();

    // Checks whether target library is loaded in memory
    bool IsLibraryLoaded();

    // Returns absolute virtual address for an RVA from dump.cs
    inline uintptr_t GetRVA(uintptr_t rva) {
        uintptr_t base = GetLibBase();
        return base ? (base + rva) : 0;
    }

    // High-performance, zero-syscall pointer validation for ARM64 Linux/Android
    // Eliminates kernel context-switching overhead from per-frame game loops
    inline bool IsValidPtr(const void *ptr) {
        if (ptr == nullptr) return false;
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        // Ensure 8-byte pointer alignment on 64-bit architecture
        if ((addr & 0x7) != 0) return false;
        // User space virtual address boundary check for Linux ARM64
        return (addr >= 0x10000000ULL && addr <= 0x00007FFFFFFFFFFFULL);
    }

    // Safely reads a field at (instance + offset) from Il2Cpp objects
    template<typename T>
    inline T ReadField(void *instance, uintptr_t offset, T defaultVal = T()) {
        if (!IsValidPtr(instance)) return defaultVal;
        void *fieldPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(instance) + offset);
        return *reinterpret_cast<T*>(fieldPtr);
    }

    // Safely writes a field at (instance + offset) to Il2Cpp objects
    template<typename T>
    inline bool WriteField(void *instance, uintptr_t offset, const T &value) {
        if (!IsValidPtr(instance)) return false;
        void *fieldPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(instance) + offset);
        *reinterpret_cast<T*>(fieldPtr) = value;
        return true;
    }

    // Clears instruction cache after modifying executable memory
    inline void FlushInstructionCache(void *addr, size_t size) {
        char *p = reinterpret_cast<char*>(addr);
        __builtin___clear_cache(p, p + size);
    }

    // Writes raw bytes to memory with cache clearing
    bool WriteBytes(uintptr_t absAddr, const uint8_t *bytes, size_t size);

    // Reads raw bytes from memory
    bool ReadBytes(uintptr_t absAddr, uint8_t *outBuffer, size_t size);

    // Installs hook on target RVA
    bool HookRVA(uintptr_t rva, void *hookFunc, void **origFunc);

} // namespace Memory

#endif // MEMORY_HELPER_H
