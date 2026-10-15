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
        BodyBonesAuto = 0, // Otomatis Body Bone (Anti-Map + Bones/BumpMap)
        BonesGPU      = 1, // Skeletal Bones (_Bones / GPU Skinning)
        BumpMap       = 2, // Karakter Normal Map (_BumpMap)
        Shininess     = 3, // Specular Karakter (_Shininess)
        EmisColor     = 4, // Emissive (_EmisColor)
        ColorNonMap   = 5, // Karakter Warna (_Color [Anti-Map])
        MainTexNonMap = 6, // Karakter Texture (_MainTex [Anti-Map])
        AllMeshes     = 7  // Semua Mesh (Map & Player)
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
