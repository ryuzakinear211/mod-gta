#ifndef GTA_MOD_HOOK_H
#define GTA_MOD_HOOK_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "KittyMemory/MemoryPatch.h"

// Target il2cpp library name for GTA SA FPS (64-bit ARM)
#ifndef targetLibName
#define targetLibName OBFUSCATE("libil2cpp.so")
#endif

#include "Includes/Macros.h"

// Forward declaration of custom logging function implemented in Main.cpp
void ModLog(const char *fmt, ...);
void setLastAction(const char *action);

// =========================================================================
// SECTION 1: Game & Unity RVA Catalog (Preserved Exactly Without Any Change)
// =========================================================================
namespace Offsets {
    // ---------------------------------------------------------------------
    // Unity Engine RVAs (libil2cpp.so)
    // ---------------------------------------------------------------------
    constexpr uintptr_t UNITY_GET_TRANSFORM                 = 0x8597C20;
    constexpr uintptr_t UNITY_SET_LOCAL_SCALE_INJECTED      = 0x85B224C;
    constexpr uintptr_t UNITY_GET_BONE_TRANSFORM            = 0x84EBAB0;
    constexpr uintptr_t UNITY_GET_POSITION_INJECTED         = 0x85B1394;
    constexpr uintptr_t UNITY_GET_LOCAL_EULER_ANGLES        = 0x85B15F4;
    constexpr uintptr_t UNITY_SET_LOCAL_EULER_ANGLES        = 0x85B16E8;
    constexpr uintptr_t UNITY_WORLD_TO_VIEWPORT_POINT       = 0x851DF1C;
    constexpr uintptr_t UNITY_WORLD_TO_SCREEN_POINT         = 0x851DE04;
    constexpr uintptr_t UNITY_CAMERA_GET_MAIN               = 0x851ED38;
    constexpr uintptr_t UNITY_SET_ROTATION_INJECTED         = 0x85B1E5C;
    constexpr uintptr_t UNITY_LOOK_ROTATION_INJECTED        = 0x8588584;
    constexpr uintptr_t UNITY_COLLIDER_GET_ENABLED          = 0x864B404;
    constexpr uintptr_t UNITY_BEHAVIOUR_GET_IS_ACTIVE       = 0x85974E0;
    constexpr uintptr_t UNITY_PHYSICS_GET_DEFAULT_SCENE     = 0x864C91C;
    constexpr uintptr_t UNITY_RAYCAST_TEST_INJECTED         = 0x8654A80;

    // ---------------------------------------------------------------------
    // Player Weapon, Firerate & Recoil RVAs
    // ---------------------------------------------------------------------
    constexpr uintptr_t FPC_UPDATE                          = 0x40A1BF4;
    constexpr uintptr_t FPC_GET_CURRENT_WEAPON              = 0x408FC44;
    constexpr uintptr_t FPC_GET_SHOOTER_BEHAVIOUR           = 0x409188C;
    constexpr uintptr_t WEAPON_SET_CLIP_AMMO                = 0x405A39C;
    constexpr uintptr_t ACTK_OP_IMPLICIT_FLOAT              = 0x3D6E4DC;
    constexpr uintptr_t TARGET_INFO_GET_TARGET_TYPE         = 0x40AAA58;
    constexpr uintptr_t TARGET_INFO_GET_TARGET_TYPE_REMOTE  = 0x4B9B610;
    constexpr uintptr_t FPC_GET_NETWORK_PLAYER              = 0x40A1050;
    constexpr uintptr_t WEAPON_CAN_SHOOT                    = 0x405BBF0;
    constexpr uintptr_t WEAPON_SET_COOLDOWN                 = 0x405D084;
    constexpr uintptr_t WEAPON_SHOOT_ACTION_GETTER          = 0x405C8DC;
    constexpr uintptr_t FPC_GET_SHOT_INTERVAL1              = 0x40906CC;
    constexpr uintptr_t FPC_GET_SHOT_INTERVAL2              = 0x409F2F8;
    constexpr uintptr_t SHOOT_COROUTINE_MOVE_NEXT           = 0x40B93A0;
    constexpr uintptr_t WEAPON_PARAMS_GET_FIRE_RATE         = 0x49D0E90;
    constexpr uintptr_t WEAPON_PROFILE_GET_FIRE_RATE        = 0x4A451C0;
    constexpr uintptr_t FPC_RECOIL_SHIFT                    = 0x40A4420;
    constexpr uintptr_t AIMING_CONTROL_RECOIL1              = 0x4D27AB8;
    constexpr uintptr_t AIMING_CONTROL_RECOIL2              = 0x4D27E20;
    constexpr uintptr_t AIMING_CONTROL_RECOIL3              = 0x4D284B0;

    // ---------------------------------------------------------------------
    // Entity Lifecycle & Target Filtering RVAs
    // ---------------------------------------------------------------------
    constexpr uintptr_t NETWORK_PLAYER_UPDATE               = 0x42CC8C8;
    constexpr uintptr_t NETWORK_PLAYER_ON_DESTROY           = 0x42C9540;
    constexpr uintptr_t BOT_PLAYER_START                    = 0x44493F4;
    constexpr uintptr_t BOT_PLAYER_UPDATE                   = 0x444A310;
    constexpr uintptr_t BOT_PLAYER_ON_DESTROY               = 0x4449EB0;
    constexpr uintptr_t TARGETIBLE_GET_ALLY_OBJECT_TOOGLE   = 0x4DED164;
    constexpr uintptr_t TARGETIBLE_GET_IS_AUTO_AIM_ALLOWED  = 0x4DED124;
    constexpr uintptr_t NETWORK_PLAYER_GET_TARGETIBLE_OBJ   = 0x42D1FBC;
    constexpr uintptr_t BOT_PLAYER_GET_TARGETIBLE_OBJ       = 0x444A050;
    constexpr uintptr_t AIMING_CONTROL_SET_TARGETIBLE_OBJ   = 0x4D287F8;
    constexpr uintptr_t AIMING_CONTROL_CLEAR_TARGETIBLE_OBJ = 0x4D2883C;
    constexpr uintptr_t AIMING_CONTROL_ADD_DELTA            = 0x4D281AC;
    constexpr uintptr_t NETWORK_PLAYER_IS_TEAMMATE          = 0x42CE9CC;
    constexpr uintptr_t BOT_PLAYER_HEALTH_GET_HEALTH        = 0x4BEE4FC;
    constexpr uintptr_t FPC_ON_DESTROY                      = 0x40A3A84;
    constexpr uintptr_t AIMING_CONTROL_ON_DESTROY           = 0x4D28714;

    // ---------------------------------------------------------------------
    // Native Aim Assist & Sensitivity RVAs
    // ---------------------------------------------------------------------
    constexpr uintptr_t STRAFE_ROT_GET_MAX_ROTATION_POWER   = 0x52A6264;
    constexpr uintptr_t STRAFE_ROT_GET_FOV_AREA_MULTIPLIER  = 0x52A6210;
    constexpr uintptr_t STRAFE_ROT_GET_DEFAULT_MAX_DISTANCE = 0x52A624C;
    constexpr uintptr_t STRAFE_ROT_GET_ZOOMED_MAX_DISTANCE  = 0x52A65A4;
    constexpr uintptr_t STRAFE_ROT_GET_FOV_POWER_MULTIPLIER = 0x52A6598;
    constexpr uintptr_t SPIN_SLOWDOWN_GET_MAX_SLOWDOWN      = 0x4600D68;
    constexpr uintptr_t SPIN_SLOWDOWN_GET_FOV_AREA_MULT     = 0x4600BCC;
    constexpr uintptr_t SPIN_SLOWDOWN_GET_RADIUS            = 0x4600BD8;
    constexpr uintptr_t SPIN_SLOWDOWN_GET_DEFAULT_MAX_DIST  = 0x4600C44;
    constexpr uintptr_t SPIN_SLOWDOWN_GET_ZOOMED_MAX_DIST   = 0x4600C5C;
    constexpr uintptr_t STRAFE_ROTATION_CALCULATE           = 0x4F0E254;
    constexpr uintptr_t AIM_ASSIST_MGR_SET_ENABLED          = 0x529060C;
    constexpr uintptr_t AIMING_CONTROL_UPDATE               = 0x4D27294;
    constexpr uintptr_t NEW_AUTO_AIM_SET_ENABLED            = 0x421C470;
    constexpr uintptr_t BASE_AIM_ASSIST_SET_ENABLED         = 0x4478A30;
    constexpr uintptr_t BASE_AIM_ASSIST_IS_VALID_TARGET     = 0x4478A70;
    constexpr uintptr_t NEW_AUTO_AIM_IS_VALID_TARGET        = 0x421C254;
}

// =========================================================================
// SECTION 2: Core Data Types & Math Structures
// =========================================================================
struct Vector2 {
    float x;
    float y;
    Vector2() : x(0.0f), y(0.0f) {}
    Vector2(float _x, float _y) : x(_x), y(_y) {}
};

struct Vector3 {
    float x;
    float y;
    float z;
    Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vector3 operator+(const Vector3 &o) const { return Vector3(x + o.x, y + o.y, z + o.z); }
    Vector3 operator-(const Vector3 &o) const { return Vector3(x - o.x, y - o.y, z - o.z); }
    Vector3 operator*(float s) const { return Vector3(x * s, y * s, z * s); }
    Vector3 operator/(float s) const { return (s != 0.0f) ? Vector3(x / s, y / s, z / s) : Vector3(); }
};

struct Quaternion {
    float x;
    float y;
    float z;
    float w;

    Quaternion() : x(0.0f), y(0.0f), z(0.0f), w(1.0f) {}
    Quaternion(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

    static Quaternion LookRotation(Vector3 forward, Vector3 up = Vector3(0.0f, 1.0f, 0.0f));
};

struct Ray {
    Vector3 origin;
    Vector3 direction;
    Ray() : origin(0.0f, 0.0f, 0.0f), direction(0.0f, 0.0f, 0.0f) {}
    Ray(Vector3 o, Vector3 d) : origin(o), direction(d) {}
};

struct PhysicsScene {
    int index;
    int version;
    PhysicsScene() : index(0), version(0) {}
    PhysicsScene(int i, int v) : index(i), version(v) {}
};

struct ObscuredFloat {
    int hash;
    int hiddenValue;
    int currentCryptoKey;
    float fakeValue;
    uint32_t hiddenValueOldByte4;
};

struct EntityStats {
    int realPlayerCount;
    int botCount;
    int totalEntities;
    bool inGame;
};

// =========================================================================
// SECTION 3: Safe Math & Normalization Helpers (NaN / Inf Hardened)
// =========================================================================
inline float NormalizeAngle(float angle) {
    if (std::isnan(angle) || std::isinf(angle)) return 0.0f;
    angle = fmodf(angle, 360.0f);
    if (angle < 0.0f) angle += 360.0f;
    return angle;
}

inline Vector3 NormalizeAngles(Vector3 angles) {
    angles.x = NormalizeAngle(angles.x);
    angles.y = NormalizeAngle(angles.y);
    angles.z = NormalizeAngle(angles.z);
    return angles;
}

inline Vector3 Vector3Normalize(const Vector3 &v) {
    float len = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-6f) {
        float inv = 1.0f / len;
        return Vector3(v.x * inv, v.y * inv, v.z * inv);
    }
    return Vector3(0.0f, 0.0f, 0.0f);
}

inline Vector3 Vector3Cross(const Vector3 &a, const Vector3 &b) {
    return Vector3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

inline Vector3 ToEulerRad(Quaternion q1) {
    float Rad2Deg = 360.0f / ((float)M_PI * 2.0f);

    float sqw = q1.w * q1.w;
    float sqx = q1.x * q1.x;
    float sqy = q1.y * q1.y;
    float sqz = q1.z * q1.z;
    float unit = sqx + sqy + sqz + sqw;
    if (unit < 1e-6f) return Vector3(0.0f, 0.0f, 0.0f);

    float test = q1.x * q1.w - q1.y * q1.z;
    Vector3 v;

    if (test > 0.4995f * unit) {
        v.y = 2.0f * atan2f(q1.y, q1.x);
        v.x = (float)M_PI / 2.0f;
        v.z = 0;
        return NormalizeAngles(v * Rad2Deg);
    }
    if (test < -0.4995f * unit) {
        v.y = -2.0f * atan2f(q1.y, q1.x);
        v.x = -(float)M_PI / 2.0f;
        v.z = 0;
        return NormalizeAngles(v * Rad2Deg);
    }

    Quaternion q(q1.w, q1.z, q1.x, q1.y);
    v.y = atan2f(2.0f * q.x * q.w + 2.0f * q.y * q.z, 1.0f - 2.0f * (q.z * q.z + q.w * q.w)); // yaw

    // Clamp sin input to [-1.0f, 1.0f] to prevent NaN
    float sinPitch = 2.0f * (q.x * q.z - q.w * q.y);
    if (sinPitch > 1.0f) sinPitch = 1.0f;
    else if (sinPitch < -1.0f) sinPitch = -1.0f;
    v.x = asinf(sinPitch); // pitch

    v.z = atan2f(2.0f * q.x * q.y + 2.0f * q.z * q.w, 1.0f - 2.0f * (q.y * q.y + q.z * q.z)); // roll
    return NormalizeAngles(v * Rad2Deg);
}

// Forward declaration of engine look rotation pointer used in Quaternion::LookRotation
extern void (*LookRotation_Injected)(const Vector3 *, const Vector3 *, Quaternion *);

inline Quaternion Quaternion::LookRotation(Vector3 forward, Vector3 up) {
    if (LookRotation_Injected != nullptr) {
        Quaternion qOut;
        LookRotation_Injected(&forward, &up, &qOut);
        if (fabsf(qOut.w) > 1e-4f || fabsf(qOut.x) > 1e-4f || fabsf(qOut.y) > 1e-4f || fabsf(qOut.z) > 1e-4f) {
            return qOut;
        }
    }

    Vector3 f = Vector3Normalize(forward);
    if (f.x == 0.0f && f.y == 0.0f && f.z == 0.0f) {
        return Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
    }

    Vector3 r = Vector3Cross(up, f);
    float rLen = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
    if (rLen < 1e-4f) {
        Vector3 fallbackUp = (fabsf(f.y) > 0.9f) ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(0.0f, 1.0f, 0.0f);
        r = Vector3Normalize(Vector3Cross(fallbackUp, f));
    } else {
        r = Vector3(r.x / rLen, r.y / rLen, r.z / rLen);
    }

    Vector3 u = Vector3Cross(f, r);

    float m00 = r.x, m01 = u.x, m02 = f.x;
    float m10 = r.y, m11 = u.y, m12 = f.y;
    float m20 = r.z, m21 = u.z, m22 = f.z;

    float trace = m00 + m11 + m22;
    Quaternion q;
    if (trace > 0.0f) {
        float s = 0.5f / sqrtf(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (m21 - m12) * s;
        q.y = (m02 - m20) * s;
        q.z = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        float s = 2.0f * sqrtf(1.0f + m00 - m11 - m22);
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = 2.0f * sqrtf(1.0f + m11 - m00 - m22);
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = 2.0f * sqrtf(1.0f + m22 - m00 - m11);
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

// =========================================================================
// SECTION 4: Memory Safety & Safe Pointer Validation
// =========================================================================
inline int g_safetyPipe[2] = {-1, -1};
inline std::mutex g_pipeMutex;

inline void initSafetyPipe() {
    if (g_safetyPipe[0] == -1) {
        if (pipe(g_safetyPipe) == 0) {
            fcntl(g_safetyPipe[0], F_SETFL, O_NONBLOCK);
            fcntl(g_safetyPipe[1], F_SETFL, O_NONBLOCK);
            fcntl(g_safetyPipe[0], F_SETFD, FD_CLOEXEC);
            fcntl(g_safetyPipe[1], F_SETFD, FD_CLOEXEC);
            ModLog("[SAFETY] Memory inspection pipe initialized successfully.");
        } else {
            ModLog("[WARN] Failed to initialize safety pipe: errno=%d", errno);
        }
    }
}

inline bool isPointerReadable(const void *ptr) {
    if (ptr == nullptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000 || addr >= 0x0080000000000000ULL) return false;

    if (g_safetyPipe[1] != -1) {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        char dummy;
        while (read(g_safetyPipe[0], &dummy, 1) > 0); // Drain any residual bytes

        ssize_t written = write(g_safetyPipe[1], ptr, 1);
        if (written == 1) {
            read(g_safetyPipe[0], &dummy, 1);
            return true;
        }
        return false;
    }
    return true;
}

inline bool isUnityObjectAlive(const void *unityObj) {
    if (unityObj == nullptr || !isPointerReadable(unityObj)) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(unityObj);
    if ((addr & 0x7) != 0) return false;

    // Check klass at offset 0x00
    void *klass = *reinterpret_cast<void **>(addr);
    if (klass == nullptr || !isPointerReadable(klass)) return false;

    // Validate that address (addr + 0x10) is readable before dereferencing m_CachedPtr
    if (!isPointerReadable(reinterpret_cast<const void *>(addr + 0x10))) return false;
    void *cachedPtr = *reinterpret_cast<void **>(addr + 0x10);
    if (cachedPtr == nullptr || !isPointerReadable(cachedPtr)) return false;

    return true;
}

inline void* getNativeUnityPointer(void *unityObj) {
    if (!isUnityObjectAlive(unityObj)) return nullptr;
    return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(unityObj) + 0x10);
}

// =========================================================================
// SECTION 5: Engine & Game Function Pointers
// =========================================================================
inline void *(*get_transform)(void *) = nullptr;
inline void (*set_localScale_Injected)(void *, const Vector3 *) = nullptr;
inline void *(*GetBoneTransform)(void *, int) = nullptr;
inline void (*get_position_Injected)(void *, Vector3 *) = nullptr;
inline void (*GetLocalEulerAngles_Injected)(void *, int, Vector3 *) = nullptr;
inline void (*SetLocalEulerAngles_Injected)(void *, const Vector3 *, int) = nullptr;
inline void (*WorldToViewportPoint_Injected)(void *, const Vector3 *, int, Vector3 *) = nullptr;
inline void (*WorldToScreenPoint_Injected)(void *, const Vector3 *, int, Vector3 *) = nullptr;
inline void *(*Camera_get_main)() = nullptr;
inline void (*set_rotation_Injected)(void *, const Quaternion *) = nullptr;
inline void (*LookRotation_Injected)(const Vector3 *, const Vector3 *, Quaternion *) = nullptr;
inline bool (*Collider_get_enabled)(void *) = nullptr;
inline bool (*Behaviour_get_isActiveAndEnabled)(void *) = nullptr;
inline PhysicsScene (*Physics_get_defaultPhysicsScene)() = nullptr;
inline bool (*Internal_RaycastTest_Injected)(const void *, const Ray *, float, int, int) = nullptr;

inline void* (*get_CurrentWeapon)(void *fpc) = nullptr;
inline void* (*get_ShooterBehaviour)(void *fpc, void *item) = nullptr;
inline void (*set_ClipAmmo)(void *shooter, int ammo) = nullptr;
inline ObscuredFloat (*actk_op_Implicit_Float)(float val) = nullptr;
inline int (*get_TargetType)(void *targetInfo) = nullptr;
inline int (*TargetInfo_GetTargetType_Remote)(void *targetInfo) = nullptr;
inline void *(*FPC_GetNetworkPlayer)(void *fpc) = nullptr;

inline bool (*get_AllyObjectToogle)(void *) = nullptr;
inline bool (*get_IsAutoAimAllowed)(void *) = nullptr;
inline void *(*NetworkPlayer_GetTargetibleObject)(void *) = nullptr;
inline void *(*BotPlayer_GetTargetibleObject)(void *) = nullptr;
inline void (*AimingControl_SetTargetibleObject)(void *, void *) = nullptr;
inline void (*AimingControl_ClearTargetibleObject)(void *) = nullptr;
inline void (*AimingControl_AddDelta)(void *, Vector2) = nullptr;
inline bool (*NetworkPlayer_IsTeammate)(void *, void *) = nullptr;
inline float (*BotPlayerHealth_GetHealth)(void *) = nullptr;

// Safe wrapper to set local scale on a Transform
inline void safeSetLocalScale(void *transformObj, const Vector3 &scale) {
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || set_localScale_Injected == nullptr) return;
    void *nativeTrans = getNativeUnityPointer(transformObj);
    if (nativeTrans == nullptr) return;
    set_localScale_Injected(nativeTrans, &scale);
}

// Safe wrapper to query local euler angles from a Transform
inline bool getTransformLocalEulerAngles(void *transformObj, Vector3 &eulerOut) {
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || GetLocalEulerAngles_Injected == nullptr) return false;
    void *nativeTrans = getNativeUnityPointer(transformObj);
    if (nativeTrans == nullptr) return false;
    GetLocalEulerAngles_Injected(nativeTrans, 4, &eulerOut);
    return true;
}

// Safe wrapper to set local euler angles on a Transform
inline void setTransformLocalEulerAngles(void *transformObj, const Vector3 &euler) {
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || SetLocalEulerAngles_Injected == nullptr) return;
    void *nativeTrans = getNativeUnityPointer(transformObj);
    if (nativeTrans == nullptr) return;
    SetLocalEulerAngles_Injected(nativeTrans, &euler, 4);
}

// Safe wrapper to query position from a Transform
inline Vector3 getTransformPosition(void *transformObj) {
    Vector3 pos(0.0f, 0.0f, 0.0f);
    if (transformObj == nullptr || !isUnityObjectAlive(transformObj) || get_position_Injected == nullptr) return pos;
    void *nativeTrans = getNativeUnityPointer(transformObj);
    if (nativeTrans == nullptr) return pos;
    get_position_Injected(nativeTrans, &pos);
    return pos;
}

// Safe wrapper to project world coordinates to viewport (Mono mode = 2)
inline bool worldToViewport(void *cameraObj, const Vector3 &worldPos, Vector3 &viewportPos) {
    if (cameraObj == nullptr || !isUnityObjectAlive(cameraObj) || WorldToViewportPoint_Injected == nullptr) return false;
    void *nativeCam = getNativeUnityPointer(cameraObj);
    if (nativeCam == nullptr) return false;
    WorldToViewportPoint_Injected(nativeCam, &worldPos, 2, &viewportPos);
    return (viewportPos.z > 0.1f);
}

// =========================================================================
// SECTION 6: High-Performance Cached Library Address Resolver
// =========================================================================
inline uintptr_t g_cachedLibBase = 0;

inline uintptr_t getCachedLibBase() {
    if (g_cachedLibBase == 0) {
        g_cachedLibBase = findLibrary(targetLibName);
    }
    return g_cachedLibBase;
}

inline void* getLibAddress(uintptr_t offset) {
    uintptr_t base = getCachedLibBase();
    if (base == 0) return nullptr;
    return reinterpret_cast<void *>(base + offset);
}

// Template helper to resolve a function pointer with automatic logging
template <typename T>
inline void resolvePointer(uintptr_t base, uintptr_t offset, T &target, const char *name) {
    if (base == 0 || offset == 0) {
        target = nullptr;
        return;
    }
    target = reinterpret_cast<T>(base + offset);
    ModLog("[RESOLVE] %s: %p (0x%lX)", name, reinterpret_cast<void *>(target), offset);
}

// Resolves all game and engine function pointers in a structured batch
inline void initAllFunctionPointers(uintptr_t base) {
    setLastAction("initAllFunctionPointers");

    // Unity Engine Pointers
    resolvePointer(base, Offsets::UNITY_GET_TRANSFORM,                 get_transform,                 "get_transform");
    resolvePointer(base, Offsets::UNITY_SET_LOCAL_SCALE_INJECTED,      set_localScale_Injected,       "set_localScale_Injected");
    resolvePointer(base, Offsets::UNITY_GET_BONE_TRANSFORM,            GetBoneTransform,              "GetBoneTransform");
    resolvePointer(base, Offsets::UNITY_GET_POSITION_INJECTED,         get_position_Injected,         "get_position_Injected");
    resolvePointer(base, Offsets::UNITY_GET_LOCAL_EULER_ANGLES,        GetLocalEulerAngles_Injected,  "GetLocalEulerAngles_Injected");
    resolvePointer(base, Offsets::UNITY_SET_LOCAL_EULER_ANGLES,        SetLocalEulerAngles_Injected,  "SetLocalEulerAngles_Injected");
    resolvePointer(base, Offsets::UNITY_WORLD_TO_VIEWPORT_POINT,       WorldToViewportPoint_Injected, "WorldToViewportPoint_Injected");
    resolvePointer(base, Offsets::UNITY_WORLD_TO_SCREEN_POINT,         WorldToScreenPoint_Injected,   "WorldToScreenPoint_Injected");
    resolvePointer(base, Offsets::UNITY_CAMERA_GET_MAIN,               Camera_get_main,               "Camera_get_main");
    resolvePointer(base, Offsets::UNITY_SET_ROTATION_INJECTED,         set_rotation_Injected,         "set_rotation_Injected");
    resolvePointer(base, Offsets::UNITY_LOOK_ROTATION_INJECTED,        LookRotation_Injected,         "LookRotation_Injected");
    resolvePointer(base, Offsets::UNITY_COLLIDER_GET_ENABLED,          Collider_get_enabled,          "Collider_get_enabled");
    resolvePointer(base, Offsets::UNITY_BEHAVIOUR_GET_IS_ACTIVE,       Behaviour_get_isActiveAndEnabled, "Behaviour_get_isActiveAndEnabled");
    resolvePointer(base, Offsets::UNITY_PHYSICS_GET_DEFAULT_SCENE,     Physics_get_defaultPhysicsScene, "Physics_get_defaultPhysicsScene");
    resolvePointer(base, Offsets::UNITY_RAYCAST_TEST_INJECTED,         Internal_RaycastTest_Injected, "Internal_RaycastTest_Injected");

    // Player & Weapon Pointers
    resolvePointer(base, Offsets::FPC_GET_CURRENT_WEAPON,              get_CurrentWeapon,             "get_CurrentWeapon");
    resolvePointer(base, Offsets::FPC_GET_SHOOTER_BEHAVIOUR,           get_ShooterBehaviour,          "get_ShooterBehaviour");
    resolvePointer(base, Offsets::WEAPON_SET_CLIP_AMMO,                set_ClipAmmo,                  "set_ClipAmmo");
    resolvePointer(base, Offsets::ACTK_OP_IMPLICIT_FLOAT,              actk_op_Implicit_Float,        "actk_op_Implicit_Float");
    resolvePointer(base, Offsets::TARGET_INFO_GET_TARGET_TYPE,         get_TargetType,                "get_TargetType");
    resolvePointer(base, Offsets::TARGET_INFO_GET_TARGET_TYPE_REMOTE,  TargetInfo_GetTargetType_Remote, "TargetInfo_GetTargetType_Remote");
    resolvePointer(base, Offsets::FPC_GET_NETWORK_PLAYER,              FPC_GetNetworkPlayer,          "FPC_GetNetworkPlayer");

    // Aim Assist & Filtering Pointers
    resolvePointer(base, Offsets::TARGETIBLE_GET_ALLY_OBJECT_TOOGLE,   get_AllyObjectToogle,          "get_AllyObjectToogle");
    resolvePointer(base, Offsets::TARGETIBLE_GET_IS_AUTO_AIM_ALLOWED,  get_IsAutoAimAllowed,          "get_IsAutoAimAllowed");
    resolvePointer(base, Offsets::NETWORK_PLAYER_GET_TARGETIBLE_OBJ,   NetworkPlayer_GetTargetibleObject, "NetworkPlayer_GetTargetibleObject");
    resolvePointer(base, Offsets::BOT_PLAYER_GET_TARGETIBLE_OBJ,       BotPlayer_GetTargetibleObject, "BotPlayer_GetTargetibleObject");
    resolvePointer(base, Offsets::AIMING_CONTROL_SET_TARGETIBLE_OBJ,   AimingControl_SetTargetibleObject, "AimingControl_SetTargetibleObject");
    resolvePointer(base, Offsets::AIMING_CONTROL_CLEAR_TARGETIBLE_OBJ, AimingControl_ClearTargetibleObject, "AimingControl_ClearTargetibleObject");
    resolvePointer(base, Offsets::AIMING_CONTROL_ADD_DELTA,            AimingControl_AddDelta,        "AimingControl_AddDelta");
    resolvePointer(base, Offsets::NETWORK_PLAYER_IS_TEAMMATE,          NetworkPlayer_IsTeammate,      "NetworkPlayer_IsTeammate");
    resolvePointer(base, Offsets::BOT_PLAYER_HEALTH_GET_HEALTH,        BotPlayerHealth_GetHealth,     "BotPlayerHealth_GetHealth");
}

// =========================================================================
// SECTION 7: Modular Hook Registry & Hook Manager
// =========================================================================
struct HookEntry {
    uintptr_t offset;
    void *hookFunc;
    void **origFunc;
    const char *name;
};

class HookManager {
private:
    std::vector<HookEntry> m_hooks;

public:
    static HookManager& getInstance() {
        static HookManager instance;
        return instance;
    }

    void registerHook(uintptr_t offset, void *hookFunc, void **origFunc, const char *name) {
        m_hooks.push_back({offset, hookFunc, origFunc, name});
    }

    size_t installAll(uintptr_t base) {
        if (base == 0) {
            LOGE(OBFUSCATE("[HOOK_MGR] Library base is 0. Aborting hook installation."));
            return 0;
        }

        size_t successCount = 0;
        for (const auto &entry : m_hooks) {
            void *targetAddress = reinterpret_cast<void *>(base + entry.offset);
            hook(targetAddress, entry.hookFunc, entry.origFunc);
            bool success = (entry.origFunc != nullptr && *entry.origFunc != nullptr);
            if (success) {
                successCount++;
            }
            ModLog("[HOOK] %s (0x%lX): %s", entry.name, entry.offset, success ? "SUCCESS" : "INSTALLED");
        }
        ModLog("[HOOK_MGR] %zu / %zu hooks processed successfully.", successCount, m_hooks.size());
        return successCount;
    }

    void clear() {
        m_hooks.clear();
    }
};

// =========================================================================
// SECTION 8: Extensible Utilities for Future Features (ESP, String reading)
// =========================================================================
// Safely reads UTF-16 strings from Il2Cpp managed memory with bounds protection
inline std::string readIl2CppString(const void *il2cppString, size_t maxChars = 64) {
    if (il2cppString == nullptr || !isPointerReadable(il2cppString)) return "";
    uintptr_t strAddr = reinterpret_cast<uintptr_t>(il2cppString);
    if (!isPointerReadable(reinterpret_cast<const void *>(strAddr + 0x10))) return "";

    int32_t len = *reinterpret_cast<const int32_t *>(strAddr + 0x10);
    if (len <= 0 || len > 256) return "";

    size_t charCount = static_cast<size_t>(len);
    if (charCount > maxChars) charCount = maxChars;

    const uint16_t *chars = reinterpret_cast<const uint16_t *>(strAddr + 0x14);
    if (!isPointerReadable(chars)) return "";

    std::string result;
    result.reserve(charCount);
    for (size_t i = 0; i < charCount; i++) {
        if (!isPointerReadable(&chars[i])) break;
        uint16_t c = chars[i];
        if (c == 0) break;
        if (c < 128) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('?');
        }
    }
    return result;
}

#endif // GTA_MOD_HOOK_H
