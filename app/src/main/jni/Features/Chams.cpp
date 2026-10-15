#include "Chams.h"
#include "../Core/Logger.h"

#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <algorithm>

#if defined(__aarch64__)
#include "../And64InlineHook/And64InlineHook.hpp"
#else
#include "../Substrate/SubstrateHook.h"
#endif

namespace Chams {

    static std::atomic<bool> s_enabled{false};
    static std::atomic<int>  s_mode{static_cast<int>(Mode::Shading)}; // Default: Shading / Wallhack
    static std::atomic<int>  s_shaderFilter{static_cast<int>(ShaderFilter::MainTex)};

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

    static inline const char* GetShaderUniformName(int filter) {
        switch (static_cast<ShaderFilter>(filter)) {
            case ShaderFilter::MainTex:   return "_MainTex";
            case ShaderFilter::Color:     return "_Color";
            case ShaderFilter::MainTexST: return "_MainTex_ST";
            case ShaderFilter::All:
            default:                      return nullptr;
        }
    }

    static void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
        // Draw standard geometry first
        if (old_glDrawElements) {
            old_glDrawElements(mode, count, type, indices);
        }

        if (!s_enabled.load(std::memory_order_relaxed)) {
            return;
        }

        // Only target 3D meshes (character models in Unity use GL_TRIANGLES with count >= 600)
        // This prevents corrupting UI, minimap, text, and small particle elements
        if (mode != GL_TRIANGLES || count < 600) {
            return;
        }

        // If shader uniform filter is configured, verify program uses target uniform (e.g. _MainTex for characters)
        int filter = s_shaderFilter.load(std::memory_order_relaxed);
        const char *uniformName = GetShaderUniformName(filter);
        if (uniformName != nullptr) {
            GLint currProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);
            if (currProgram != 0) {
                GLint loc = glGetUniformLocation(currProgram, uniformName);
                if (loc == -1) {
                    return; // Not a character/mesh shader matching the uniform
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

    void SetMode(int mode) {
        s_mode.store(mode, std::memory_order_relaxed);
        LOG_INFO("[CHAMS] Mode set to %d", mode);
    }

    int GetMode() {
        return s_mode.load(std::memory_order_relaxed);
    }

    void SetShaderFilter(int filter) {
        s_shaderFilter.store(filter, std::memory_order_relaxed);
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
