#pragma once
#include "client_state.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

struct MotionDecision {
  bool forward = false;
  int turn = 0; // +1 left (increasing facing), -1 right.
  bool arrived = false;
  std::string stopReason;
  size_t waypoint = 0;
  float headingError = 0;
};

class RouteFollower {
  std::vector<Position> points_;
  size_t next_ = 0;
  uint64_t began_ = 0, previousTime_ = 0, progressTime_ = 0;
  uint64_t guid_ = 0;
  Position previous_{};
  float bestDistance_ = 1e9f, bestTurnError_ = 4;
  bool wasTurning_ = false, active_ = false;
  bool forward_ = false;
  uint64_t settleUntil_ = 0;

public:
  static float horizontal(Position a, Position b) {
    return std::hypot(a.x - b.x, a.y - b.y);
  }
  static float angle(float value) {
    return std::remainder(value, 6.28318530718f);
  }
  void begin(const std::vector<Position> &points, const ClientState &s,
             uint64_t now) {
    points_ = points;
    next_ = points.size() > 1 ? 1 : 0;
    began_ = previousTime_ = progressTime_ = now;
    guid_ = s.guid;
    previous_ = s.position;
    bestDistance_ = 1e9f;
    bestTurnError_ = 4;
    wasTurning_ = false;
    forward_ = false;
    settleUntil_ = 0;
    active_ = !points.empty();
  }
  void stop() { active_ = false; }
  MotionDecision step(const ClientState &s, uint64_t now) {
    MotionDecision d;
    auto fail = [&](const char *reason) {
      active_ = false;
      d.forward = false;
      d.turn = 0;
      d.stopReason = reason;
      return d;
    };
    if (!active_)
      return fail("Movement is not active.");
    if (!s.valid || !s.health || s.guid != guid_)
      return fail("Character readings unavailable, character changed, or "
                  "health is zero.");
    if (now < previousTime_ || now - previousTime_ > 250)
      return fail("Movement update timed out.");
    if (now - began_ > 90000)
      return fail("90-second movement test limit reached.");
    if (!std::isfinite(s.position.x) || !std::isfinite(s.position.y) ||
        !std::isfinite(s.position.z) || !std::isfinite(s.facing))
      return fail("Invalid position or facing.");
    float dt = (now - previousTime_) / 1000.0f;
    if (horizontal(s.position, previous_) > std::max(2.0f, dt * 14.0f) ||
        std::abs(s.position.z - previous_.z) > 4)
      return fail("Position jumped unexpectedly. Recalculate the route.");
    previous_ = s.position;
    previousTime_ = now;
    const size_t previousWaypoint = next_;
    while (next_ < points_.size()) {
      float radius = next_ + 1 == points_.size() ? 0.8f : 0.55f;
      if (horizontal(s.position, points_[next_]) > radius ||
          std::abs(s.position.z - points_[next_].z) > 1.5f)
        break;
      ++next_;
      bestDistance_ = 1e9f;
      bestTurnError_ = 4;
      progressTime_ = now;
    }
    d.waypoint = next_;
    if (next_ == points_.size()) {
      active_ = false;
      d.arrived = true;
      return d;
    }
    auto target = points_[next_];
    auto from = points_[next_ ? next_ - 1 : 0];
    float dx = target.x - from.x, dy = target.y - from.y;
    float length2 = dx * dx + dy * dy;
    float t = length2 > 1e-6f ? std::clamp(((s.position.x - from.x) * dx +
                                            (s.position.y - from.y) * dy) /
                                               length2,
                                           0.0f, 1.0f)
                              : 0;
    Position projected{from.x + t * dx, from.y + t * dy,
                       from.z + t * (target.z - from.z)};
    if (horizontal(s.position, projected) > 3 ||
        std::abs(s.position.z - projected.z) > 3)
      return fail("Character left the planned ground route. Recalculate.");
    d.headingError =
        angle(std::atan2(target.y - s.position.y, target.x - s.position.x) -
              s.facing);
    float error = std::abs(d.headingError);
    // Turn in place before moving; small corrections are allowed while walking.
    // Hysteresis avoids alternating W down/up around a single threshold.
    // At a bend, release W and allow queued movement to settle before turning.
    if (forward_ &&
        (error > 0.18f || (next_ != previousWaypoint && error > 0.10f))) {
      forward_ = false;
      settleUntil_ = now + 180;
    }
    if (!forward_ && now >= settleUntil_ && error < 0.065f)
      forward_ = true;
    d.turn = error > 0.04f && now >= settleUntil_
                 ? (d.headingError > 0 ? 1 : -1)
                 : 0;
    d.forward = forward_;
    float distance = horizontal(s.position, target);
    if (!d.forward) {
      if (!wasTurning_ || error < bestTurnError_ - 0.025f) {
        progressTime_ = now;
        bestTurnError_ = error;
      }
      if (now - progressTime_ > 3000)
        return fail("Mouse turning is not responding. Check mouselook and "
                    "close game menus.");
    } else {
      if (wasTurning_ || distance < bestDistance_ - 0.15f) {
        progressTime_ = now;
        bestDistance_ = distance;
      }
      if (now - progressTime_ > 3000)
        return fail("No forward progress. Check W binding or an obstacle.");
    }
    wasTurning_ = !d.forward;
    return d;
  }
};

// Adapt relative mouse pulses to observed facing changes, rather than assume
// one fixed in-game mouse sensitivity. Positive mouse X turns facing negative.
class MouseSteering {
  float radiansPerPixel_ = 0.006f, previousFacing_ = 0;
  int previousPixels_ = 0;
  uint64_t issued_ = 0, clock_ = 0, changedAt_ = 0;
  float lastObservedFacing_ = 0;
  bool responseSeen_ = false, calibrated_ = false, blocksForward_ = false;
  unsigned cleanResponses_ = 0;

public:
  void reset(float facing, float calibratedGain = 0.006f,
             bool calibrated = false) {
    radiansPerPixel_ = std::clamp(calibratedGain, 0.0005f, 0.06f);
    previousFacing_ = facing;
    previousPixels_ = 0;
    issued_ = clock_ = changedAt_ = 0;
    lastObservedFacing_ = facing;
    responseSeen_ = blocksForward_ = false;
    calibrated_ = calibrated;
    cleanResponses_ = 0;
  }
  bool blocksForward() const { return blocksForward_; }
  int step(float facing, float headingError, bool turn, uint64_t now = 0) {
    if (!now)
      now = clock_ + 50;
    clock_ = now;
    float changed = RouteFollower::angle(facing - previousFacing_);
    if (previousPixels_) {
      // Only one unobserved correction at a time. Repeating pulses against
      // stale facing was causing overshoot when the background client lagged.
      if (changed * previousPixels_ >= -0.00001f)
        return 0;
      // A large pulse can be reflected over several game frames. Wait for a
      // stable facing sample before learning gain or issuing another pulse.
      if (!responseSeen_ || std::abs(RouteFollower::angle(
                                facing - lastObservedFacing_)) > .001f) {
        responseSeen_ = true;
        lastObservedFacing_ = facing;
        changedAt_ = now;
        return 0;
      }
      if (now - changedAt_ < 40)
        return 0;
      float observed = std::abs(changed / previousPixels_);
      if (observed > 0.0002f && observed < 0.08f) {
        const float ratio = observed / radiansPerPixel_;
        if (ratio < .5f || ratio > 2.f) {
          calibrated_ = false;
          cleanResponses_ = 0;
        } else if (++cleanResponses_ >= 2)
          calibrated_ = true;
        radiansPerPixel_ = std::clamp(observed, .0005f, .06f);
      }
      blocksForward_ = false;
      previousPixels_ = 0;
    }
    if (now - issued_ < 40)
      return 0;
    int pixels = 0;
    if (turn) {
      const bool large = calibrated_ && std::abs(headingError) > .35f;
      const float radians = large
                                ? std::min(std::abs(headingError) * .90f, 1.2f)
                                : std::min(std::abs(headingError) * .45f, .16f);
      // Retain the working native bridge's 120-pixel limit. Uncalibrated
      // foreground input starts with small pulses until gain is observed.
      float magnitude = std::clamp(
          radians / radiansPerPixel_, 1.f,
          large ? 120.f : (std::abs(headingError) > .5f ? 40.f : 16.f));
      pixels =
          static_cast<int>(std::round(magnitude)) * (headingError > 0 ? -1 : 1);
    }
    previousFacing_ = facing;
    previousPixels_ = pixels;
    if (pixels) {
      issued_ = now;
      responseSeen_ = false;
      blocksForward_ = std::abs(headingError) > .18f;
    }
    return pixels;
  }
};
