// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// CPU picking against the CPU copy of each model: ray vs instance bounds, then exact triangles.
// Phase 1 has no GPU id buffer; scenes are editor-sized, and the meshes stay resident on the CPU
// for the importer anyway.
#include "scuba/asset/model.h"
#include "scuba/render/camera.h"

#include <cstdint>
#include <optional>
#include <span>

namespace scuba::render {

struct PickTarget {
    uint32_t id = 0;                      // caller's id (scene instance)
    const asset::ModelData* model = nullptr;
    Transform transform;
};

struct PickHit {
    uint32_t id = 0;
    float distance = 0;
    Float3 point;
};

std::optional<PickHit> Pick(const Ray& ray, std::span<const PickTarget> targets);

// Where the ray meets the plane z = height, if in front of it.
std::optional<Float3> RayPlaneZ(const Ray& ray, float height);

}  // namespace scuba::render
