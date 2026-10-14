#ifndef FEATURE_ESP_H
#define FEATURE_ESP_H

#include <jni.h>
#include <cstdint>

namespace ESP {

    void Init();

    // Callbacks executed on Unity Main Render Thread
    void OnAimingControlUpdate(void *aimingControl);
    void OnLocalPlayerUpdate(void *fpc);

    // Feature Toggles
    void SetEnabled(bool enable);
    bool IsEnabled();

    void SetEnemyOnly(bool enemyOnly);
    bool IsEnemyOnly();

    void SetBoxEnabled(bool enable);
    bool IsBoxEnabled();

    void SetLineEnabled(bool enable);
    bool IsLineEnabled();

    void SetDistanceEnabled(bool enable);
    bool IsDistanceEnabled();

    void SetHealthEnabled(bool enable);
    bool IsHealthEnabled();

    void SetNameEnabled(bool enable);
    bool IsNameEnabled();

    // High-performance JNI bridge methods consumed by ESPOverlayView
    jfloatArray GetESPData(JNIEnv *env, jint screenWidth, jint screenHeight);
    jboolean GetESPSetting(jint settingId);

} // namespace ESP

#endif // FEATURE_ESP_H
