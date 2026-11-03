// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scuba/render/camera.h"

#include <span>

namespace scuba::render {

struct ScreenPoint {
    float x = 0, y = 0, depth = 0;
};

struct ScreenSegment {
    ScreenPoint start;
    ScreenPoint end;
};

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
                                  Float3& worldPosition);

// Transforms a direction (translation is ignored) and normalizes it. Used to draw local-space
// scale axes through a rotated or nonuniformly scaled parent transform.
bool NormalizeTransformedDirection(Float3 direction, const Float4x4& matrix,
                                   Float3& normalizedDirection);

// Maps one complete projected gizmo span to one local scale unit. The caller applies document
// scale limits transactionally after this input-space calculation.
bool AxisScaleFromParameter(float startScale, float startParameter, float currentParameter,
                            float handleWorldLength, float& localScale);

}  // namespace scuba::render
