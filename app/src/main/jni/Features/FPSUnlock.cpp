#include "FPSUnlock.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"
#include "../Core/Logger.h"
#include <atomic>

namespace FPSUnlock {

    static std::atomic<bool> s_enabled{false};
    static std::atomic<bool> s_pendingApply{false};
    static std::atomic<bool> s_pendingRestore{false};

    static void (*set_targetFrameRate)(int) = nullptr;
    static int  (*get_targetFrameRate)() = nullptr;
    static void (*set_vSyncCount)(int) = nullptr;

    void ApplyFrameRate() {
        if (s_enabled.load(std::memory_order_relaxed)) {
            if (set_vSyncCount != nullptr) {
                set_vSyncCount(0); // Disable VSync frame cap
            }
            if (set_targetFrameRate != nullptr) {
                set_targetFrameRate(60); // Unlock 60 FPS
            }
            if (s_pendingApply.exchange(false, std::memory_order_relaxed)) {
                LOG_INFO("[FPS] 60 FPS applied on Unity render thread (targetFrameRate=60, vSyncCount=0)");
            }
        } else if (s_pendingRestore.exchange(false, std::memory_order_relaxed)) {
            if (set_targetFrameRate != nullptr) {
                set_targetFrameRate(30);
            }
            if (set_vSyncCount != nullptr) {
                set_vSyncCount(1);
            }
            LOG_INFO("[FPS] Restored 30 FPS default on Unity render thread (targetFrameRate=30, vSyncCount=1)");
        }
    }

    void Init() {
        set_targetFrameRate = reinterpret_cast<void(*)(int)>(Memory::GetRVA(Offsets::Unity::SetTargetFrameRate));
        get_targetFrameRate = reinterpret_cast<int(*)()>(Memory::GetRVA(Offsets::Unity::GetTargetFrameRate));
        set_vSyncCount      = reinterpret_cast<void(*)(int)>(Memory::GetRVA(Offsets::Unity::SetVSyncCount));

        LOG_INFO("[FPS] FPSUnlock pointers resolved: set_targetFrameRate=%p, set_vSyncCount=%p",
                 set_targetFrameRate, set_vSyncCount);
    }

    void SetEnabled(bool enable) {
        bool wasEnabled = s_enabled.exchange(enable, std::memory_order_relaxed);
        if (enable) {
            s_pendingApply.store(true, std::memory_order_relaxed);
            LOG_INFO("[FPS] Unlock 60 FPS: ON (Queued for Unity render thread)");
        } else if (wasEnabled) {
            s_pendingRestore.store(true, std::memory_order_relaxed);
            LOG_INFO("[FPS] Unlock 60 FPS: OFF (Queued restore for Unity render thread)");
        }
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

} // namespace FPSUnlock
