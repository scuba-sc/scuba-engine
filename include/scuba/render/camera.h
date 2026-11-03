// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// View math shared by the renderer, picking and the editor. Right-handed, Z up (the sco-core SDK
// convention), row-vector matrices (v * M), stored row-major: the layout the HLSL side reads.
#include <array>
#include <cstdint>

namespace scuba::render {

struct Float3 {
    float x = 0, y = 0, z = 0;
};
struct Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};
struct Float4x4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

struct Ray {
    Float3 origin, dir;   // dir is unit length
};

// A rectangle in client pixels (the 3D viewport inside the docked editor).
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// Translation, rotation about Z, uniform scale: enough for placing imported models.
struct Transform {
    Float3 position;
    float yaw = 0;      // radians about +Z
    float scale = 1;
    Float4x4 Matrix() const;
};

Float4x4 Multiply(const Float4x4& a, const Float4x4& b);

// Build and operate on affine row-vector transforms. Inverse/normal calculations refuse singular,
// non-affine, non-finite, or numerically ill-conditioned matrices.
bool AffineFromTrs(Float3 translation, Float4 rotation, Float3 scale, Float4x4& out);
bool InvertAffine(const Float4x4& world, Float4x4& inverse);
bool NormalMatrix(const Float4x4& world, Float4x4& normal);
bool TransformBounds(const std::array<float, 3>& localMin, const std::array<float, 3>& localMax,
                    const Float4x4& world, std::array<float, 3>& worldMin, std::array<float, 3>& worldMax);

class OrbitCamera {
public:
    Float3 target;
    float distance = 6.0f;
    float yaw = 0.8f;     // radians about +Z
    float pitch = 0.45f;  // radians above the XY plane
    float fovY = 0.9f, nearZ = 0.05f, farZ = 10000.0f;

    void Orbit(float dxPixels, float dyPixels);
    void Pan(float dxPixels, float dyPixels, float viewportHeight);
    void Dolly(float wheelSteps);
    // Existing convenience API; invalid bounds leave the camera unchanged.
    void Frame(const std::array<float, 3>& boundsMin, const std::array<float, 3>& boundsMax);
    // Focuses the camera on finite, ordered bounds. Point/flat bounds are valid and use the
    // minimum viewing distance; invalid or out-of-range bounds leave the camera unchanged.
    bool TryFrame(const std::array<float, 3>& boundsMin, const std::array<float, 3>& boundsMax);

    Float3 Eye() const;
    Float4x4 View() const;
    Float4x4 Projection(float aspect) const;
    // The world ray through pixel (px, py) of viewport.
    Ray ScreenRay(float px, float py, const Rect& viewport) const;
};

}  // namespace scuba::render
