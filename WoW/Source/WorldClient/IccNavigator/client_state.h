#pragma once
#include "navigation.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_set>

// Independently implemented, read-only 12340 object-manager walk.
// Addresses are relative to the stock image base (0x400000).
struct ClientState {
  bool valid = false;
  Position position{};
  float facing = 0;
  uint32_t health = 0, maxHealth = 0, level = 0;
  uint64_t guid = 0, transportGuid = 0;
  std::string message = "Waiting for an in-world character.";
};
using MemoryRead = std::function<bool(uint32_t, void *, size_t)>;
static_assert(sizeof(Position) == 12,
              "Client position must contain exactly three floats");
inline bool pointerOK(uint32_t p) {
  return p >= 0x10000 && p < 0xffff0000 && (p & 3) == 0;
}
inline ClientState readClientState(const MemoryRead &read, uint32_t imageBase,
                                   uint32_t &cachedPlayer) {
  ClientState s;
  auto get = [&](uint32_t a, auto &v) { return read(a, &v, sizeof(v)); };
  uint32_t connection = 0, manager = 0, object = 0;
  if (!get(imageBase + 0x879ce0, connection) || !pointerOK(connection) ||
      !get(connection + 0x2ed0, manager) || !pointerOK(manager) ||
      !get(manager + 0xc0, s.guid) || !s.guid) {
    cachedPlayer = 0;
    return s;
  }
  uint64_t checkGuid = 0;
  uint32_t type = 0;
  if (pointerOK(cachedPlayer) && get(cachedPlayer + 0x30, checkGuid) &&
      checkGuid == s.guid && get(cachedPlayer + 0x14, type) && type == 4)
    object = cachedPlayer;
  else {
    if (!get(manager + 0xac, object))
      object = 0;
    std::unordered_set<uint32_t> visited;
    bool found = false;
    for (int n = 0; pointerOK(object) && n < 5000; ++n) {
      if (!visited.insert(object).second)
        break;
      if (!get(object + 0x30, checkGuid) || !get(object + 0x14, type))
        break;
      if (checkGuid == s.guid && type == 4) {
        found = true;
        break;
      }
      uint32_t next = 0;
      if (!get(object + 0x3c, next))
        break;
      object = next;
    }
    if (!found) {
      cachedPlayer = 0;
      s.message =
          "Character object unavailable; loading or unsupported memory layout.";
      return s;
    }
  }
  uint32_t fields = 0;
  if (!get(object + 0x790, s.transportGuid) ||
      !get(object + 0x798, s.position) || !get(object + 0x7a8, s.facing) ||
      !get(object + 8, fields) || !pointerOK(fields) ||
      !get(fields + 0x60, s.health) || !get(fields + 0x80, s.maxHealth) ||
      !get(fields + 0xd8, s.level)) {
    cachedPlayer = 0;
    s.message = "A character memory read failed.";
    return s;
  }
  uint64_t finalGuid = 0;
  uint32_t finalConnection = 0, finalManager = 0;
  bool stable = get(imageBase + 0x879ce0, finalConnection) &&
                finalConnection == connection &&
                get(connection + 0x2ed0, finalManager) &&
                finalManager == manager && get(manager + 0xc0, finalGuid) &&
                finalGuid == s.guid;
  auto p = s.position;
  if (!stable || !std::isfinite(p.x) || !std::isfinite(p.y) ||
      !std::isfinite(p.z) || std::abs(p.x) > 50000 || std::abs(p.y) > 50000 ||
      std::abs(p.z) > 50000 || !std::isfinite(s.facing) || s.facing < 0 ||
      s.facing > 6.284f || s.maxHealth == 0 || s.health > s.maxHealth ||
      s.level < 1 || s.level > 80) {
    cachedPlayer = 0;
    s.message =
        "Character values failed validation; no live start is available.";
    return s;
  }
  uint64_t finalTransport = 0;
  if (!get(object + 0x790, finalTransport) ||
      finalTransport != s.transportGuid) {
    cachedPlayer = 0;
    s.message = "Transport changed during position read; waiting for stable "
                "ground position.";
    return s;
  }
  // 0x798 is relative to the transport when +0x790 is nonzero. Verified
  // in 12340: unit CMovement base +0x788; 4F4230/4F42A0 test base+8/+C.
  if (s.transportGuid) {
    cachedPlayer = object;
    s.message = "On a vehicle/transport (possibly Bone Spike). Free the "
                "character or exit it, then retry.";
    return s;
  }
  s.valid = true;
  s.message = "Live position and health";
  cachedPlayer = object;
  return s;
}
