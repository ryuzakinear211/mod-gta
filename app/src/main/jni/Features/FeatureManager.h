#ifndef FEATURE_MANAGER_H
#define FEATURE_MANAGER_H

#include <jni.h>

namespace FeatureManager {

    // Initializes all game features and hooks once libil2cpp.so is loaded
    void InitAll();

    // Returns the features list array for the LGL floating menu UI
    jobjectArray GetFeatureList(JNIEnv *env, jobject context);

    // Handles user toggle / changes from the LGL menu UI
    void OnToggle(JNIEnv *env, jclass clazz, jobject ctx,
                  jint featNum, jstring featName, jint value,
                  jboolean boolean, jstring str);

} // namespace FeatureManager

#endif // FEATURE_MANAGER_H
