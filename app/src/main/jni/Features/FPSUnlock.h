#ifndef FPS_UNLOCK_H
#define FPS_UNLOCK_H

namespace FPSUnlock {
    void Init();
    void SetEnabled(bool enable);
    bool IsEnabled();
    void ApplyFrameRate();
}

#endif // FPS_UNLOCK_H
