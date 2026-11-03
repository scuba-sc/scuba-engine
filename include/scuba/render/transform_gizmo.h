// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scuba/render/camera.h"

#include <array>
#include <cstddef>
#include <span>

namespace scuba::render {

struct ScreenPoint {
    float x = 0, y = 0, depth = 0;
};

struct ScreenSegment {
    ScreenPoint start;
    ScreenPoint end;
};

inline constexpr size_t kRotationRingSegments = 64;
inline constexpr float kMinTranslationSnapStep = 0.001f;
inline constexpr float kMaxTranslationSnapStep = 100000.0f;
inline constexpr float kMinRotationSnapStepDegrees = 0.1f;
inline constexpr float kMaxRotationSnapStepDegrees = 180.0f;
inline constexpr float kMinScaleSnapStep = 0.001f;
inline constexpr float kMaxScaleSnapStep = 1000.0f;

struct GizmoSnapSettings {
    bool translationEnabled = false;
    float translationStep = 1.0f;  // world units
    bool rotationEnabled = false;
    float rotationStepDegrees = 15.0f;
    bool scaleEnabled = false;
    float scaleStep = 0.1f;  // additive local scale units
};

bool ValidateGizmoSnapSettings(const GizmoSnapSettings& settings);
bool SnapTranslationDelta(float delta, float step, float& snappedDelta);
bool SnapRotationDelta(float deltaRadians, float stepDegrees, float& snappedDeltaRadians);
bool SnapScaleDelta(float delta, float step, float& snappedDelta);

// Projects a world point through row-vector view/projection matrices into client-pixel viewport
// coordinates. Points behind the camera or outside the depth range are rejected; output is intact.
bool ProjectPoint(Float3 world, const Float4x4& view, const Float4x4& projection,
                  Rect viewport, ScreenPoint& screen);

// Returns the nearest screen-space handle within the pixel radius, or -1. Short projected axes
// are ignored because they cannot be picked reliably when viewed end-on.
int HitTestScreenSegments(float x, float y, std::span<const ScreenSegment> handles,
                          float radiusPixels = 9.0f, float minimumLengthPixels = 12.0f);

// Finds the closest parameter on an infinite world-axis line to a forward ray. Fails for nearly
// parallel/ill-conditioned ray-axis pairs, or when the closest ray point lies behind its origin.
bool ClosestRayAxisParameter(const Ray& ray, Float3 axisOrigin, Float3 unitAxis, float& parameter);

// Applies a ray-derived parameter delta to a starting world position along a world unit axis.
// Output remains unchanged if the inputs are invalid.
bool AxisTranslationFromParameter(Float3 startWorldPosition, Float3 unitAxis,
                                  float startParameter, float currentParameter,
                                  Float3& worldPosition, float snapStep = 0.0f);

// Transforms a direction (translation is ignored) and normalizes it. Used to draw local-space
// scale axes through a rotated or nonuniformly scaled parent transform.
bool NormalizeTransformedDirection(Float3 direction, const Float4x4& matrix,
                                   Float3& normalizedDirection);

// Applies the linear part of a row-vector affine matrix to a direction. Output stays intact on
// invalid input.
bool TransformDirection(Float3 direction, const Float4x4& matrix, Float3& transformedDirection);

// Intersects a forward ray with the plane spanned by basisU/basisV. Rejects parallel or
// ill-conditioned planes and leaves output unchanged on failure.
bool IntersectRayPlane(const Ray& ray, Float3 planeOrigin, Float3 basisU, Float3 basisV,
                       Float3& intersection);

// Returns coordinates for a point in the (possibly nonorthogonal) plane basis. The caller may
// project an approximate point into the plane first. Output stays unchanged on failure.
bool PlaneBasisCoordinates(Float3 planeOrigin, Float3 basisU, Float3 basisV, Float3 point,
                           float& coordinateU, float& coordinateV);

// Computes the signed shortest angle from one nonzero 2D vector to another, in radians.
bool SignedPlanarAngleDelta(float fromU, float fromV, float toU, float toV, float& deltaRadians);

// Projects an affine local-space circle into screen-space line segments. The basis vectors include
// the parent linear transform, so nonuniform parents correctly produce an ellipse.
bool ProjectRotationRing(Float3 center, Float3 basisU, Float3 basisV, float radius,
                         const Float4x4& view, const Float4x4& projection, Rect viewport,
                         std::span<ScreenSegment> segments);

// Maps one complete projected gizmo span to one local scale unit. The caller applies document
// scale limits transactionally after this input-space calculation.
bool AxisScaleFromParameter(float startScale, float startParameter, float currentParameter,
                            float handleWorldLength, float& localScale,
                            float snapStep = 0.0f);

}  // namespace scuba::render
