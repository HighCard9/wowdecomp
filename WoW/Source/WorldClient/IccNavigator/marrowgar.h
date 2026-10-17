#pragma once
#include "flame_trails.h"
#include "movement.h"
#include <functional>
#include <queue>

namespace marrowgar {
constexpr float flameMargin = 4.0f; // Estimated 3 yd footprint + 1 yd buffer.
inline float distance(Position a, Position b) {
  return RouteFollower::horizontal(a, b);
}
inline float pointSegment(Position p, Position a, Position b) {
  float dx = b.x - a.x, dy = b.y - a.y, n = dx * dx + dy * dy;
  float t = n > 0.0001f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / n,
                                     0.f, 1.f)
                        : 0;
  return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}
struct Hazards {
  std::vector<FlameTrailSegment> lines;
  std::vector<std::vector<size_t>> groups;
  explicit Hazards(std::vector<FlameTrailSegment> trails, uint64_t now)
      : lines(std::move(trails)) {
    // Short prediction only from recently observed motion, never NPC facing.
    std::unordered_map<uint64_t, FlameTrailSegment> latest;
    for (auto &s : lines)
      if (s.toMs > s.fromMs && now >= s.toMs && now - s.toMs < 600)
        if (!latest.count(s.emitter) || latest[s.emitter].toMs < s.toMs)
          latest[s.emitter] = s;
    for (auto &entry : latest) {
      auto s = entry.second;
      float len = distance(s.from, s.to),
            seconds = float(s.toMs - s.fromMs) / 1000;
      if (len < .1f || seconds <= 0)
        continue;
      float extra = std::min(6.f, len / seconds * .6f);
      Position end{s.to.x + (s.to.x - s.from.x) * extra / len,
                   s.to.y + (s.to.y - s.from.y) * extra / len, s.to.z};
      lines.push_back({s.emitter, now, now, s.to, end});
    }
    // Continuous sections from one emitter form one hazard, not separate walls.
    std::unordered_map<uint64_t, size_t> groupIndex;
    for (size_t i = 0; i < lines.size(); ++i) {
      auto [it, inserted] = groupIndex.emplace(lines[i].emitter, groups.size());
      if (inserted)
        groups.emplace_back();
      groups[it->second].push_back(i);
    }
  }
  bool pointClear(Position p, float margin = flameMargin) const {
    for (auto &s : lines)
      if (std::abs(p.z - s.from.z) < 3 &&
          pointSegment(p, s.from, s.to) < margin)
        return false;
    return true;
  }
  float groupDistance(Position p, const std::vector<size_t> &group) const {
    float best = 1e9f;
    for (auto i : group) {
      auto &s = lines[i];
      if (std::abs(p.z - s.from.z) < 3)
        best = std::min(best, pointSegment(p, s.from, s.to));
    }
    return best;
  }
  // An emergency may leave overlapping flames that already contain the player.
  // It may not enter a new emitter's trail, or re-enter a trail after leaving
  // it.
  bool escapeSegment(Position a, Position b) const {
    int count = std::max(1, int(std::ceil(distance(a, b) / .25f)));
    for (auto &g : groups) {
      bool inside = groupDistance(a, g) < 4.125f;
      for (int i = 1; i <= count; ++i) {
        float t = float(i) / count;
        Position p{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y),
                   a.z + t * (b.z - a.z)};
        bool next = groupDistance(p, g) < 4.125f;
        if (next && !inside)
          return false;
        inside = next;
      }
    }
    return true;
  }
  bool segmentClear(Position a, Position b, bool escape = false) const {
    int count = std::max(1, int(std::ceil(distance(a, b) / .25f)));
    for (auto &g : groups) {
      float previous = groupDistance(a, g), initial = previous;
      bool leaving = escape && previous < 4.125f;
      for (int i = 1; i <= count; ++i) {
        float t = float(i) / count;
        Position p{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y),
                   a.z + t * (b.z - a.z)};
        float d = groupDistance(p, g);
        if (leaving) {
          if (d + .01f < previous)
            return false;
        } else if (d < 4.125f)
          return false;
        previous = d;
      }
      if (leaving && previous <= initial + .05f)
        return false;
    }
    return true;
  }
};
inline const NearbyObject *boss(const NearbySnapshot *s) {
  if (!s)
    return nullptr;
  const NearbyObject *found = nullptr;
  for (auto &o : s->objects)
    if (o.type == 3 && o.entry == 36612 && o.positioned) {
      if (found)
        return nullptr;
      found = &o;
    }
  return found;
}
inline std::string observationError(const NearbySnapshot *scan,
                                    const ClientState &p, uint64_t now) {
  if (!scan || !scan->valid || scan->playerGuid != p.guid ||
      now < scan->timeMs || now - scan->timeMs > 750)
    return "Waiting: fresh nearby observations unavailable.";
  if (!scan->encounterComplete)
    return "Waiting: incomplete boss/flame scan; movement paused.";
  auto b = boss(scan);
  if (!b || !b->bossStateValid)
    return "Waiting: Marrowgar position/aura/health read unavailable.";
  if (!b->health)
    return "Marrowgar is dead.";
  if (!(b->unitFlags & 0x80000))
    return "Waiting: Marrowgar is out of combat.";
  if (distance(p.position, b->position) > 45 ||
      std::abs(p.position.z - b->position.z) > 3)
    return "Waiting: Marrowgar is too far away or on another floor.";
  return {};
}
using Walkable = std::function<bool(Position, Position)>;
struct Plan {
  std::vector<Position> points;
  bool holding = false, emergency = false;
  std::string status;
};
// Storm movement cannot cut through the boss centre. If he lands on the
// player, an outward segment is permitted even though it starts inside.
inline bool stormSegment(Position a, Position z, const NearbyObject &b) {
  if (!b.boneStorm)
    return true;
  // Directly under him, an "away" bearing flips with tiny server updates.
  // Commit to the validated exit direction until we are clear of his centre.
  if (distance(a, b.position) < 2.f)
    return true;
  return pointSegment(b.position, a, z) + .05f >=
         std::min(4.5f, distance(a, b.position));
}
class HistoryGate {
  uint64_t until = 0;

public:
  void reset(uint64_t now) { until = now + 5000; }
  bool ready(uint64_t now, bool historyIntact) {
    if (!historyIntact)
      reset(now);
    return now >= until;
  }
};
class EscapeController {
  bool active_ = false, goalValid_ = false;
  uint64_t clearSince_ = 0;
  Position goal_{};

public:
  void reset() {
    active_ = goalValid_ = false;
    clearSince_ = 0;
  }
  bool active() const { return active_; }
  Plan step(const ClientState &p, const NearbyObject &b, const Hazards &h,
            const Walkable &walk, uint64_t now) {
    Plan result;
    bool flame = !h.pointClear(p.position, 4.25f);
    bool close = b.boneStorm && distance(p.position, b.position) < 4.5f;
    if (!active_ && !flame && !close)
      return result;
    active_ = true;
    result.emergency = true;
    bool clear = h.pointClear(p.position, 4.75f) &&
                 (!b.boneStorm || distance(p.position, b.position) >= 7.f);
    if (clear) {
      if (!clearSince_)
        clearSince_ = now;
      if (now - clearSince_ >= 250) {
        reset();
        return {};
      }
      result.holding = true;
      result.points = {p.position};
      result.status = "Escape clear: holding briefly to avoid oscillation.";
      return result;
    }
    clearSince_ = 0;
    auto usable = [&](Position q) {
      return h.pointClear(q, 4.75f) &&
             (!b.boneStorm || distance(q, b.position) >= 7.f) &&
             stormSegment(p.position, q, b) && h.escapeSegment(p.position, q) &&
             walk(p.position, q);
    };
    // Keep a chosen exit while it stays usable; tiny boss/scan changes must not
    // flip the desired turn direction every frame.
    if (!goalValid_ || distance(p.position, goal_) < .7f || !usable(goal_)) {
      goalValid_ = false;
      float best = 1e9f;
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
      for (float radius : {2.f, 4.f, 6.f, 8.f, 10.f, 12.f, 16.f}) {
        for (int i = 0; i < 32; ++i) {
          float angle = p.facing + i * (6.28318530718f / 32);
          Position q{p.position.x + radius * std::cos(angle),
                     p.position.y + radius * std::sin(angle), p.position.z};
          float turn = std::abs(RouteFollower::angle(angle - p.facing));
          // Approximate time to clear: avoid choosing a slightly shorter exit
          // that first needs a 180-degree mouse turn.
          float score = radius / 7.f + turn * .7f;
          if (score < best && usable(q)) {
            best = score;
            goal_ = q;
            goalValid_ = true;
          }
        }
        if (std::chrono::steady_clock::now() > deadline)
          break;
      }
    }
    if (goalValid_) {
      result.points = {p.position, goal_};
      result.status = flame
                          ? "ESCAPE: leaving estimated Coldflame."
                          : "ESCAPE: Marrowgar is too close during Bone Storm.";
    } else {
      result.holding = true;
      result.status = "ESCAPE blocked: no clear exit through the mesh/flames.";
    }
    return result;
  }
};
inline bool goodPosition(Position p, const NearbyObject &b,
                         bool holding = false) {
  float d = distance(p, b.position);
  if (b.boneStorm)
    return d >= 4.5f &&
           d <= std::min(12.f, b.combatReach + 2.f) + (holding ? 1.f : 0.f);
  float dot = (p.x - b.position.x) * std::cos(b.facing) +
              (p.y - b.position.y) * std::sin(b.facing);
  return d >= 1.5f && d <= (holding ? 5.f : 4.5f) && dot < -.45f * d;
}
inline Plan plan(Position player, const NearbyObject &b, const Hazards &hazards,
                 const Walkable &walk) {
  Plan result;
  const char *phase = b.boneStorm ? "Bone Storm" : "Normal";
  if (goodPosition(player, b, true) && hazards.pointClear(player)) {
    result.holding = true;
    result.points = {player};
    result.status = std::string(phase) + ": holding; face Marrowgar.";
    return result;
  }
  // Bounded local graph. Every edge checks both navmesh clearance and flames.
  constexpr int width = 33, middle = 16, total = width * width,
                root = middle * width + middle;
  std::array<float, total> costs;
  costs.fill(1e9f);
  std::array<int, total> parent;
  parent.fill(-1);
  std::array<bool, total> done{};
  auto pos = [&](int i) {
    return Position{player.x + 2 * (i % width - middle),
                    player.y + 2 * (i / width - middle), player.z};
  };
  using Node = std::pair<float, int>;
  std::priority_queue<Node, std::vector<Node>, std::greater<Node>> queue;
  costs[root] = 0;
  queue.push({0, root});
  int best = -1;
  float bestScore = 1e9f;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
  while (!queue.empty()) {
    auto [cost, i] = queue.top();
    queue.pop();
    if (done[i])
      continue;
    if (std::chrono::steady_clock::now() > deadline) {
      result.status = "Planning budget exceeded; W released.";
      return result;
    }
    done[i] = true;
    auto p = pos(i);
    if (cost > bestScore)
      break;
    if (i != root && goodPosition(p, b) && hazards.pointClear(p)) {
      float preferred = b.boneStorm ? std::min(8.f, b.combatReach + 1.f) : 3.f;
      float score = cost + 2 * std::abs(distance(p, b.position) - preferred);
      if (score < bestScore) {
        best = i;
        bestScore = score;
      }
    }
    // A trapped start can take a longer monotonic escape edge out of a flame.
    int radius = i == root && !hazards.pointClear(player) ? 4 : 1;
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dx = -radius; dx <= radius; ++dx) {
        if (!dx && !dy)
          continue;
        int x = i % width + dx, y = i / width + dy;
        if (x < 0 || x >= width || y < 0 || y >= width)
          continue;
        int j = y * width + x;
        if (done[j])
          continue;
        auto q = pos(j);
        float next = cost + distance(p, q);
        if (next >= costs[j] || !hazards.pointClear(q) ||
            !hazards.segmentClear(p, q, i == root) || !stormSegment(p, q, b))
          continue;
        // Do not route through the normal-phase frontal cleave area.
        if (!b.boneStorm) {
          bool front = false;
          int samples = std::max(1, int(distance(p, q) / .5f));
          for (int n = 1; n <= samples; ++n) {
            float t = float(n) / samples;
            Position v{p.x + t * (q.x - p.x), p.y + t * (q.y - p.y), p.z};
            float r = distance(v, b.position);
            float dot = (v.x - b.position.x) * std::cos(b.facing) +
                        (v.y - b.position.y) * std::sin(b.facing);
            if (r < 12 && dot > .3f * r) {
              front = true;
              break;
            }
          }
          if (front)
            continue;
        }
        if (!walk(p, q))
          continue;
        costs[j] = next;
        parent[j] = i;
        queue.push({next, j});
      }
  }
  if (best < 0) {
    result.status = std::string(phase) + ": no clear local path; holding.";
    return result;
  }
  for (int i = best; i >= 0; i = parent[i]) {
    result.points.push_back(pos(i));
    if (i == root)
      break;
  }
  std::reverse(result.points.begin(), result.points.end());
  // Remove grid zigzags only where the full shortcut is validated again.
  std::vector<Position> smooth{player};
  for (size_t i = 0; i + 1 < result.points.size();) {
    size_t next = i + 1;
    for (size_t j = i + 2; j < result.points.size(); ++j) {
      if (!hazards.segmentClear(result.points[i], result.points[j], i == 0) ||
          !walk(result.points[i], result.points[j]) ||
          !stormSegment(result.points[i], result.points[j], b))
        break;
      // Keep normal-phase grid edges to preserve the frontal exclusion.
      if (!b.boneStorm)
        break;
      next = j;
    }
    smooth.push_back(result.points[next]);
    i = next;
  }
  result.points = std::move(smooth);
  result.status = std::string(phase) + ": moving to a clear attack position.";
  return result;
}
// Persistent watchdog across replans: replanning cannot reset no-motion time.
class Driver {
  uint64_t previousTime = 0, began = 0, forwardMs = 0, turnMs = 0,
           settleUntil = 0;
  Position previous{}, progress{};
  float lastFacing = 0;
  bool forward = false;

public:
  void begin(const ClientState &s, uint64_t now) {
    previousTime = began = now;
    previous = progress = s.position;
    lastFacing = s.facing;
    forwardMs = turnMs = settleUntil = 0;
    forward = false;
  }
  MotionDecision step(const ClientState &s, uint64_t now, Position target,
                      bool hold, bool emergency = false) {
    MotionDecision d;
    auto fail = [&](const char *r) {
      d.stopReason = r;
      forward = false;
      return d;
    };
    if (now < previousTime || now - previousTime > 300)
      return fail("DPS update timed out.");
    if (now - began > 600000)
      return fail("10-minute DPS test limit reached.");
    uint64_t dt = now - previousTime;
    if (!validWorldPosition(s.position) || !std::isfinite(s.facing) ||
        distance(previous, s.position) > 2 ||
        std::abs(previous.z - s.position.z) > 3)
      return fail("DPS position jumped or became invalid.");
    if (distance(progress, s.position) > .25f) {
      progress = s.position;
      forwardMs = 0;
    }
    if (std::abs(RouteFollower::angle(s.facing - lastFacing)) > .02f) {
      lastFacing = s.facing;
      turnMs = 0;
    }
    d.headingError = RouteFollower::angle(
        std::atan2(target.y - s.position.y, target.x - s.position.x) -
        s.facing);
    float error = std::abs(d.headingError);
    if (forward && (hold || error > .18f)) {
      forward = false;
      settleUntil = now + (emergency ? 80 : 180);
    }
    if (!hold && !forward && now >= settleUntil && error < .065f)
      forward = true;
    d.forward = forward;
    if (now >= settleUntil && error > .04f)
      d.turn = d.headingError > 0 ? 1 : -1;
    if (d.forward)
      forwardMs += dt;
    if (d.turn)
      turnMs += dt;
    if (forwardMs > 3000)
      return fail("DPS forward progress stalled; stopped.");
    if (turnMs > 3000)
      return fail("DPS turning stalled; stopped.");
    previous = s.position;
    previousTime = now;
    return d;
  }
};
} // namespace marrowgar
