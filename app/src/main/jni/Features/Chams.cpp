#include "Chams.h"
#include "NotificationReward.h"
#include "../Core/Logger.h"

#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <vector>
#include <mutex>

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
    static int8_t s_programFilterCache[8][256];

    static inline void ClearProgramCache() {
        memset(s_programFilterCache, -1, sizeof(s_programFilterCache));
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

    // Check if the GL program uses skeletal bone matrices for GPU skinning
    static inline bool HasBoneSkinning(GLint program) {
        if (program <= 0) return false;
        if (glGetUniformLocation(program, "_Bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "_Bones") != -1) return true;
        if (glGetUniformLocation(program, "hlslcc_mtx4x4_Bones[0]") != -1) return true;
        if (glGetUniformLocation(program, "unity_SkinMatrices") != -1) return true;
        if (glGetUniformLocation(program, "unity_BoneMatrices") != -1) return true;
        return false;
    }

    // Determine if the current mesh should be rendered in Chams
    static inline bool ShouldRenderMesh(GLint program, int filter) {
        if (program <= 0) return false;
        if (filter < 0 || filter > 7) filter = 0;

        if (program < 256) {
            int8_t cached = s_programFilterCache[filter][program];
            if (cached != -1) {
                return cached == 1;
            }
        }

        ShaderFilter sf = static_cast<ShaderFilter>(filter);
        bool shouldRender = false;

        // Anti-map rule: Never render lightmapped static world objects unless explicitly in AllMeshes mode
        bool isMap = (sf != ShaderFilter::AllMeshes) && IsMapGeometryProgram(program);

        if (!isMap) {
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

        if (program < 256) {
            s_programFilterCache[filter][program] = shouldRender ? 1 : 0;
        }

        return shouldRender;
    }

    // Check if the currently drawn mesh belongs to a teammate or local player
    static inline bool IsCurrentMeshTeammate(GLint currProgram) {
        if (!s_enemyOnly.load(std::memory_order_relaxed)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(s_teammatesMutex);
        if (s_teammatePositions.empty()) {
            return false;
        }

        GLint loc = glGetUniformLocation(currProgram, "hlslcc_mtx4x4unity_ObjectToWorld[0]");
        if (loc == -1) {
            loc = glGetUniformLocation(currProgram, "unity_ObjectToWorld");
        }
        if (loc == -1) {
            return false;
        }

        GLfloat m[16];
        glGetUniformfv(currProgram, loc, m);

        // Candidate 1: Column-major translation (m[12], m[13], m[14])
        float cx1 = m[12], cy1 = m[13], cz1 = m[14];
        // Candidate 2: Row-major translation (m[3], m[7], m[11])
        float cx2 = m[3], cy2 = m[7], cz2 = m[11];

        constexpr float MAX_DIST_SQ = 2.8f * 2.8f; // ~2.8m radius to account for character bone offsets

        for (const auto &tp : s_teammatePositions) {
            float dx1 = cx1 - tp.x;
            float dy1 = cy1 - tp.y;
            float dz1 = cz1 - tp.z;
            if ((dx1 * dx1 + dy1 * dy1 + dz1 * dz1) <= MAX_DIST_SQ) {
                return true;
            }

            float dx2 = cx2 - tp.x;
            float dy2 = cy2 - tp.y;
            float dz2 = cz2 - tp.z;
            if ((dx2 * dx2 + dy2 * dy2 + dz2 * dz2) <= MAX_DIST_SQ) {
                return true;
            }
        }

        return false;
    }

    static void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
        // Draw standard geometry first
        if (old_glDrawElements) {
            old_glDrawElements(mode, count, type, indices);
        }

        // Process any queued notification reward events on the engine render thread
        NotificationReward::OnRenderFrame();

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

        // Filter out teammates and local player so only enemies are rendered in Chams
        if (IsCurrentMeshTeammate(currProgram)) {
            return;
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

        // Restore clean default OpenGL-ES state so UI, world terrain and sky are not affected
        glDepthRangef(0.5f, 1.0f);
        glDepthFunc(GL_LESS);
        glColorMask(1, 1, 1, 1);
        glDisable(GL_BLEND);
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
