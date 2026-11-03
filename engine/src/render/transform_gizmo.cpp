// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/render/transform_gizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace scuba::render {
namespace {

bool Finite(Float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Normalize(Float3 value, Float3& normalized) {
    if (!Finite(value)) return false;
    const double lengthSquared = static_cast<double>(value.x) * value.x +
                                 static_cast<double>(value.y) * value.y +
                                 static_cast<double>(value.z) * value.z;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-20) return false;
    const double inverseLength = 1.0 / std::sqrt(lengthSquared);
    const Float3 candidate{static_cast<float>(value.x * inverseLength),
                           static_cast<float>(value.y * inverseLength),
                           static_cast<float>(value.z * inverseLength)};
    if (!Finite(candidate)) return false;
    normalized = candidate;
    return true;
}

double Dot(Float3 a, Float3 b) {
    return static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y +
           static_cast<double>(a.z) * b.z;
}

}  // namespace

bool ProjectPoint(Float3 world, const Float4x4& view, const Float4x4& projection,
                  Rect viewport, ScreenPoint& screen) {
    if (!Finite(world) || !std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(viewport.w) || !std::isfinite(viewport.h) || viewport.w <= 0 || viewport.h <= 0)
        return false;
    const double input[4] = {world.x, world.y, world.z, 1.0};
    double viewPoint[4]{};
    double clip[4]{};
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 4; ++row)
            viewPoint[column] += input[row] * view.m[row][column];
    }
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 4; ++row)
            clip[column] += viewPoint[row] * projection.m[row][column];
    }
    if (!std::isfinite(clip[3]) || clip[3] <= 1e-6) return false;
    const double ndcX = clip[0] / clip[3];
    const double ndcY = clip[1] / clip[3];
    const double depth = clip[2] / clip[3];
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY) || !std::isfinite(depth) || depth < 0 || depth > 1)
        return false;
    const double pixelX = viewport.x + (ndcX + 1.0) * 0.5 * viewport.w;
    const double pixelY = viewport.y + (1.0 - ndcY) * 0.5 * viewport.h;
    if (!std::isfinite(pixelX) || !std::isfinite(pixelY) ||
        std::fabs(pixelX) > std::numeric_limits<float>::max() ||
        std::fabs(pixelY) > std::numeric_limits<float>::max())
        return false;
    screen = {static_cast<float>(pixelX), static_cast<float>(pixelY), static_cast<float>(depth)};
    return true;
}

int HitTestScreenSegments(float x, float y, std::span<const ScreenSegment> handles,
                          float radiusPixels, float minimumLengthPixels) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radiusPixels) || radiusPixels < 0 ||
        !std::isfinite(minimumLengthPixels) || minimumLengthPixels < 0)
        return -1;
    const double radiusSquared = static_cast<double>(radiusPixels) * radiusPixels;
    double bestDistanceSquared = radiusSquared;
    int best = -1;
    for (size_t index = 0; index < handles.size(); ++index) {
        const auto& handle = handles[index];
        const double dx = static_cast<double>(handle.end.x) - handle.start.x;
        const double dy = static_cast<double>(handle.end.y) - handle.start.y;
        const double lengthSquared = dx * dx + dy * dy;
        if (!std::isfinite(lengthSquared) || lengthSquared <
            static_cast<double>(minimumLengthPixels) * minimumLengthPixels)
            continue;
        const double t = std::clamp(((static_cast<double>(x) - handle.start.x) * dx +
                                     (static_cast<double>(y) - handle.start.y) * dy) / lengthSquared,
                                    0.0, 1.0);
        const double nearestX = handle.start.x + t * dx;
        const double nearestY = handle.start.y + t * dy;
        const double offsetX = static_cast<double>(x) - nearestX;
        const double offsetY = static_cast<double>(y) - nearestY;
        const double distanceSquared = offsetX * offsetX + offsetY * offsetY;
        if (std::isfinite(distanceSquared) && distanceSquared <= bestDistanceSquared) {
            bestDistanceSquared = distanceSquared;
            best = static_cast<int>(index);
        }
    }
    return best;
}

bool ClosestRayAxisParameter(const Ray& ray, Float3 axisOrigin, Float3 unitAxis, float& parameter) {
    Float3 rayDirection, axisDirection;
    if (!Finite(ray.origin) || !Finite(axisOrigin) || !Normalize(ray.dir, rayDirection) ||
        !Normalize(unitAxis, axisDirection))
        return false;
    const Float3 offset{ray.origin.x - axisOrigin.x, ray.origin.y - axisOrigin.y,
                        ray.origin.z - axisOrigin.z};
    const double directionDot = Dot(rayDirection, axisDirection);
    const double rayOffset = Dot(rayDirection, offset);
    const double axisOffset = Dot(axisDirection, offset);
    const double denominator = 1.0 - directionDot * directionDot;
    if (!std::isfinite(denominator) || denominator <= 1e-5) return false;
    const double candidate = (axisOffset - directionDot * rayOffset) / denominator;
    const double rayParameter = directionDot * candidate - rayOffset;
    if (!std::isfinite(candidate) || !std::isfinite(rayParameter) || rayParameter < 0 ||
        std::fabs(candidate) > std::numeric_limits<float>::max())
        return false;
    parameter = static_cast<float>(candidate);
    return true;
}

bool AxisTranslationFromParameter(Float3 startWorldPosition, Float3 unitAxis,
                                  float startParameter, float currentParameter,
                                  Float3& worldPosition) {
    Float3 axisDirection;
    if (!Finite(startWorldPosition) || !Normalize(unitAxis, axisDirection) ||
        !std::isfinite(startParameter) || !std::isfinite(currentParameter))
        return false;
    const double delta = static_cast<double>(currentParameter) - startParameter;
    const Float3 candidate{static_cast<float>(startWorldPosition.x + axisDirection.x * delta),
                           static_cast<float>(startWorldPosition.y + axisDirection.y * delta),
                           static_cast<float>(startWorldPosition.z + axisDirection.z * delta)};
    if (!Finite(candidate)) return false;
    worldPosition = candidate;
    return true;
}

}  // namespace scuba::render
