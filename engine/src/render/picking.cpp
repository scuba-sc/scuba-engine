// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/render/picking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace scuba::render {
namespace {

// The ray in the model's local space: undo translation, yaw and uniform scale.
Ray ToLocal(const Ray& r, const Transform& t, float& scaleOut) {
    const float s = t.scale != 0 ? t.scale : 1.0f;
    const float c = std::cos(-t.yaw), sn = std::sin(-t.yaw);
    auto local = [&](Float3 p, bool point) {
        if (point) p = {p.x - t.position.x, p.y - t.position.y, p.z - t.position.z};
        Float3 q{p.x * c - p.y * sn, p.x * sn + p.y * c, p.z};
        return Float3{q.x / s, q.y / s, q.z / s};
    };
    scaleOut = s;
    Ray out{local(r.origin, true), local(r.dir, false)};   // dir keeps length 1/s: t stays world distance
    return out;
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
    std::optional<PickHit> best;
    float bestT = std::numeric_limits<float>::max();
    for (const auto& target : targets) {
        if (!target.model) continue;
        float scale = 1;
        const Ray local = ToLocal(ray, target.transform, scale);
        if (!HitsBox(local, target.model->boundsMin, target.model->boundsMax, bestT)) continue;
        for (const auto& mesh : target.model->meshes) {
            if (!HitsBox(local, mesh.boundsMin, mesh.boundsMax, bestT)) continue;
            const float* pos = mesh.positions.data();
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                float t = 0;
                if (HitsTriangle(local, pos + mesh.indices[i] * 3, pos + mesh.indices[i + 1] * 3, pos + mesh.indices[i + 2] * 3, t) && t < bestT) {
                    bestT = t;
                    best = PickHit{target.id, t, {ray.origin.x + ray.dir.x * t, ray.origin.y + ray.dir.y * t, ray.origin.z + ray.dir.z * t}};
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
