#include "FPSUnlock.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>

namespace FPSUnlock {

    static std::atomic<bool> s_enabled{false};

    static void (*set_targetFrameRate)(int) = nullptr;
    static int  (*get_targetFrameRate)() = nullptr;
    static void (*set_vSyncCount)(int) = nullptr;

    // Hook Application.set_targetFrameRate to prevent the game engine from throttling back to 30 FPS
    static void (*old_set_targetFrameRate)(int) = nullptr;
    static void hook_set_targetFrameRate(int fps) {
        if (s_enabled.load(std::memory_order_relaxed)) {
            // Force 60 FPS minimum
            if (old_set_targetFrameRate) old_set_targetFrameRate(60);
            return;
        }
        if (old_set_targetFrameRate) old_set_targetFrameRate(fps);
    }

    void ApplyFrameRate() {
        if (!s_enabled.load(std::memory_order_relaxed)) return;

        if (set_vSyncCount != nullptr) {
            set_vSyncCount(0); // Disable VSync frame cap
        }
        if (set_targetFrameRate != nullptr) {
            set_targetFrameRate(60); // Unlock 60 FPS
        }
    }

    void Init() {
        set_targetFrameRate = reinterpret_cast<void(*)(int)>(Memory::GetRVA(Offsets::Unity::SetTargetFrameRate));
        get_targetFrameRate = reinterpret_cast<int(*)()>(Memory::GetRVA(Offsets::Unity::GetTargetFrameRate));
        set_vSyncCount      = reinterpret_cast<void(*)(int)>(Memory::GetRVA(Offsets::Unity::SetVSyncCount));

        Memory::HookRVA(Offsets::Unity::SetTargetFrameRate,
                        reinterpret_cast<void*>(hook_set_targetFrameRate),
                        reinterpret_cast<void**>(&old_set_targetFrameRate));

        LOG_INFO("[FPS] FPSUnlock hooks installed. Pointers: set_targetFrameRate=%p, set_vSyncCount=%p",
                 set_targetFrameRate, set_vSyncCount);

        if (s_enabled.load(std::memory_order_relaxed)) {
            ApplyFrameRate();
        }
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        if (enable) {
            ApplyFrameRate();
            LOG_INFO("[FPS] 60 FPS Unlocked (targetFrameRate=60, vSyncCount=0)");
        } else {
            if (set_targetFrameRate != nullptr) {
                set_targetFrameRate(30);
            }
            if (set_vSyncCount != nullptr) {
                set_vSyncCount(1);
            }
            LOG_INFO("[FPS] FPS restored to default (30 FPS, vSyncCount=1)");
        }
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace FPSUnlock
