#include "marrowgar.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
int main() {
  using namespace marrowgar;
  NearbyObject b;
  b.guid = 99;
  b.type = 3;
  b.entry = 36612;
  b.positioned = b.bossStateValid = true;
  b.health = 100;
  b.unitFlags = 0x80000;
  b.combatReach = 8;
  b.position = {0, 0, 42};
  b.facing = 0;
  auto walk = [](Position, Position) { return true; };
  Hazards empty({}, 10000);
  auto normal = plan({-15, 0, 42}, b, empty, walk);
  assert(normal.points.size() > 1 && goodPosition(normal.points.back(), b));
  assert(plan({-3, 0, 42}, b, empty, walk).holding);
  b.boneStorm = true;
  std::vector<FlameTrailSegment> cross{
      {1, 9000, 9000, {-20, 0, 42}, {20, 0, 42}},
      {2, 9000, 9000, {0, -20, 42}, {0, 20, 42}}};
  Hazards flames(cross, 10000);
  auto storm = plan({-15, -15, 42}, b, flames, walk);
  assert(storm.points.size() > 1 && goodPosition(storm.points.back(), b));
  for (size_t i = 1; i < storm.points.size(); ++i)
    assert(flames.segmentClear(storm.points[i - 1], storm.points[i], true));
  assert(!flames.segmentClear({-10, -10, 42}, {10, 10, 42}));
  assert(!flames.segmentClear({-10, -10, 42}, {10, 10, 42}, true));
  // Already burning: can escape monotonically, never cross to the opposite
  // side.
  assert(flames.segmentClear({-8, -2, 42}, {-8, -7, 42}, true));
  assert(!flames.segmentClear({-8, -2, 42}, {-8, 7, 42}, true));
  auto escape = plan({-8, -2, 42}, b, flames, walk);
  assert(escape.points.size() > 1);
  assert(plan({-15, -15, 42}, b, flames, [](Position, Position) {
           return false;
         }).points.empty());
  assert(flames.pointClear({0, 0, 50}));
  Hazards forecast({{8, 9500, 10000, {-10, 0, 42}, {-7, 0, 42}}}, 10000);
  assert(!forecast.pointClear({-4, 0, 42}));
  // Solid wall blocks a tempting shortcut; reachable positions stay on our
  // side.
  auto wall = [](Position a, Position z) { return a.x < -5 && z.x < -5; };
  auto detour = plan({-14, -14, 42}, b, empty, wall);
  assert(detour.points.size() > 1);
  for (auto p : detour.points)
    assert(p.x < -5);
  ClientState p;
  p.valid = true;
  p.guid = 1;
  p.health = 100;
  p.position = {-10, -10, 42};
  p.facing = 0;
  NearbySnapshot scan;
  scan.valid = scan.unitsComplete = scan.encounterComplete = true;
  scan.playerGuid = 1;
  scan.timeMs = 10000;
  scan.objects = {b};
  assert(observationError(&scan, p, 10000).empty());
  assert(!observationError(&scan, p, 10751).empty());
  scan.encounterComplete = false;
  assert(!observationError(&scan, p, 10000).empty());
  scan.encounterComplete = true;
  scan.objects[0].bossStateValid = false;
  assert(!observationError(&scan, p, 10000).empty());
  Driver d;
  d.begin(p, 10000);
  MotionDecision m;
  for (uint64_t t = 10050; t <= 13200; t += 50) {
    m = d.step(p, t, {-5, -10, 42}, false);
    if (!m.stopReason.empty())
      break;
  }
  assert(!m.stopReason.empty()); // Replans do not reset this watchdog.
  d.begin(p, 10000);
  assert(!d.step(p, 10400, p.position, true).stopReason.empty());
  d.begin(p, 10000);
  p.position.x += 10;
  assert(!d.step(p, 10050, p.position, true).stopReason.empty());
  // Immediate emergency selection when the storm boss lands exactly on us.
  ClientState runner;
  runner.valid = true;
  runner.health = 100;
  runner.guid = 3;
  runner.position = {0, 0, 42};
  runner.facing = .4f;
  EscapeController urgent;
  auto exit = urgent.step(runner, b, empty, walk, 20000);
  assert(exit.emergency && !exit.holding && exit.points.size() == 2);
  assert(distance(exit.points.back(), b.position) >= 7);
  assert(std::abs(RouteFollower::angle(
             std::atan2(exit.points.back().y, exit.points.back().x) -
             runner.facing)) < .01f);
  auto committed = exit.points.back();
  for (int i = 1; i <= 5; ++i) {
    b.position.x = (i % 2 ? .3f : -.3f);
    b.position.y = (i % 2 ? -.3f : .3f);
    auto stable = urgent.step(runner, b, empty, walk, 20000 + i * 50);
    assert(stable.emergency &&
           distance(stable.points.back(), committed) < .01f);
  }
  b.position = {0, 0, 42};
  assert(!stormSegment({-10, 0, 42}, {10, 0, 42}, b));
  assert(stormSegment({0, 0, 42}, {8, 0, 42}, b));
  assert(!stormSegment({2, 0, 42}, {-8, 0, 42}, b));
  // Flame escape does not require the endpoint to stay in attack range.
  b.position = {10, 0, 42};
  runner.position = {0, 0, 42};
  runner.facing = 3.14159265f;
  Hazards overlapping({{1, 10000, 10000, {0, -20, 42}, {0, 20, 42}},
                       {2, 10000, 10000, {2, -20, 42}, {2, 20, 42}}},
                      20000);
  urgent.reset();
  exit = urgent.step(runner, b, overlapping, walk, 20000);
  assert(exit.emergency && !exit.holding && exit.points.back().x < -4.75f);
  assert(distance(exit.points.back(), b.position) > b.combatReach + 2);
  assert(overlapping.escapeSegment(runner.position, exit.points.back()));
  assert(!overlapping.escapeSegment({-10, 0, 42}, {10, 0, 42}));
  // The first clear frame is held briefly; recovery does not immediately flip.
  runner.position = exit.points.back();
  assert(urgent.step(runner, b, overlapping, walk, 20100).holding);
  assert(!urgent.step(runner, b, overlapping, walk, 20400).emergency);
  urgent.reset();
  runner.position = {0, 0, 42};
  assert(urgent
             .step(
                 runner, b, overlapping,
                 [](Position, Position) { return false; }, 21000)
             .holding);
  HistoryGate gate;
  gate.reset(1000);
  assert(!gate.ready(5999, true));
  assert(gate.ready(6000, true));
  // An intact history survives a transient partial frame (movement itself is
  // paused).
  assert(gate.ready(6050, true));
  assert(gate.ready(6100, true));
  assert(!gate.ready(6150, false));
  assert(!gate.ready(11000, true));
  assert(gate.ready(11150, true));
  // Drive a stable escape while boss position jitters around the starting
  // point.
  runner.position = {0, 0, 42};
  runner.facing = .4f;
  b.position = {0, 0, 42};
  urgent.reset();
  Driver escapeDriver;
  escapeDriver.begin(runner, 30000);
  MouseSteering steering;
  steering.reset(runner.facing, .006f);
  bool gotForward = false, recovered = false;
  for (uint64_t t = 30050; t <= 33500; t += 50) {
    auto e = urgent.step(runner, b, empty, walk, t);
    if (!e.emergency) {
      recovered = true;
      break;
    }
    Position aim =
        e.holding ? Position{runner.position.x + std::cos(runner.facing),
                             runner.position.y + std::sin(runner.facing), 42}
                  : e.points.back();
    auto move = escapeDriver.step(runner, t, aim, e.holding, true);
    assert(move.stopReason.empty());
    int dx = steering.step(runner.facing, move.headingError, move.turn != 0, t);
    runner.facing =
        std::fmod(runner.facing + dx * .006f + 6.2831853f, 6.2831853f);
    if (move.forward) {
      gotForward = true;
      runner.position.x += .35f * std::cos(runner.facing);
      runner.position.y += .35f * std::sin(runner.facing);
    }
  }
  assert(gotForward && recovered);
  std::cout << "ICC mesh tests omitted: map binaries are not in this repository.\n";
  // Exact binary aura layout, both storage modes, corrupt count and races.
  std::map<uint32_t, unsigned char> mem;
  auto put = [&](uint32_t address, uint32_t v) {
    for (int i = 0; i < 4; ++i)
      mem[address + i] = (v >> (8 * i)) & 255;
  };
  MemoryRead read = [&](uint32_t a, void *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (!mem.count(a + i))
        return false;
      static_cast<unsigned char *>(out)[i] = mem[a + i];
    }
    return true;
  };
  uint32_t unit = 0x100000;
  put(unit + 0xdd0, 1);
  put(unit + 0xc58, 69076);
  auto aura = readUnitAuras(read, unit);
  assert(aura.valid && aura.spells == std::vector<uint32_t>{69076});
  put(unit + 0xdd0, 17);
  assert(!readUnitAuras(read, unit).valid);
  put(unit + 0xdd0, 0xffffffff);
  put(unit + 0xc54, 1);
  put(unit + 0xc58, 0x200000);
  put(0x200008, 69076);
  assert(readUnitAuras(read, unit).valid);
  int calls = 0;
  MemoryRead race = [&](uint32_t a, void *out, size_t n) {
    if (a == 0x200008 && ++calls == 2)
      put(a, 0);
    return read(a, out, n);
  };
  assert(!readUnitAuras(race, unit).valid);
  put(unit + 0xc54, 257);
  assert(!readUnitAuras(read, unit).valid);
  put(unit + 0xdd0, 0);
  assert(readUnitAuras(read, unit).valid);
  std::cout << "Marrowgar: phases, safe paths, escape, wall, prediction, "
               "freshness, watchdog and aura fixtures passed.\n";
}

