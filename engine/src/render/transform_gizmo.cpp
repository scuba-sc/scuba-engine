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

bool ValidPlaneBasis(Float3 basisU, Float3 basisV, double& uu, double& uv, double& vv,
                     double& determinant) {
    if (!Finite(basisU) || !Finite(basisV)) return false;
    uu = Dot(basisU, basisU);
    uv = Dot(basisU, basisV);
    vv = Dot(basisV, basisV);
    determinant = uu * vv - uv * uv;
    if (!std::isfinite(uu) || !std::isfinite(uv) || !std::isfinite(vv) ||
        !std::isfinite(determinant) || uu <= 1e-20 || vv <= 1e-20 ||
        determinant <= 1e-12 * uu * vv)
        return false;
    const double trace = uu + vv;
    const double discriminant = std::sqrt(std::max(0.0, (uu - vv) * (uu - vv) + 4.0 * uv * uv));
    const double largestEigenvalue = 0.5 * (trace + discriminant);
    const double smallestEigenvalue = determinant / largestEigenvalue;
    return std::isfinite(largestEigenvalue) && std::isfinite(smallestEigenvalue) &&
           largestEigenvalue > 0 && smallestEigenvalue > 1e-12 * largestEigenvalue;
}

bool QuantizeDelta(double delta, double step, double minimumStep, double maximumStep,
                   double& snappedDelta) {
    if (!std::isfinite(delta) || !std::isfinite(step) || step < minimumStep || step > maximumStep)
        return false;
    const double increments = std::round(delta / step);
    const double candidate = increments * step;
    if (!std::isfinite(increments) || !std::isfinite(candidate) ||
        std::fabs(candidate) > std::numeric_limits<float>::max())
        return false;
    snappedDelta = candidate;
    return true;
}

}  // namespace

bool ValidateGizmoSnapSettings(const GizmoSnapSettings& settings) {
    const auto inRange = [](float value, float minimum, float maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    return inRange(settings.translationStep, kMinTranslationSnapStep, kMaxTranslationSnapStep) &&
           inRange(settings.rotationStepDegrees, kMinRotationSnapStepDegrees,
                   kMaxRotationSnapStepDegrees) &&
           inRange(settings.scaleStep, kMinScaleSnapStep, kMaxScaleSnapStep);
}

bool SnapTranslationDelta(float delta, float step, float& snappedDelta) {
    double candidate = 0;
    if (!QuantizeDelta(delta, step, kMinTranslationSnapStep, kMaxTranslationSnapStep, candidate))
        return false;
    snappedDelta = static_cast<float>(candidate);
    return std::isfinite(snappedDelta);
}

bool SnapRotationDelta(float deltaRadians, float stepDegrees, float& snappedDeltaRadians) {
    if (!std::isfinite(deltaRadians) || !std::isfinite(stepDegrees) ||
        stepDegrees < kMinRotationSnapStepDegrees || stepDegrees > kMaxRotationSnapStepDegrees)
        return false;
    constexpr double kDegreesToRadians = 3.1415926535897932384626433832795 / 180.0;
    double candidate = 0;
    const double stepRadians = static_cast<double>(stepDegrees) * kDegreesToRadians;
    if (!QuantizeDelta(deltaRadians, stepRadians,
                       static_cast<double>(kMinRotationSnapStepDegrees) * kDegreesToRadians,
                       static_cast<double>(kMaxRotationSnapStepDegrees) * kDegreesToRadians,
                       candidate))
        return false;
    snappedDeltaRadians = static_cast<float>(candidate);
    return std::isfinite(snappedDeltaRadians);
}

bool SnapScaleDelta(float delta, float step, float& snappedDelta) {
    double candidate = 0;
    if (!QuantizeDelta(delta, step, kMinScaleSnapStep, kMaxScaleSnapStep, candidate)) return false;
    snappedDelta = static_cast<float>(candidate);
    return std::isfinite(snappedDelta);
}

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
                                  Float3& worldPosition, float snapStep) {
    Float3 axisDirection;
    if (!Finite(startWorldPosition) || !Normalize(unitAxis, axisDirection) ||
        !std::isfinite(startParameter) || !std::isfinite(currentParameter) ||
        !std::isfinite(snapStep) || snapStep < 0)
        return false;
    double delta = static_cast<double>(currentParameter) - startParameter;
    if (snapStep > 0 && !QuantizeDelta(delta, snapStep, kMinTranslationSnapStep,
                                       kMaxTranslationSnapStep, delta))
        return false;
    const Float3 candidate{static_cast<float>(startWorldPosition.x + axisDirection.x * delta),
                           static_cast<float>(startWorldPosition.y + axisDirection.y * delta),
                           static_cast<float>(startWorldPosition.z + axisDirection.z * delta)};
    if (!Finite(candidate)) return false;
    worldPosition = candidate;
    return true;
}

bool NormalizeTransformedDirection(Float3 direction, const Float4x4& matrix,
                                   Float3& normalizedDirection) {
    Float3 transformed;
    if (!TransformDirection(direction, matrix, transformed)) return false;
    return Normalize(transformed, normalizedDirection);
}

bool TransformDirection(Float3 direction, const Float4x4& matrix, Float3& transformedDirection) {
    if (!Finite(direction)) return false;
    for (const auto& row : matrix.m)
        for (const float value : row)
            if (!std::isfinite(value)) return false;
    const Float3 transformed{
        static_cast<float>(static_cast<double>(direction.x) * matrix.m[0][0] +
                           static_cast<double>(direction.y) * matrix.m[1][0] +
                           static_cast<double>(direction.z) * matrix.m[2][0]),
        static_cast<float>(static_cast<double>(direction.x) * matrix.m[0][1] +
                           static_cast<double>(direction.y) * matrix.m[1][1] +
                           static_cast<double>(direction.z) * matrix.m[2][1]),
        static_cast<float>(static_cast<double>(direction.x) * matrix.m[0][2] +
                           static_cast<double>(direction.y) * matrix.m[1][2] +
                           static_cast<double>(direction.z) * matrix.m[2][2]),
    };
    if (!Finite(transformed)) return false;
    transformedDirection = transformed;
    return true;
}

bool IntersectRayPlane(const Ray& ray, Float3 planeOrigin, Float3 basisU, Float3 basisV,
                       Float3& intersection) {
    double uu = 0, uv = 0, vv = 0, determinant = 0;
    if (!Finite(ray.origin) || !Finite(planeOrigin) || !ValidPlaneBasis(basisU, basisV,
                                                                        uu, uv, vv, determinant))
        return false;
    Float3 direction;
    if (!Normalize(ray.dir, direction)) return false;
    const double normalX = static_cast<double>(basisU.y) * basisV.z -
                           static_cast<double>(basisU.z) * basisV.y;
    const double normalY = static_cast<double>(basisU.z) * basisV.x -
                           static_cast<double>(basisU.x) * basisV.z;
    const double normalZ = static_cast<double>(basisU.x) * basisV.y -
                           static_cast<double>(basisU.y) * basisV.x;
    const double normalLength = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
    const double denominator = static_cast<double>(direction.x) * normalX +
                               static_cast<double>(direction.y) * normalY +
                               static_cast<double>(direction.z) * normalZ;
    if (!std::isfinite(normalLength) || normalLength <= 1e-12 || !std::isfinite(denominator) ||
        std::fabs(denominator) <= normalLength * 1e-6)
        return false;
    const Float3 offset{planeOrigin.x - ray.origin.x, planeOrigin.y - ray.origin.y,
                        planeOrigin.z - ray.origin.z};
    const double distance = (static_cast<double>(offset.x) * normalX +
                             static_cast<double>(offset.y) * normalY +
                             static_cast<double>(offset.z) * normalZ) / denominator;
    if (!std::isfinite(distance) || distance < 0 || distance > std::numeric_limits<float>::max())
        return false;
    const Float3 candidate{static_cast<float>(ray.origin.x + direction.x * distance),
                           static_cast<float>(ray.origin.y + direction.y * distance),
                           static_cast<float>(ray.origin.z + direction.z * distance)};
    if (!Finite(candidate)) return false;
    intersection = candidate;
    return true;
}

bool PlaneBasisCoordinates(Float3 planeOrigin, Float3 basisU, Float3 basisV, Float3 point,
                           float& coordinateU, float& coordinateV) {
    double uu = 0, uv = 0, vv = 0, determinant = 0;
    if (!Finite(planeOrigin) || !Finite(point) ||
        !ValidPlaneBasis(basisU, basisV, uu, uv, vv, determinant))
        return false;
    const Float3 offset{point.x - planeOrigin.x, point.y - planeOrigin.y, point.z - planeOrigin.z};
    const double du = Dot(offset, basisU), dv = Dot(offset, basisV);
    const double u = (du * vv - dv * uv) / determinant;
    const double v = (dv * uu - du * uv) / determinant;
    if (!std::isfinite(u) || !std::isfinite(v) ||
        std::fabs(u) > std::numeric_limits<float>::max() ||
        std::fabs(v) > std::numeric_limits<float>::max())
        return false;
    const float candidateU = static_cast<float>(u), candidateV = static_cast<float>(v);
    if (!std::isfinite(candidateU) || !std::isfinite(candidateV)) return false;
    coordinateU = candidateU;
    coordinateV = candidateV;
    return true;
}

bool SignedPlanarAngleDelta(float fromU, float fromV, float toU, float toV, float& deltaRadians) {
    const float input[] = {fromU, fromV, toU, toV};
    for (const float value : input)
        if (!std::isfinite(value)) return false;
    const double fromLengthSquared = static_cast<double>(fromU) * fromU + static_cast<double>(fromV) * fromV;
    const double toLengthSquared = static_cast<double>(toU) * toU + static_cast<double>(toV) * toV;
    if (!std::isfinite(fromLengthSquared) || !std::isfinite(toLengthSquared) ||
        fromLengthSquared <= 1e-20 || toLengthSquared <= 1e-20)
        return false;
    const double cross = static_cast<double>(fromU) * toV - static_cast<double>(fromV) * toU;
    const double dot = static_cast<double>(fromU) * toU + static_cast<double>(fromV) * toV;
    const double candidate = std::atan2(cross, dot);
    if (!std::isfinite(candidate) || std::fabs(candidate) > std::numeric_limits<float>::max()) return false;
    deltaRadians = static_cast<float>(candidate);
    return std::isfinite(deltaRadians);
}

bool ProjectRotationRing(Float3 center, Float3 basisU, Float3 basisV, float radius,
                         const Float4x4& view, const Float4x4& projection, Rect viewport,
                         std::span<ScreenSegment> segments) {
    double uu = 0, uv = 0, vv = 0, determinant = 0;
    if (segments.size() != kRotationRingSegments || !Finite(center) || !std::isfinite(radius) ||
        radius <= 0 || !ValidPlaneBasis(basisU, basisV, uu, uv, vv, determinant))
        return false;
    ScreenPoint screenCenter;
    if (!ProjectPoint(center, view, projection, viewport, screenCenter)) return false;
    std::array<ScreenPoint, kRotationRingSegments> points{};
    std::array<bool, kRotationRingSegments> valid{};
    constexpr double kTau = 6.283185307179586476925286766559;
    for (size_t i = 0; i < kRotationRingSegments; ++i) {
        const double angle = kTau * static_cast<double>(i) /
                             static_cast<double>(kRotationRingSegments);
        const double u = radius * std::cos(angle), v = radius * std::sin(angle);
        const double worldX = static_cast<double>(center.x) + u * basisU.x + v * basisV.x;
        const double worldY = static_cast<double>(center.y) + u * basisU.y + v * basisV.y;
        const double worldZ = static_cast<double>(center.z) + u * basisU.z + v * basisV.z;
        if (!std::isfinite(worldX) || !std::isfinite(worldY) || !std::isfinite(worldZ) ||
            std::fabs(worldX) > std::numeric_limits<float>::max() ||
            std::fabs(worldY) > std::numeric_limits<float>::max() ||
            std::fabs(worldZ) > std::numeric_limits<float>::max())
            continue;
        const Float3 worldPoint{
            static_cast<float>(worldX), static_cast<float>(worldY), static_cast<float>(worldZ),
        };
        valid[i] = Finite(worldPoint) && ProjectPoint(worldPoint, view, projection, viewport, points[i]);
    }
    std::array<ScreenSegment, kRotationRingSegments> candidate{};
    size_t visibleSegments = 0;
    for (size_t i = 0; i < kRotationRingSegments; ++i) {
        const size_t next = (i + 1) % kRotationRingSegments;
        candidate[i] = {screenCenter, screenCenter};
        if (valid[i] && valid[next]) {
            candidate[i] = {points[i], points[next]};
            ++visibleSegments;
        }
    }
    if (visibleSegments == 0) return false;
    std::copy(candidate.begin(), candidate.end(), segments.begin());
    return true;
}

bool AxisScaleFromParameter(float startScale, float startParameter, float currentParameter,
                            float handleWorldLength, float& localScale, float snapStep) {
    if (!std::isfinite(startScale) || !std::isfinite(startParameter) ||
        !std::isfinite(currentParameter) || !std::isfinite(handleWorldLength) ||
        handleWorldLength <= 0 || !std::isfinite(snapStep) || snapStep < 0)
        return false;
    double scaleDelta = (static_cast<double>(currentParameter) - startParameter) / handleWorldLength;
    if (snapStep > 0 && !QuantizeDelta(scaleDelta, snapStep, kMinScaleSnapStep, kMaxScaleSnapStep,
                                       scaleDelta))
        return false;
    const double candidate = static_cast<double>(startScale) + scaleDelta;
    if (!std::isfinite(candidate) || std::fabs(candidate) > std::numeric_limits<float>::max()) return false;
    localScale = static_cast<float>(candidate);
    return std::isfinite(localScale);
}

}  // namespace scuba::render
