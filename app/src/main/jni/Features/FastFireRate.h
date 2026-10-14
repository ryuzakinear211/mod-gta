#ifndef FEATURE_FAST_FIRERATE_H
#define FEATURE_FAST_FIRERATE_H

namespace FastFireRate {

    void Init();
    void SetEnabled(bool enable);
    bool IsEnabled();

    // Invoked from player update loop to track client weapon state
    void OnLocalPlayerUpdate(void *fpc);

} // namespace FastFireRate

#endif // FEATURE_FAST_FIRERATE_H
