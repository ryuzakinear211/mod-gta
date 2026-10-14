#ifndef FEATURE_NO_RECOIL_H
#define FEATURE_NO_RECOIL_H

namespace NoRecoil {

    void Init();
    void SetEnabled(bool enable);
    bool IsEnabled();
    void OnLocalPlayerUpdate(void *fpc);

} // namespace NoRecoil

#endif // FEATURE_NO_RECOIL_H
