#include "flame_trails.h"
#include <cassert>
#include <iostream>

NearbySnapshot snapshot(uint64_t time, std::vector<NearbyObject> objects) {
  NearbySnapshot s;
  s.valid = true;
  s.complete = true;
  s.playerGuid = 123;
  s.timeMs = time;
  s.origin = {0, 0, 42};
  s.objects = std::move(objects);
  return s;
}
NearbyObject flame(uint64_t guid, float x, float y = 0, float z = 42) {
  NearbyObject o;
  o.guid = guid;
  o.type = 3;
  o.entry = 36672;
  o.positioned = true;
  o.position = {x, y, z};
  return o;
}
int main() {
  // Faster sampling must not inflate inferred emitter speed when samples are
  // accumulated until the half-yard trail threshold is reached.
  FlameTrails fast;
  fast.add(snapshot(1000, {flame(1, 0)}));
  fast.add(snapshot(1050, {flame(1, .2f)}));
  fast.add(snapshot(1100, {flame(1, .4f)}));
  fast.add(snapshot(1150, {flame(1, .6f)}));
  auto fastLines = fast.visible(1200, 5000);
  assert(fastLines.size() == 2 && fastLines.back().fromMs == 1000 &&
         fastLines.back().toMs == 1150);
  FlameTrails t;
  t.add(snapshot(1000, {flame(1, 0)}));
  t.add(snapshot(1500, {flame(1, 5)}));
  t.add(snapshot(2000, {flame(1, 10)}));
  auto v = t.visible(2100, 8000);
  assert(v.size() == 3 && v[1].from.x == 0 && v[1].to.x == 5);
  t.add(snapshot(2200, {})); // emitter gone; trail still present
  assert(t.visible(2300, 8000).size() == 3);
  v = t.visible(9250,
                8000); // front expires individually and clips next segment
  assert(v.size() == 2 && std::abs(v[0].from.x - 2.5f) < .01f);
  assert(t.visible(10000, 8000).empty());
  assert(t.visible(2300, 0).empty());
  t.clear();
  // Four separate arms must never connect to each other.
  t.add(snapshot(1000, {flame(1, 0), flame(2, 0), flame(3, 0), flame(4, 0)}));
  t.add(snapshot(1500,
                 {flame(1, 5), flame(2, -5), flame(3, 0, 5), flame(4, 0, -5)}));
  v = t.visible(2000, 8000);
  assert(v.size() == 8);
  for (const auto &s : v)
    assert(s.from.x == 0 && s.from.y == 0);
  // Stationary endpoint never continually renews its inferred fire.
  for (uint64_t time = 2000; time <= 35000; time += 200)
    t.add(snapshot(
        time, {flame(1, 5), flame(2, -5), flame(3, 0, 5), flame(4, 0, -5)}));
  assert(t.visible(35000, 8000).empty());
  t.clear();
  t.add(snapshot(1000, {flame(1, 0)}));
  t.add(snapshot(3000, {flame(1, 5)}));
  v = t.visible(3100, 8000);
  assert(v.back().from.x == 5); // sample gap
  t.add(snapshot(3200, {flame(1, 100)}));
  v = t.visible(3300, 8000);
  assert(v.back().from.x == 100); // jump
  t.add(snapshot(3400, {flame(1, 105, 0, 50)}));
  v = t.visible(3500, 8000);
  assert(v.back().from.z == 50); // floor change
  auto changed = snapshot(3600, {flame(1, 0)});
  changed.playerGuid = 456;
  t.add(changed);
  assert(t.visible(3700, 8000).size() == 1);
  changed.timeMs = 3800;
  changed.origin.x = 200;
  changed.objects.clear();
  t.add(changed);
  assert(t.visible(3900, 8000).empty());
  t.add(snapshot(5000, {flame(1, 0)}));
  auto invalid = snapshot(5200, {});
  invalid.valid = false;
  t.add(invalid);
  assert(t.visible(5300, 8000).empty());
  t.add(snapshot(10000, {flame(1, 0)}));
  t.add(snapshot(1000, {flame(1, 5)})); // clock reset
  assert(t.visible(1100, 8000).size() == 1);
  t.add(snapshot(1000, {flame(1, 5)})); // duplicate
  assert(t.visible(1100, 8000).size() == 1);
  t.clear();
  std::vector<NearbyObject> many;
  for (int i = 1; i <= 200; ++i)
    many.push_back(flame(i, 0));
  for (uint64_t time = 1000; time < 10000; time += 100) {
    for (auto &o : many)
      o.position.x += 1;
    t.add(snapshot(time, many));
  }
  assert(t.visible(10000, 20000).size() <= FlameTrails::maxSegments);
  t.prune(40000);
  assert(t.visible(40000, 20000).empty());
  std::cout << "PASS: separate Coldflame trails, per-section expiry, despawn "
               "persistence, stationary heads, gaps, jumps, floors, resets and "
               "bounded storage\n";
}

