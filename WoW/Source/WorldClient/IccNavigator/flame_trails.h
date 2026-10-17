#pragma once
#include "nearby_objects.h"
#include <unordered_map>

// Visual reconstruction only. No claim that a segment is still damaging.
struct FlameTrailSegment {
  uint64_t emitter = 0, fromMs = 0, toMs = 0;
  Position from{}, to{};
};
class FlameTrails {
  struct Head {
    Position position{}, observed{};
    uint64_t seenMs = 0, positionMs = 0;
  };
  std::unordered_map<uint64_t, Head> heads_;
  std::deque<FlameTrailSegment> segments_;
  uint64_t playerGuid_ = 0, lastTime_ = 0;
  Position origin_{};

public:
  static constexpr uint64_t maxHistoryMs = 20000;
  static constexpr size_t maxHeads = 128, maxSegments = 4096;
  void clear() {
    heads_.clear();
    segments_.clear();
    playerGuid_ = lastTime_ = 0;
    origin_ = {};
  }
  void prune(uint64_t now) {
    while (!segments_.empty() && (now < segments_.front().toMs ||
                                  now - segments_.front().toMs > maxHistoryMs))
      segments_.pop_front();
    for (auto i = heads_.begin(); i != heads_.end();) {
      if (now < i->second.seenMs || now - i->second.seenMs > maxHistoryMs)
        i = heads_.erase(i);
      else
        ++i;
    }
  }
  void add(const NearbySnapshot &s) {
    if (!s.valid || !s.playerGuid || !validWorldPosition(s.origin)) {
      clear();
      return;
    }
    if (s.playerGuid != playerGuid_ || s.timeMs < lastTime_ ||
        (playerGuid_ && objectDistanceSquared(origin_, s.origin) > 100 * 100))
      clear();
    // A duplicate snapshot must not extend a deposit's life.
    if (playerGuid_ && s.timeMs == lastTime_)
      return;
    playerGuid_ = s.playerGuid;
    lastTime_ = s.timeMs;
    origin_ = s.origin;
    prune(s.timeMs);
    for (const auto &o : s.objects) {
      if (!o.coldflame() || !o.positioned || !o.guid ||
          !validWorldPosition(o.position))
        continue;
      auto i = heads_.find(o.guid);
      if (i == heads_.end()) {
        if (heads_.size() >= maxHeads)
          continue;
        heads_[o.guid] = {o.position, o.position, s.timeMs, s.timeMs};
        segments_.push_back(
            {o.guid, s.timeMs, s.timeMs, o.position, o.position});
      } else {
        auto &head = i->second;
        float distance = objectDistanceSquared(head.position, o.position);
        uint64_t gap = s.timeMs - head.seenMs;
        if (objectDistanceSquared(head.observed, o.position) < 0.0001f)
          head.positionMs =
              s.timeMs; // exclude stationary time from later motion
        head.observed = o.position;
        if (distance >= 0.25f) {
          // No links across missing data, floors or implausible jumps.
          bool connect = gap <= 1200 && distance <= 12 * 12 &&
                         std::abs(head.position.z - o.position.z) <= 3;
          segments_.push_back({o.guid, connect ? head.positionMs : s.timeMs,
                               s.timeMs, connect ? head.position : o.position,
                               o.position});
          head.position = o.position;
          head.positionMs = s.timeMs;
        }
        // Stationary heads can outlive emission: never refresh old trail age.
        head.seenMs = s.timeMs;
      }
      while (segments_.size() > maxSegments)
        segments_.pop_front();
    }
  }
  std::vector<FlameTrailSegment> visible(uint64_t now,
                                         uint64_t historyMs) const {
    std::vector<FlameTrailSegment> result;
    historyMs = std::min(historyMs, maxHistoryMs);
    if (!historyMs || now < lastTime_)
      return result;
    uint64_t cutoff = now > historyMs ? now - historyMs : 0;
    for (auto segment : segments_) {
      if (segment.toMs > now || segment.toMs <= cutoff)
        continue;
      if (segment.fromMs < cutoff && segment.toMs > segment.fromMs) {
        double t =
            double(cutoff - segment.fromMs) / (segment.toMs - segment.fromMs);
        segment.from = {
            float(segment.from.x + t * (segment.to.x - segment.from.x)),
            float(segment.from.y + t * (segment.to.y - segment.from.y)),
            float(segment.from.z + t * (segment.to.z - segment.from.z))};
        segment.fromMs = cutoff;
      }
      result.push_back(segment);
    }
    return result;
  }
};
