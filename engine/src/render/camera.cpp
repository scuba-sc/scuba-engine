// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/render/camera.h"

#include <algorithm>
#include <cmath>

namespace scuba::render {
namespace {

Float3 Sub(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 Add(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 Scale(Float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 Cross(Float3 a, Float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Float3 Normalize(Float3 a) {
    const float l = std::sqrt(Dot(a, a));
    return l > 1e-20f ? Scale(a, 1.0f / l) : Float3{0, 0, 1};
}

constexpr float kPitchLimit = 1.55f;

}  // namespace

Float4x4 Multiply(const Float4x4& a, const Float4x4& b) {
    Float4x4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

Float4x4 Transform::Matrix() const {
    const float c = std::cos(yaw) * scale, s = std::sin(yaw) * scale;
    Float4x4 m;
    m.m[0][0] = c;  m.m[0][1] = s;  m.m[0][2] = 0;
    m.m[1][0] = -s; m.m[1][1] = c;  m.m[1][2] = 0;
    m.m[2][0] = 0;  m.m[2][1] = 0;  m.m[2][2] = scale;
    m.m[3][0] = position.x; m.m[3][1] = position.y; m.m[3][2] = position.z;
    return m;
}

void OrbitCamera::Orbit(float dx, float dy) {
    yaw -= dx * 0.008f;
    pitch = std::clamp(pitch + dy * 0.008f, -kPitchLimit, kPitchLimit);
}

void OrbitCamera::Pan(float dx, float dy, float viewportHeight) {
    const Float3 eye = Eye();
    const Float3 forward = Normalize(Sub(target, eye));
    const Float3 right = Normalize(Cross(forward, {0, 0, 1}));
    const Float3 up = Cross(right, forward);
    const float worldPerPixel = 2.0f * distance * std::tan(fovY * 0.5f) / std::max(viewportHeight, 1.0f);
    target = Add(target, Add(Scale(right, -dx * worldPerPixel), Scale(up, dy * worldPerPixel)));
}

void OrbitCamera::Dolly(float wheelSteps) {
    distance = std::clamp(distance * std::pow(0.88f, wheelSteps), 0.05f, farZ * 0.5f);
}

void OrbitCamera::Frame(const std::array<float, 3>& mn, const std::array<float, 3>& mx) {
    target = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
    const float r = 0.5f * std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                                     (mx[2] - mn[2]) * (mx[2] - mn[2]));
    distance = std::max(r / std::sin(fovY * 0.5f) * 1.1f, 0.5f);
}

Float3 OrbitCamera::Eye() const {
    const float cp = std::cos(pitch);
    return Add(target, Float3{distance * cp * std::cos(yaw), distance * cp * std::sin(yaw), distance * std::sin(pitch)});
}

Float4x4 OrbitCamera::View() const {
    const Float3 eye = Eye();
    const Float3 f = Normalize(Sub(target, eye));
    const Float3 s = Normalize(Cross(f, {0, 0, 1}));
    const Float3 u = Cross(s, f);
    // Right-handed look-at for row vectors: columns are the camera axes, -f is +z.
    Float4x4 m;
    m.m[0][0] = s.x; m.m[0][1] = u.x; m.m[0][2] = -f.x; m.m[0][3] = 0;
    m.m[1][0] = s.y; m.m[1][1] = u.y; m.m[1][2] = -f.y; m.m[1][3] = 0;
    m.m[2][0] = s.z; m.m[2][1] = u.z; m.m[2][2] = -f.z; m.m[2][3] = 0;
    m.m[3][0] = -Dot(s, eye); m.m[3][1] = -Dot(u, eye); m.m[3][2] = Dot(f, eye); m.m[3][3] = 1;
    return m;
}

Float4x4 OrbitCamera::Projection(float aspect) const {
    // Right-handed perspective, depth 0 (near) .. 1 (far).
    const float y = 1.0f / std::tan(fovY * 0.5f), x = y / std::max(aspect, 1e-4f);
    const float range = farZ / (nearZ - farZ);
    Float4x4 m;
    m.m[0][0] = x; m.m[1][1] = y;
    m.m[2][2] = range; m.m[2][3] = -1;
    m.m[3][2] = range * nearZ; m.m[3][3] = 0;
    return m;
}

Ray OrbitCamera::ScreenRay(float px, float py, const Rect& vp) const {
    const float ndcX = (px - vp.x) / std::max(vp.w, 1.0f) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (py - vp.y) / std::max(vp.h, 1.0f) * 2.0f;
    const Float3 eye = Eye();
    const Float3 f = Normalize(Sub(target, eye));
    const Float3 s = Normalize(Cross(f, {0, 0, 1}));
    const Float3 u = Cross(s, f);
    const float th = std::tan(fovY * 0.5f), aspect = vp.w / std::max(vp.h, 1.0f);
    const Float3 dir = Normalize(Add(f, Add(Scale(s, ndcX * th * aspect), Scale(u, ndcY * th))));
    return {eye, dir};
}

}  // namespace scuba::render
