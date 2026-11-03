// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/render/picking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace scuba::render {
namespace {

Float3 TransformPoint(Float3 p, const Float4x4& matrix) {
    return {p.x * matrix.m[0][0] + p.y * matrix.m[1][0] + p.z * matrix.m[2][0] + matrix.m[3][0],
            p.x * matrix.m[0][1] + p.y * matrix.m[1][1] + p.z * matrix.m[2][1] + matrix.m[3][1],
            p.x * matrix.m[0][2] + p.y * matrix.m[1][2] + p.z * matrix.m[2][2] + matrix.m[3][2]};
}

Float3 TransformVector(Float3 p, const Float4x4& matrix) {
    return {p.x * matrix.m[0][0] + p.y * matrix.m[1][0] + p.z * matrix.m[2][0],
            p.x * matrix.m[0][1] + p.y * matrix.m[1][1] + p.z * matrix.m[2][1],
            p.x * matrix.m[0][2] + p.y * matrix.m[1][2] + p.z * matrix.m[2][2]};
}

bool Finite(Float3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool HitsBox(const Ray& r, const std::array<float, 3>& mn, const std::array<float, 3>& mx, float maxT) {
    float t0 = 0, t1 = maxT;
    const float o[3] = {r.origin.x, r.origin.y, r.origin.z}, d[3] = {r.dir.x, r.dir.y, r.dir.z};
    for (size_t i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
            continue;
        }
        float a = (mn[i] - o[i]) / d[i], b = (mx[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

// Moller-Trumbore, both faces.
bool HitsTriangle(const Ray& r, const float* a, const float* b, const float* c, float& t) {
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float d[3] = {r.dir.x, r.dir.y, r.dir.z};
    const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const float s[3] = {r.origin.x - a[0], r.origin.y - a[1], r.origin.z - a[2]};
    const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0 || u > 1) return false;
    const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0 || u + v > 1) return false;
    t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return t > 0;
}

}  // namespace

std::optional<PickHit> Pick(const Ray& ray, std::span<const PickTarget> targets) {
    if (!Finite(ray.origin) || !Finite(ray.dir)) return std::nullopt;
    const float directionLength = std::sqrt(ray.dir.x * ray.dir.x + ray.dir.y * ray.dir.y + ray.dir.z * ray.dir.z);
    if (!std::isfinite(directionLength) || directionLength <= 1e-20f) return std::nullopt;
    const Ray worldRay{ray.origin, {ray.dir.x / directionLength, ray.dir.y / directionLength, ray.dir.z / directionLength}};
    std::optional<PickHit> best;
    float bestT = std::numeric_limits<float>::max();
    for (const auto& target : targets) {
        if (!target.model) continue;
        Float4x4 inverse;
        if (!InvertAffine(target.world, inverse)) continue;
        const Ray local{TransformPoint(worldRay.origin, inverse), TransformVector(worldRay.dir, inverse)};
        const float localDirectionLengthSquared = local.dir.x * local.dir.x + local.dir.y * local.dir.y +
                                                  local.dir.z * local.dir.z;
        if (!Finite(local.origin) || !Finite(local.dir) || !std::isfinite(localDirectionLengthSquared) ||
            localDirectionLengthSquared <= 1e-30f) continue;
        if (!HitsBox(local, target.model->boundsMin, target.model->boundsMax, bestT)) continue;
        for (const auto& mesh : target.model->meshes) {
            if (mesh.positions.size() % 3 != 0) continue;
            if (!HitsBox(local, mesh.boundsMin, mesh.boundsMax, bestT)) continue;
            const float* pos = mesh.positions.data();
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                if (mesh.indices[i] >= mesh.positions.size() / 3 || mesh.indices[i + 1] >= mesh.positions.size() / 3 ||
                    mesh.indices[i + 2] >= mesh.positions.size() / 3) continue;
                float t = 0;
                if (HitsTriangle(local, pos + mesh.indices[i] * 3, pos + mesh.indices[i + 1] * 3, pos + mesh.indices[i + 2] * 3, t) && t < bestT) {
                    bestT = t;
                    best = PickHit{target.id, t, {worldRay.origin.x + worldRay.dir.x * t,
                                                   worldRay.origin.y + worldRay.dir.y * t,
                                                   worldRay.origin.z + worldRay.dir.z * t}};
                }
            }
        }
    }
    return best;
}

std::optional<Float3> RayPlaneZ(const Ray& ray, float height) {
    if (std::fabs(ray.dir.z) < 1e-6f) return std::nullopt;
    const float t = (height - ray.origin.z) / ray.dir.z;
    if (t <= 0) return std::nullopt;
    return Float3{ray.origin.x + ray.dir.x * t, ray.origin.y + ray.dir.y * t, height};
}

}  // namespace scuba::render
