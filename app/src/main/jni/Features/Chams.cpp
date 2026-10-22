#include "Chams.h"
#include "../Core/Logger.h"
#include "../GameOffsets.h"
#include "../Core/MemoryHelper.h"

#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <vector>
#include <mutex>
#include <chrono>

#if defined(__aarch64__)
#include "../And64InlineHook/And64InlineHook.hpp"
#else
#include "../Substrate/SubstrateHook.h"
#endif

namespace Chams {

    static std::atomic<bool> s_enabled{false};
    static std::atomic<bool> s_enemyOnly{true}; // Ignore teammates by default
    static std::atomic<int>  s_mode{static_cast<int>(Mode::Shading)}; // Default: Shading / Wallhack
    static std::atomic<int>  s_shaderFilter{static_cast<int>(ShaderFilter::BodyBonesAuto)}; // Default: Body Bones (Anti-Map)

    static std::mutex           s_teammatesMutex;
    static std::vector<Vector3> s_teammatePositions;

    // Vehicle tracking structure (Tanks, Cars & Armored Vehicles)
    struct VehicleTrackInfo {
        Vector3 position;
        uint64_t lastSeenMs;
    };
    static std::mutex                   s_vehicleMutex;
    static std::vector<VehicleTrackInfo> s_vehiclePositions;

    static void*   (*s_get_transform)(void *) = nullptr;
    static Vector3 (*s_Transform_get_position)(void *) = nullptr;

    static inline uint64_t GetCurrentTimeMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
    }

    static std::atomic<float> s_r{255.0f}; // Default vibrant crimson red
    static std::atomic<float> s_g{42.0f};
    static std::atomic<float> s_b{85.0f};
    static std::atomic<float> s_a{255.0f};
    static std::atomic<int>   s_lineWidth{2};

    // Rainbow animation state
    static float s_rainbowR = 255.0f;
    static float s_rainbowG = 0.0f;
    static float s_rainbowB = 0.0f;
    static int   s_rainbowStage = 0;

    // Original GLES function pointers
    static void (*old_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void *indices) = nullptr;

    // Classification cache for GLES programs: -1 = uncomputed, 0 = reject, 1 = allow
    static constexpr int CACHE_SIZE = 2048;
    static int8_t s_programFilterCache[8][CACHE_SIZE];
    static int16_t s_objToWorldLocCache[CACHE_SIZE];

    void ClearProgramCache() {
        memset(s_programFilterCache, -1, sizeof(s_programFilterCache));
        for (int i = 0; i < CACHE_SIZE; ++i) {
            s_objToWorldLocCache[i] = -2;
        }
    }

    void ClearTeammatePositions() {
        std::lock_guard<std::mutex> lock(s_teammatesMutex);
        s_teammatePositions.clear();
    }

    void ClearVehiclePositions() {
        std::lock_guard<std::mutex> lock(s_vehicleMutex);
        s_vehiclePositions.clear();
    }

    void RegisterVehicleInstance(void *instance) {
        if (!instance || !Memory::IsValidPtr(instance)) return;

        if (!s_get_transform) {
            s_get_transform = reinterpret_cast<void*(*)(void*)>(Memory::GetRVA(Offsets::Unity::GetTransform));
        }
        if (!s_Transform_get_position) {
            s_Transform_get_position = reinterpret_cast<Vector3(*)(void*)>(Memory::GetRVA(Offsets::Unity::TransformGetPosition));
        }
        if (!s_get_transform || !s_Transform_get_position) return;

        void *trans = s_get_transform(instance);
        if (!trans || !Memory::IsValidPtr(trans)) return;

        Vector3 pos = s_Transform_get_position(trans);
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) return;

        uint64_t now = GetCurrentTimeMs();
        std::lock_guard<std::mutex> lock(s_vehicleMutex);

        for (auto it = s_vehiclePositions.begin(); it != s_vehiclePositions.end(); ) {
            if (now - it->lastSeenMs > 3000) {
                it = s_vehiclePositions.erase(it);
            } else {
                ++it;
            }
        }

        for (auto &v : s_vehiclePositions) {
            float dx = v.position.x - pos.x;
            float dy = v.position.y - pos.y;
            float dz = v.position.z - pos.z;
            if ((dx * dx + dy * dy + dz * dz) < 4.0f) {
                v.position = pos;
                v.lastSeenMs = now;
                return;
            }
        }
        s_vehiclePositions.push_back({pos, now});
    }

    void ResetGLState() {
        glDepthRangef(0.0f, 1.0f);
        glDepthFunc(GL_LESS);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        glDisable(GL_BLEND);
    }

    void OnMatchFinished() {
        ClearProgramCache();
        ClearTeammatePositions();
        ClearVehiclePositions();
        ResetGLState();
    }

    // Check if the GL program belongs to static world/map/terrain geometry
    static inline bool IsMapGeometryProgram(GLint program) {
        if (program <= 0) return false;
        // Static map/terrain in Unity uses baked lightmapping or splat maps
        if (glGetUniformLocation(program, "unity_Lightmap") != -1) return true;
        if (glGetUniformLocation(program, "unity_LightmapST") != -1) return true;
        if (glGetUniformLocation(program, "unity_Lightmaps[0]") != -1) return true;
        if (glGetUniformLocation(program, "unity_LightmapsInd[0]") != -1) return true;
        if (glGetUniformLocation(program, "unity_DynamicLightmap") != -1) return true;
        if (glGetUniformLocation(program, "_Control") != -1) return true;
        if (glGetUniformLocation(program, "_TerrainHolesTexture") != -1) return true;
        if (glGetUniformLocation(program, "_Splat0") != -1) return true;
        return false;
    }

    // Check if the GL program belongs to 2D UI / Canvas / Text / Sprites (never render in Chams)
    static inline bool IsUIProgram(GLint program) {
        if (program <= 0) return false;
        // Strict 2D UI / Canvas / TextMeshPro uniforms
        if (glGetUniformLocation(program, "_ClipRect") != -1) return true;
        if (glGetUniformLocation(program, "_MaskTex") != -1) return true;
        if (glGetUniformLocation(program, "_UseUIAlphaClip") != -1) return true;
        if (glGetUniformLocation(program, "_TextureSampleAdd") != -1) return true;
        if (glGetUniformLocation(program, "_Stencil") != -1) return true;
        if (glGetUniformLocation(program, "_StencilComp") != -1) return true;
        if (glGetUniformLocation(program, "_StencilOp") != -1) return true;
        if (glGetUniformLocation(program, "_StencilReadMask") != -1) return true;
        if (glGetUniformLocation(program, "_StencilWriteMask") != -1) return true;
        if (glGetUniformLocation(program, "_ColorMask") != -1) return true;
        return false;
    }

    // Check if the GL program belongs specifically to vehicle track/tread/wheel materials
    static inline bool IsVehicleProgram(GLint program) {
        if (program <= 0) return false;
        // Strict vehicle-exclusive uniforms (never present on character or weapon shaders)
        if (glGetUniformLocation(program, "_Tread") != -1) return true;
        if (glGetUniformLocation(program, "_Treads") != -1) return true;
        if (glGetUniformLocation(program, "_TrackSpeed") != -1) return true;
        if (glGetUniformLocation(program, "_TrackOffset") != -1) return true;
        if (glGetUniformLocation(program, "_TreadOffset") != -1) return true;
        if (glGetUniformLocation(program, "_TrackTex") != -1) return true;
        if (glGetUniformLocation(program, "_TrackTexture") != -1) return true;
        if (glGetUniformLocation(program, "_WheelRadius") != -1) return true;
        if (glGetUniformLocation(program, "_WheelRotation") != -1) return true;
        if (glGetUniformLocation(program, "_Skidmark") != -1) return true;
        if (glGetUniformLocation(program, "_SkidmarkTex") != -1) return true;
        return false;
    }

    // Check if the GL program uses skeletal bone matrices for GPU skinning (Character Bot / Player)
    static inline bool HasBoneSkinning(GLint program) {
        if (program <= 0) return false;
        // Standard Unity & HLSLcc bone matrix arrays
        if (glGetUniformLocation(program, "_Bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "_Bones") != -1) return true;
        if (glGetUniformLocation(program, "hlslcc_mtx4x4_Bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "hlslcc_mtx4x4_Bones") != -1) return true;
        if (glGetUniformLocation(program, "unity_SkinMatrices") != -1) return true;
        if (glGetUniformLocation(program, "unity_SkinMatrices[0]") != -1) return true;
        if (glGetUniformLocation(program, "unity_BoneMatrices") != -1) return true;
        if (glGetUniformLocation(program, "unity_BoneMatrices[0]") != -1) return true;
        if (glGetUniformLocation(program, "hlslcc_mtx4x4unity_SkinMatrices[0]") != -1) return true;
        if (glGetUniformLocation(program, "hlslcc_mtx4x4unity_SkinMatrices") != -1) return true;
        if (glGetUniformLocation(program, "hlslcc_mtx4x4unity_BoneMatrices[0]") != -1) return true;
        if (glGetUniformLocation(program, "hlslcc_mtx4x4unity_BoneMatrices") != -1) return true;
        if (glGetUniformLocation(program, "_BoneMatrices[0]") != -1) return true;
        if (glGetUniformLocation(program, "_BoneMatrices") != -1) return true;
        if (glGetUniformLocation(program, "_SkinBones[0]") != -1) return true;
        if (glGetUniformLocation(program, "_SkinBones") != -1) return true;
        if (glGetUniformLocation(program, "_BoneTransforms[0]") != -1) return true;
        if (glGetUniformLocation(program, "_BoneTransforms") != -1) return true;
        if (glGetUniformLocation(program, "_Bones4x3[0]") != -1) return true;
        if (glGetUniformLocation(program, "_Bones4x3") != -1) return true;
        if (glGetUniformLocation(program, "unity_Bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "unity_Bones") != -1) return true;
        if (glGetUniformLocation(program, "g_bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "g_bones") != -1) return true;
        if (glGetUniformLocation(program, "bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "bones") != -1) return true;
        if (glGetUniformLocation(program, "u_bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "u_bones") != -1) return true;
        return false;
    }

    // Determine if the current mesh should be rendered in Chams
    static inline bool ShouldRenderMesh(GLint program, int filter) {
        if (program <= 0) return false;
        if (filter < 0 || filter > 7) filter = 0;

        if (program < CACHE_SIZE) {
            int8_t cached = s_programFilterCache[filter][program];
            if (cached != -1) {
                return cached == 1;
            }
        }

        ShaderFilter sf = static_cast<ShaderFilter>(filter);
        bool shouldRender = false;

        // Anti-map, Anti-vehicle, and Anti-UI shader rules:
        bool isMap = (sf != ShaderFilter::AllMeshes) && IsMapGeometryProgram(program);
        bool isVehicle = (sf != ShaderFilter::AllMeshes) && IsVehicleProgram(program);
        bool isUI = (sf != ShaderFilter::AllMeshes) && IsUIProgram(program);

        if (!isMap && !isVehicle && !isUI) {
            switch (sf) {
                case ShaderFilter::BodyBonesAuto: {
                    // Priority 1: GPU Skinning Bone Matrices (Characters with skeletal rig)
                    if (HasBoneSkinning(program)) {
                        shouldRender = true;
                        break;
                    }
                    // Priority 2: Character detail shaders (Normal Map / BumpMap, Specular Shininess, Emission)
                    if (glGetUniformLocation(program, "_BumpMap") != -1 ||
                        glGetUniformLocation(program, "_Shininess") != -1 ||
                        glGetUniformLocation(program, "_EmisColor") != -1 ||
                        glGetUniformLocation(program, "_Cutoff") != -1) {
                        shouldRender = true;
                        break;
                    }
                    // Priority 3: Dynamic entity Color or MainTex that is strictly NON-MAP
                    if (glGetUniformLocation(program, "_Color") != -1 ||
                        glGetUniformLocation(program, "_MainTex") != -1) {
                        shouldRender = true;
                        break;
                    }
                    break;
                }

                case ShaderFilter::BonesGPU: {
                    shouldRender = HasBoneSkinning(program);
                    break;
                }

                case ShaderFilter::BumpMap: {
                    shouldRender = (glGetUniformLocation(program, "_BumpMap") != -1);
                    break;
                }

                case ShaderFilter::Shininess: {
                    shouldRender = (glGetUniformLocation(program, "_Shininess") != -1);
                    break;
                }

                case ShaderFilter::EmisColor: {
                    shouldRender = (glGetUniformLocation(program, "_EmisColor") != -1);
                    break;
                }

                case ShaderFilter::ColorNonMap: {
                    shouldRender = (glGetUniformLocation(program, "_Color") != -1);
                    break;
                }

                case ShaderFilter::MainTexNonMap: {
                    shouldRender = (glGetUniformLocation(program, "_MainTex") != -1);
                    break;
                }

                case ShaderFilter::AllMeshes:
                default:
                    shouldRender = true;
                    break;
            }
        }

        if (program < CACHE_SIZE) {
            s_programFilterCache[filter][program] = shouldRender ? 1 : 0;
        }

        return shouldRender;
    }

    // Check if the 3D position is near an active vehicle (tank, car, etc.)
    static inline bool IsPositionNearVehicle(float cx1, float cy1, float cz1, float cx2, float cy2, float cz2) {
        std::lock_guard<std::mutex> lock(s_vehicleMutex);
        if (s_vehiclePositions.empty()) return false;

        constexpr float VEHICLE_RADIUS_SQ = 6.5f * 6.5f;
        for (const auto &v : s_vehiclePositions) {
            float dx1 = cx1 - v.position.x, dy1 = cy1 - v.position.y, dz1 = cz1 - v.position.z;
            if ((dx1 * dx1 + dy1 * dy1 + dz1 * dz1) <= VEHICLE_RADIUS_SQ) return true;

            float dx2 = cx2 - v.position.x, dy2 = cy2 - v.position.y, dz2 = cz2 - v.position.z;
            if ((dx2 * dx2 + dy2 * dy2 + dz2 * dz2) <= VEHICLE_RADIUS_SQ) return true;
        }
        return false;
    }

    // Check if the 3D position belongs to a teammate or local player
    static inline bool IsPositionNearTeammate(float cx1, float cy1, float cz1, float cx2, float cy2, float cz2) {
        std::lock_guard<std::mutex> lock(s_teammatesMutex);
        if (s_teammatePositions.empty()) return false;

        constexpr float MAX_DIST_SQ = 2.8f * 2.8f; // ~2.8m radius to account for character bone offsets
        for (const auto &tp : s_teammatePositions) {
            float dx1 = cx1 - tp.x, dy1 = cy1 - tp.y, dz1 = cz1 - tp.z;
            if ((dx1 * dx1 + dy1 * dy1 + dz1 * dz1) <= MAX_DIST_SQ) return true;

            float dx2 = cx2 - tp.x, dy2 = cy2 - tp.y, dz2 = cz2 - tp.z;
            if ((dx2 * dx2 + dy2 * dy2 + dz2 * dz2) <= MAX_DIST_SQ) return true;
        }
        return false;
    }

    static void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
        // Draw standard geometry first
        if (old_glDrawElements) {
            old_glDrawElements(mode, count, type, indices);
        }

        if (!s_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        // Only target character 3D meshes:
        // Character body parts and dolls in Unity use GL_TRIANGLES with 1000 <= count <= 32000.
        // Massive map terrain chunks (>32000) and small UI/particles (<1000) are skipped.
        if (mode != GL_TRIANGLES || count < 1000 || count > 32000) {
            return;
        }

        // Retrieve active OpenGL ES program
        GLint currProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);
        if (currProgram == 0) {
            return;
        }

        int filter = s_shaderFilter.load(std::memory_order_relaxed);
        if (!ShouldRenderMesh(currProgram, filter)) {
            return;
        }

        // Spatial matrix checks: filter out vehicle meshes, and filter out friendly teammates / local player
        bool checkTeammates = s_enemyOnly.load(std::memory_order_relaxed);
        bool hasVehicles = false;
        {
            std::lock_guard<std::mutex> lock(s_vehicleMutex);
            hasVehicles = !s_vehiclePositions.empty();
        }

        if (checkTeammates || hasVehicles) {
            GLint objLoc = -1;
            if (currProgram < CACHE_SIZE) {
                objLoc = s_objToWorldLocCache[currProgram];
                if (objLoc == -2) {
                    objLoc = glGetUniformLocation(currProgram, "hlslcc_mtx4x4unity_ObjectToWorld[0]");
                    if (objLoc == -1) {
                        objLoc = glGetUniformLocation(currProgram, "unity_ObjectToWorld");
                    }
                    s_objToWorldLocCache[currProgram] = static_cast<int16_t>(objLoc);
                }
            } else {
                objLoc = glGetUniformLocation(currProgram, "hlslcc_mtx4x4unity_ObjectToWorld[0]");
                if (objLoc == -1) {
                    objLoc = glGetUniformLocation(currProgram, "unity_ObjectToWorld");
                }
            }

            if (objLoc != -1) {
                GLfloat m[16];
                glGetUniformfv(currProgram, objLoc, m);

                float cx1 = m[12], cy1 = m[13], cz1 = m[14];
                float cx2 = m[3],  cy2 = m[7],  cz2 = m[11];

                // 1. Filter out vehicles (tanks, cars, etc.)
                if (hasVehicles && IsPositionNearVehicle(cx1, cy1, cz1, cx2, cy2, cz2)) {
                    return;
                }

                // 2. Filter out teammates & local player (enemy-only Chams filtering)
                if (checkTeammates && IsPositionNearTeammate(cx1, cy1, cz1, cx2, cy2, cz2)) {
                    return;
                }
            }
        }

        int currentMode = s_mode.load(std::memory_order_relaxed);
        float red = s_r.load(std::memory_order_relaxed);
        float green = s_g.load(std::memory_order_relaxed);
        float blue = s_b.load(std::memory_order_relaxed);
        float alpha = s_a.load(std::memory_order_relaxed);
        int lineW = s_lineWidth.load(std::memory_order_relaxed);

        float rNorm = red / 255.0f;
        float gNorm = green / 255.0f;
        float bNorm = blue / 255.0f;
        float aNorm = alpha / 255.0f;

        // Save complete OpenGL ES state before applying Chams styling
        GLboolean origBlend = glIsEnabled(GL_BLEND);
        GLboolean origDepthTest = glIsEnabled(GL_DEPTH_TEST);
        GLboolean origCullFace = glIsEnabled(GL_CULL_FACE);

        GLint origBlendSrcRGB = GL_ONE;
        GLint origBlendDstRGB = GL_ZERO;
        GLint origBlendSrcAlpha = GL_ONE;
        GLint origBlendDstAlpha = GL_ZERO;
        GLint origBlendEquationRGB = GL_FUNC_ADD;
        GLint origBlendEquationAlpha = GL_FUNC_ADD;
        GLint origDepthFunc = GL_LESS;
        GLboolean origColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        GLfloat origDepthRange[2] = {0.0f, 1.0f};
        GLfloat origBlendColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        GLfloat origLineWidth = 1.0f;

        glGetIntegerv(GL_BLEND_SRC_RGB, &origBlendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &origBlendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &origBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &origBlendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &origBlendEquationRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &origBlendEquationAlpha);
        glGetIntegerv(GL_DEPTH_FUNC, &origDepthFunc);
        glGetBooleanv(GL_COLOR_WRITEMASK, origColorMask);
        glGetFloatv(GL_DEPTH_RANGE, origDepthRange);
        glGetFloatv(GL_BLEND_COLOR, origBlendColor);
        glGetFloatv(GL_LINE_WIDTH, &origLineWidth);

        // Apply OpenGL ES state configuration matching esp-chams-template
        switch (static_cast<Mode>(currentMode)) {
            case Mode::Shading: {
                // Shading / Wallhack: Force depth test to pass from any angle (render through walls)
                glDepthRangef(1.0f, 0.5f);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_COLOR, GL_CONSTANT_COLOR);
                glBlendEquation(GL_FUNC_ADD);
                glBlendColor(rNorm, gNorm, bNorm, 1.0f);
                glDepthFunc(GL_ALWAYS);

                if (old_glDrawElements) old_glDrawElements(GL_TRIANGLES, count, type, indices);

                glColorMask(static_cast<GLboolean>(red > 0 ? 1 : 0),
                            static_cast<GLboolean>(green > 0 ? 1 : 0),
                            static_cast<GLboolean>(blue > 0 ? 1 : 0),
                            1);
                glBlendFunc(GL_DST_COLOR, GL_ONE);
                glDepthFunc(GL_LESS);
                glBlendColor(0.0f, 0.0f, 0.0f, 0.0f);
                break;
            }

            case Mode::Default: {
                // Default / Solid Color Chams through walls
                glDepthRangef(1.0f, 0.5f);
                glDepthFunc(GL_ALWAYS);
                glBlendColor(rNorm, gNorm, bNorm, aNorm);
                glColorMask(1, 1, 1, 1);
                glEnable(GL_BLEND);
                glBlendFunc(GL_CONSTANT_ALPHA, GL_CONSTANT_COLOR);

                if (old_glDrawElements) old_glDrawElements(GL_TRIANGLES, count, type, indices);
                break;
            }

            case Mode::Wireframe: {
                // Wireframe Chams: Render mesh edges as lines through walls
                glDepthRangef(1.0f, 0.5f);
                glDepthFunc(GL_ALWAYS);
                glBlendColor(rNorm, gNorm, bNorm, 1.0f);
                glColorMask(1, 1, 1, 1);
                glEnable(GL_BLEND);
                glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
                glLineWidth(static_cast<GLfloat>(lineW));

                if (old_glDrawElements) old_glDrawElements(GL_LINE_LOOP, count, type, indices);
                break;
            }

            case Mode::Glow: {
                // Glow Chams: Multi-pass lines + triangles
                glEnable(GL_BLEND);
                glBlendColor(rNorm, gNorm, bNorm, 1.0f);
                glColorMask(1, 1, 1, 1);
                glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
                glLineWidth(static_cast<GLfloat>(lineW));

                glDepthRangef(0.5f, 1.0f);
                if (old_glDrawElements) old_glDrawElements(GL_LINES, count, type, indices);

                glBlendColor(1.0f, 1.0f, 1.0f, 1.0f);
                glDepthRangef(1.0f, 0.5f);
                glDepthFunc(GL_ALWAYS);
                if (old_glDrawElements) old_glDrawElements(GL_TRIANGLES, count, type, indices);
                break;
            }

            case Mode::Outline: {
                // Outline Chams: Triangles base + wireframe outline
                glDepthRangef(1.0f, 0.5f);
                glDepthFunc(GL_ALWAYS);
                glLineWidth(static_cast<GLfloat>(lineW));
                glEnable(GL_BLEND);
                glColorMask(1, 1, 1, 1);
                glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
                glBlendColor(0.0f, 0.0f, 0.0f, 1.0f);
                if (old_glDrawElements) old_glDrawElements(GL_TRIANGLES, count, type, indices);

                glBlendColor(rNorm, gNorm, bNorm, 1.0f);
                if (old_glDrawElements) old_glDrawElements(GL_LINES, count, type, indices);
                break;
            }

            case Mode::Rainbow: {
                // Rainbow Chams: Smooth RGB cycling
                if (s_rainbowStage == 0) {
                    s_rainbowG += 5.0f;
                    if (s_rainbowG >= 255.0f) { s_rainbowG = 255.0f; s_rainbowStage = 1; }
                } else if (s_rainbowStage == 1) {
                    s_rainbowR -= 5.0f;
                    if (s_rainbowR <= 0.0f) { s_rainbowR = 0.0f; s_rainbowStage = 2; }
                } else if (s_rainbowStage == 2) {
                    s_rainbowB += 5.0f;
                    if (s_rainbowB >= 255.0f) { s_rainbowB = 255.0f; s_rainbowStage = 3; }
                } else if (s_rainbowStage == 3) {
                    s_rainbowG -= 5.0f;
                    if (s_rainbowG <= 0.0f) { s_rainbowG = 0.0f; s_rainbowStage = 4; }
                } else if (s_rainbowStage == 4) {
                    s_rainbowR += 5.0f;
                    if (s_rainbowR >= 255.0f) { s_rainbowR = 255.0f; s_rainbowStage = 5; }
                } else if (s_rainbowStage == 5) {
                    s_rainbowB -= 5.0f;
                    if (s_rainbowB <= 0.0f) { s_rainbowB = 0.0f; s_rainbowStage = 0; }
                }

                glDepthRangef(1.0f, 0.5f);
                glDepthFunc(GL_ALWAYS);
                glBlendColor(s_rainbowR / 255.0f, s_rainbowG / 255.0f, s_rainbowB / 255.0f, aNorm);
                glColorMask(1, 1, 1, 1);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE_MINUS_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_ALPHA);
                if (old_glDrawElements) old_glDrawElements(GL_TRIANGLES, count, type, indices);
                break;
            }
        }

        // Full clean restoration of all OpenGL ES states so UI, lobby viewports, world terrain and sky are 100% unaffected
        glDepthRangef(origDepthRange[0], origDepthRange[1]);
        glDepthFunc(origDepthFunc);
        glColorMask(origColorMask[0], origColorMask[1], origColorMask[2], origColorMask[3]);
        glBlendFuncSeparate(origBlendSrcRGB, origBlendDstRGB, origBlendSrcAlpha, origBlendDstAlpha);
        glBlendEquationSeparate(origBlendEquationRGB, origBlendEquationAlpha);
        glBlendColor(origBlendColor[0], origBlendColor[1], origBlendColor[2], origBlendColor[3]);
        glLineWidth(origLineWidth);

        if (origBlend) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }

        if (origDepthTest) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }

        if (origCullFace) {
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }
    }

    void Init() {
        ClearProgramCache();

        void *glesHandle = dlopen("libGLESv2.so", RTLD_LAZY);
        if (!glesHandle) {
            glesHandle = dlopen("libGLESv3.so", RTLD_LAZY);
        }

        void *drawElementsSym = nullptr;
        if (glesHandle) {
            drawElementsSym = dlsym(glesHandle, "glDrawElements");
        } else {
            drawElementsSym = dlsym(RTLD_DEFAULT, "glDrawElements");
        }

        if (drawElementsSym) {
#if defined(__aarch64__)
            A64HookFunction(drawElementsSym, reinterpret_cast<void*>(hook_glDrawElements), reinterpret_cast<void**>(&old_glDrawElements));
#else
            MSHookFunction(drawElementsSym, reinterpret_cast<void*>(hook_glDrawElements), reinterpret_cast<void**>(&old_glDrawElements));
#endif
            LOG_INFO("[CHAMS] glDrawElements hooked successfully at %p (old=%p)", drawElementsSym, old_glDrawElements);
        } else {
            LOG_WARN("[CHAMS] Failed to resolve glDrawElements symbol from GLES!");
        }
    }

    void SetEnabled(bool enable) {
        s_enabled.store(enable, std::memory_order_relaxed);
        LOG_INFO("[CHAMS] Master toggle: %s", enable ? "ON" : "OFF");
    }

    bool IsEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

    void SetEnemyOnly(bool enemyOnly) {
        s_enemyOnly.store(enemyOnly, std::memory_order_relaxed);
        LOG_INFO("[CHAMS] Enemy only filter: %s", enemyOnly ? "ON (Enemies Only)" : "OFF (All)");
    }

    bool IsEnemyOnly() {
        return s_enemyOnly.load(std::memory_order_relaxed);
    }

    void UpdateTeammatePositions(const Vector3 *positions, size_t count) {
        std::lock_guard<std::mutex> lock(s_teammatesMutex);
        s_teammatePositions.clear();
        if (positions && count > 0) {
            s_teammatePositions.insert(s_teammatePositions.end(), positions, positions + count);
        }
    }

    void SetMode(int mode) {
        s_mode.store(mode, std::memory_order_relaxed);
        LOG_INFO("[CHAMS] Mode set to %d", mode);
    }

    int GetMode() {
        return s_mode.load(std::memory_order_relaxed);
    }

    void SetShaderFilter(int filter) {
        s_shaderFilter.store(filter, std::memory_order_relaxed);
        ClearProgramCache();
        LOG_INFO("[CHAMS] Shader filter set to %d", filter);
    }

    int GetShaderFilter() {
        return s_shaderFilter.load(std::memory_order_relaxed);
    }

    void SetColor(float r, float g, float b, float a) {
        s_r.store(r, std::memory_order_relaxed);
        s_g.store(g, std::memory_order_relaxed);
        s_b.store(b, std::memory_order_relaxed);
        s_a.store(a, std::memory_order_relaxed);
    }

    void SetR(int r) {
        s_r.store(static_cast<float>(r), std::memory_order_relaxed);
    }

    void SetG(int g) {
        s_g.store(static_cast<float>(g), std::memory_order_relaxed);
    }

    void SetB(int b) {
        s_b.store(static_cast<float>(b), std::memory_order_relaxed);
    }

    void SetLineWidth(int width) {
        s_lineWidth.store(width, std::memory_order_relaxed);
    }

} // namespace Chams
