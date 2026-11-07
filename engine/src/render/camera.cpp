// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/render/camera.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

bool FiniteMatrix(const Float4x4& matrix) {
    for (const auto& row : matrix.m)
        for (const float value : row)
            if (!std::isfinite(value)) return false;
    return true;
}

bool AffineShape(const Float4x4& matrix) {
    constexpr float tolerance = 1e-6f;
    return FiniteMatrix(matrix) && std::fabs(matrix.m[0][3]) <= tolerance &&
           std::fabs(matrix.m[1][3]) <= tolerance && std::fabs(matrix.m[2][3]) <= tolerance &&
           std::fabs(matrix.m[3][3] - 1.0f) <= tolerance;
}

bool InvertLinear(const Float4x4& matrix, double inverse[3][3]) {
    if (!AffineShape(matrix)) return false;
    const double a = matrix.m[0][0], b = matrix.m[0][1], c = matrix.m[0][2];
    const double d = matrix.m[1][0], e = matrix.m[1][1], f = matrix.m[1][2];
    const double g = matrix.m[2][0], h = matrix.m[2][1], i = matrix.m[2][2];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const double norm = std::max({std::fabs(a), std::fabs(b), std::fabs(c), std::fabs(d),
                                  std::fabs(e), std::fabs(f), std::fabs(g), std::fabs(h), std::fabs(i)});
    if (!std::isfinite(determinant) || norm == 0 || std::fabs(determinant) <= 1e-12 * norm * norm * norm)
        return false;
    const double invDet = 1.0 / determinant;
    inverse[0][0] = (e * i - f * h) * invDet;
    inverse[0][1] = (c * h - b * i) * invDet;
    inverse[0][2] = (b * f - c * e) * invDet;
    inverse[1][0] = (f * g - d * i) * invDet;
    inverse[1][1] = (a * i - c * g) * invDet;
    inverse[1][2] = (c * d - a * f) * invDet;
    inverse[2][0] = (d * h - e * g) * invDet;
    inverse[2][1] = (b * g - a * h) * invDet;
    inverse[2][2] = (a * e - b * d) * invDet;
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            if (!std::isfinite(inverse[row][column]) ||
                std::fabs(inverse[row][column]) > std::numeric_limits<float>::max()) return false;
    return true;
}

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

bool AffineFromTrs(Float3 translation, Float4 rotation, Float3 scale, Float4x4& out) {
    const float input[10] = {translation.x, translation.y, translation.z, rotation.x, rotation.y,
                             rotation.z, rotation.w, scale.x, scale.y, scale.z};
    for (const float value : input)
        if (!std::isfinite(value)) return false;
    if (scale.x == 0 || scale.y == 0 || scale.z == 0) return false;
    const double normSquared = static_cast<double>(rotation.x) * rotation.x +
                              static_cast<double>(rotation.y) * rotation.y +
                              static_cast<double>(rotation.z) * rotation.z +
                              static_cast<double>(rotation.w) * rotation.w;
    if (!std::isfinite(normSquared) || normSquared <= 1e-24) return false;
    const float invNorm = static_cast<float>(1.0 / std::sqrt(normSquared));
    const double x = static_cast<double>(rotation.x) * invNorm;
    const double y = static_cast<double>(rotation.y) * invNorm;
    const double z = static_cast<double>(rotation.z) * invNorm;
    const double w = static_cast<double>(rotation.w) * invNorm;
    const double xx = x * x, yy = y * y, zz = z * z;
    const double xy = x * y, xz = x * z, yz = y * z;
    const double xw = x * w, yw = y * w, zw = z * w;
    const double rows[3][3] = {
        {1.0 - 2.0 * (yy + zz), 2.0 * (xy + zw), 2.0 * (xz - yw)},
        {2.0 * (xy - zw), 1.0 - 2.0 * (xx + zz), 2.0 * (yz + xw)},
        {2.0 * (xz + yw), 2.0 * (yz - xw), 1.0 - 2.0 * (xx + yy)},
    };
    Float4x4 result{};
    const float factors[3] = {scale.x, scale.y, scale.z};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            const double value = static_cast<double>(factors[row]) * rows[row][column];
            if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max()) return false;
            result.m[row][column] = static_cast<float>(value);
        }
    }
    result.m[3][0] = translation.x;
    result.m[3][1] = translation.y;
    result.m[3][2] = translation.z;
    out = result;
    return true;
}

bool InvertAffine(const Float4x4& world, Float4x4& inverse) {
    double inverseLinear[3][3]{};
    if (!InvertLinear(world, inverseLinear)) return false;
    Float4x4 result{};
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            result.m[row][column] = static_cast<float>(inverseLinear[row][column]);
    for (size_t column = 0; column < 3; ++column) {
        const double value = -(static_cast<double>(world.m[3][0]) * inverseLinear[0][column] +
                               static_cast<double>(world.m[3][1]) * inverseLinear[1][column] +
                               static_cast<double>(world.m[3][2]) * inverseLinear[2][column]);
        if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max()) return false;
        result.m[3][column] = static_cast<float>(value);
    }
    inverse = result;
    return true;
}

bool NormalMatrix(const Float4x4& world, Float4x4& normal) {
    double inverseLinear[3][3]{};
    if (!InvertLinear(world, inverseLinear)) return false;
    Float4x4 result{};
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            result.m[row][column] = static_cast<float>(inverseLinear[column][row]);
    normal = result;
    return true;
}

bool TransformBounds(const std::array<float, 3>& localMin, const std::array<float, 3>& localMax,
                     const Float4x4& world, std::array<float, 3>& worldMin, std::array<float, 3>& worldMax) {
    if (!AffineShape(world)) return false;
    for (size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(localMin[axis]) || !std::isfinite(localMax[axis]) || localMin[axis] > localMax[axis]) return false;
    std::array<float, 3> mn{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    std::array<float, 3> mx{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const float p[3] = {
            (corner & 1u) ? localMax[0] : localMin[0],
            (corner & 2u) ? localMax[1] : localMin[1],
            (corner & 4u) ? localMax[2] : localMin[2],
        };
        for (size_t column = 0; column < 3; ++column) {
            const double value = static_cast<double>(p[0]) * world.m[0][column] +
                                 static_cast<double>(p[1]) * world.m[1][column] +
                                 static_cast<double>(p[2]) * world.m[2][column] + world.m[3][column];
            if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max()) return false;
            const float coordinate = static_cast<float>(value);
            mn[column] = std::min(mn[column], coordinate);
            mx[column] = std::max(mx[column], coordinate);
        }
    }
    worldMin = mn;
    worldMax = mx;
    return true;
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
    (void)TryFrame(mn, mx);
}

bool OrbitCamera::TryFrame(const std::array<float, 3>& mn, const std::array<float, 3>& mx, float aspect) {
    if (!std::isfinite(fovY) || fovY <= 0.0f || fovY >= 3.14159265f ||
        !std::isfinite(farZ) || farZ <= 1.0f || !std::isfinite(aspect) || aspect <= 0.0f)
        return false;
    for (size_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(mn[axis]) || !std::isfinite(mx[axis]) || mn[axis] > mx[axis]) return false;

    const double dx = static_cast<double>(mx[0]) - mn[0];
    const double dy = static_cast<double>(mx[1]) - mn[1];
    const double dz = static_cast<double>(mx[2]) - mn[2];
    const double radius = 0.5 * std::hypot(dx, dy, dz);
    const double verticalHalfFov = static_cast<double>(fovY) * 0.5;
    const double horizontalHalfFov = std::atan(std::tan(verticalHalfFov) * static_cast<double>(aspect));
    const double sinVerticalHalfFov = std::sin(verticalHalfFov);
    const double sinHorizontalHalfFov = std::sin(horizontalHalfFov);
    if (!std::isfinite(sinVerticalHalfFov) || sinVerticalHalfFov <= 0.0 ||
        !std::isfinite(sinHorizontalHalfFov) || sinHorizontalHalfFov <= 0.0)
        return false;
    const double framedDistance = std::max({radius / sinVerticalHalfFov * 1.1,
                                            radius / sinHorizontalHalfFov * 1.1, 0.5});
    const double maxDistance = static_cast<double>(farZ) * 0.5;
    if (!std::isfinite(framedDistance) || framedDistance > maxDistance) return false;

    std::array<float, 3> framedTarget{};
    for (size_t axis = 0; axis < 3; ++axis) {
        const double center = static_cast<double>(mn[axis]) +
                              (static_cast<double>(mx[axis]) - mn[axis]) * 0.5;
        if (!std::isfinite(center) || std::fabs(center) > std::numeric_limits<float>::max()) return false;
        framedTarget[axis] = static_cast<float>(center);
        const float nextUp = std::nextafter(framedTarget[axis], std::numeric_limits<float>::infinity());
        const float nextDown = std::nextafter(framedTarget[axis], -std::numeric_limits<float>::infinity());
        const double coordinateResolution = std::max(
            std::fabs(static_cast<double>(nextUp) - framedTarget[axis]),
            std::fabs(static_cast<double>(framedTarget[axis]) - nextDown));
        if (!std::isfinite(coordinateResolution) || coordinateResolution > framedDistance * 0.25)
            return false;
    }

    target = {framedTarget[0], framedTarget[1], framedTarget[2]};
    distance = static_cast<float>(framedDistance);
    return true;
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
