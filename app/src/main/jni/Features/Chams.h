#ifndef FEATURE_CHAMS_H
#define FEATURE_CHAMS_H

#include <cstdint>

namespace Chams {

    enum class Mode {
        Shading   = 0, // Tembus Pandang / Wallhack (glDepthFunc GL_ALWAYS)
        Default   = 1, // Solid / Constant Color
        Wireframe = 2, // Line Loop Wireframe
        Glow      = 3, // Multi-pass glow effect
        Outline   = 4, // Outline wireframe border
        Rainbow   = 5  // Dynamic animated color cycling
    };

    enum class ShaderFilter {
        MainTex   = 0, // _MainTex (Character models)
        Color     = 1, // _Color
        MainTexST = 2, // _MainTex_ST
        All       = 3  // All shaders without uniform filter
    };

    void Init();

    void SetEnabled(bool enable);
    bool IsEnabled();

    void SetMode(int mode);
    int GetMode();

    void SetShaderFilter(int filter);
    int GetShaderFilter();

    void SetColor(float r, float g, float b, float a = 255.0f);
    void SetR(int r);
    void SetG(int g);
    void SetB(int b);
    void SetLineWidth(int width);

} // namespace Chams

#endif // FEATURE_CHAMS_H
