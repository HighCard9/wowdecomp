#pragma once
#include "client_state.h"
#include "unit_auras.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <iomanip>
#include <sstream>
#include <vector>

// Read-only 12340 observations. A trigger marker is not a damage footprint.
struct NearbyObject {
  uint64_t guid = 0, caster = 0;
  uint32_t type = 0, entry = 0, spell = 0;
  Position position{};
  float facing = 0, radius = 0;
  bool positioned = false;
  bool bossStateValid = false, boneStorm = false;
  uint32_t health = 0, unitFlags = 0;
  float combatReach = 0;
  std::vector<uint32_t> auras;
  bool coldflame() const { return type == 3 && entry == 36672; }
};
struct NearbySnapshot {
  bool valid = false, complete = false, unitsComplete = false,
       encounterComplete = false;
  uint64_t playerGuid = 0, timeMs = 0;
  Position origin{};
  uint32_t visited = 0, skipped = 0, omitted = 0, outOfRange = 0;
  std::vector<std::string> readIssues;
  std::array<uint32_t, 8> types{};
  std::vector<NearbyObject> objects;
  std::string message = "Waiting for an object scan.";
};
inline bool validWorldPosition(Position p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
         std::abs(p.x) <= 50000 && std::abs(p.y) <= 50000 &&
         std::abs(p.z) <= 50000;
}
inline float objectDistanceSquared(Position a, Position b) {
  return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
         (a.z - b.z) * (a.z - b.z);
}
inline NearbySnapshot readNearbyObjects(const MemoryRead &read, uint32_t base,
                                        const ClientState &player,
                                        uint64_t timeMs) {
  NearbySnapshot s;
  s.timeMs = timeMs;
  s.playerGuid = player.guid;
  s.origin = player.position;
  if (!player.valid || !player.guid || !validWorldPosition(player.position)) {
    s.message = "No valid in-world character.";
    return s;
  }
  // Wide address arithmetic prevents wrapped reads on corrupt list pointers.
  auto get = [&](uint64_t a, auto &value) {
    return a >= 0x10000 && a + sizeof(value) <= 0x100000000ull &&
           read(static_cast<uint32_t>(a), &value, sizeof(value));
  };
  uint32_t connection = 0, manager = 0, current = 0;
  uint64_t guid = 0;
  if (!get(uint64_t(base) + 0x879ce0, connection) || !pointerOK(connection) ||
      !get(uint64_t(connection) + 0x2ed0, manager) || !pointerOK(manager) ||
      !get(uint64_t(manager) + 0xc0, guid) || guid != player.guid ||
      !get(uint64_t(manager) + 0xac, current)) {
    s.message = "Object manager unavailable or character changed.";
    return s;
  }
  std::unordered_set<uint32_t> addresses;
  std::unordered_set<uint64_t> guids;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  s.complete = true;
  bool unitsOK = true, encounterOK = true;
  auto issue = [&](const NearbyObject &o, const char *reason) {
    if (s.readIssues.size() < 16)
      s.readIssues.push_back("GUID " + std::to_string(o.guid) + " entry " +
                             std::to_string(o.entry) + ": " + reason);
  };
  std::string reason;
  while (current && !(current & 1)) {
    if (!pointerOK(current) || !addresses.insert(current).second) {
      reason = "invalid/cyclic object list";
      break;
    }
    if (s.visited >= 5000 || std::chrono::steady_clock::now() >= deadline) {
      reason = "scan budget reached";
      break;
    }
    ++s.visited;
    uint64_t address = current;
    uint32_t next = 0, fields = 0;
    NearbyObject o;
    if (!get(address + 0x3c, next) || !get(address + 0x14, o.type) ||
        !get(address + 0x30, o.guid)) {
      reason = "object-list read failed";
      break;
    }
    current = next;
    if (o.type < s.types.size())
      ++s.types[o.type];
    if (o.type != 3 && o.type != 6)
      continue;
    if (!o.guid || !get(address + 8, fields) || !pointerOK(fields)) {
      ++s.skipped;
      if (o.type == 3)
        unitsOK = encounterOK = false;
      issue(o, "identity/descriptor unavailable");
      continue;
    }
    uint64_t descriptorGuid = 0;
    bool ok = get(fields, descriptorGuid) && descriptorGuid == o.guid &&
              get(uint64_t(fields) + 0xc, o.entry);
    const bool identified = ok && o.entry;
    const char *failed = identified ? "position/facing unavailable or invalid"
                                    : "entry/GUID unavailable";
    if (o.type == 3) {
      uint64_t transport = 0;
      bool transportRead = get(address + 0x790, transport);
      bool world = transportRead && !transport;
      if (!world)
        failed = transportRead ? "vehicle/transport-relative NPC position"
                               : "NPC transport read unavailable";
      ok = ok && world && o.entry && get(address + 0x798, o.position) &&
           get(address + 0x7a8, o.facing) && validWorldPosition(o.position) &&
           std::isfinite(o.facing) && o.facing >= 0 && o.facing <= 6.284f;
      o.positioned = ok;
      // Optional display metadata; missing ordinary-NPC reach never blocks
      // movement.
      if (ok && o.entry != 36612) {
        float reach = 0;
        if (get(uint64_t(fields) + 0x108, reach) && std::isfinite(reach) &&
            reach > 0 && reach < 50)
          o.combatReach = reach;
      }
      if (ok && o.entry == 36612) {
        auto auras = readUnitAuras(read, uint32_t(address));
        o.bossStateValid = auras.valid &&
                           get(uint64_t(fields) + 0x60, o.health) &&
                           get(uint64_t(fields) + 0xec, o.unitFlags) &&
                           get(uint64_t(fields) + 0x108, o.combatReach) &&
                           std::isfinite(o.combatReach) && o.combatReach > 0 &&
                           o.combatReach < 50;
        o.auras = std::move(auras.spells);
        o.boneStorm =
            std::find(o.auras.begin(), o.auras.end(), 69076) != o.auras.end();
      }
    } else {
      failed = "dynamic-object metadata unavailable/invalid";
      // 12340 update fields: OBJECT_END=6; caster=6, spell=9, radius=10.
      // Dynamic-object coordinates are intentionally NOT guessed from unit
      // offsets.
      ok = ok && get(uint64_t(fields) + 0x18, o.caster) &&
           get(uint64_t(fields) + 0x24, o.spell) &&
           get(uint64_t(fields) + 0x28, o.radius) && o.spell &&
           std::isfinite(o.radius) && o.radius >= 0 && o.radius <= 200;
    }
    uint64_t finalGuid = 0;
    uint32_t finalType = 0, finalFields = 0;
    uint32_t finalEntry = 0;
    bool stable = get(address + 0x30, finalGuid) && finalGuid == o.guid &&
                  get(address + 0x14, finalType) && finalType == o.type &&
                  get(address + 8, finalFields) && finalFields == fields &&
                  get(uint64_t(fields) + 0xc, finalEntry) &&
                  finalEntry == o.entry;
    ok = ok && stable;
    if (!stable)
      failed = "object identity changed during scan";
    if (!ok) {
      ++s.skipped;
      if (o.type == 3) {
        unitsOK = false;
        // Known, stable unrelated NPCs cannot be Marrowgar/Coldflame.
        // Unknown or changing identities remain blocking.
        if (!identified || !stable || o.entry == 36612 || o.coldflame())
          encounterOK = false;
      }
      issue(o, failed);
      continue;
    }
    if (o.positioned && o.entry != 36612 &&
        objectDistanceSquared(o.position, player.position) > 150 * 150) {
      ++s.outOfRange;
      continue;
    }
    if (!guids.insert(o.guid).second)
      continue;
    // Keep scanning after reaching the display cap so later flame triggers win.
    if (s.objects.size() < 512)
      s.objects.push_back(o);
    else {
      ++s.omitted;
      if (o.type == 3)
        unitsOK = false;
      if (o.coldflame() || o.entry == 36612) {
        auto ordinary =
            std::find_if(s.objects.begin(), s.objects.end(), [](const auto &v) {
              return !v.coldflame() && v.entry != 36612;
            });
        if (ordinary != s.objects.end())
          *ordinary = o;
        else
          encounterOK = false;
      }
    }
  }
  uint32_t endConnection = 0, endManager = 0;
  uint64_t endGuid = 0;
  if (!get(uint64_t(base) + 0x879ce0, endConnection) ||
      endConnection != connection ||
      !get(uint64_t(connection) + 0x2ed0, endManager) ||
      endManager != manager || !get(uint64_t(manager) + 0xc0, endGuid) ||
      endGuid != player.guid) {
    s.objects.clear();
    s.complete = false;
    s.message = "Character/session changed during scan; discarded.";
    return s;
  }
  s.valid = true;
  s.unitsComplete = reason.empty() && unitsOK;
  s.encounterComplete = reason.empty() && encounterOK;
  s.complete = reason.empty() && !s.skipped && !s.omitted;
  s.message = s.complete
                  ? "Object scan complete."
                  : "Partial scan: " +
                        (reason.empty() ? std::string("objects skipped/capped")
                                        : reason);
  return s;
}

// One selected client only, at most 20 seconds / 400 frames. No stale live
// marks.
struct NearbyHistory {
  std::deque<NearbySnapshot> frames;
  uint64_t guid = 0;
  void clear() {
    frames.clear();
    guid = 0;
  }
  void add(const NearbySnapshot &s) {
    if (guid != s.playerGuid ||
        (!frames.empty() &&
         (s.timeMs < frames.back().timeMs ||
          objectDistanceSquared(s.origin, frames.back().origin) > 100 * 100)))
      clear();
    guid = s.playerGuid;
    frames.push_back(s);
    while (frames.size() > 400 ||
           (!frames.empty() && s.timeMs - frames.front().timeMs > 20000))
      frames.pop_front();
  }
  const NearbySnapshot *live(uint64_t now, const ClientState &player) const {
    if (frames.empty() || !player.valid)
      return nullptr;
    const auto &s = frames.back();
    if (!s.valid || s.playerGuid != player.guid || now < s.timeMs ||
        now - s.timeMs > 1000 ||
        objectDistanceSquared(s.origin, player.position) > 100 * 100)
      return nullptr;
    return &s;
  }
};

inline void writeNearbyJson(std::ostream &f, const NearbySnapshot &s) {
  f << "{\"time_ms\":" << s.timeMs
    << ",\"valid\":" << (s.valid ? "true" : "false")
    << ",\"complete\":" << (s.complete ? "true" : "false")
    << ",\"units_complete\":" << (s.unitsComplete ? "true" : "false")
    << ",\"encounter_complete\":" << (s.encounterComplete ? "true" : "false")
    << ",\"origin\":[" << s.origin.x << ',' << s.origin.y << ',' << s.origin.z
    << ']' << ",\"out_of_range\":" << s.outOfRange << ",\"player_guid\":\""
    << s.playerGuid << "\",\"visited\":" << s.visited
    << ",\"skipped\":" << s.skipped << ",\"omitted\":" << s.omitted
    << ",\"status\":" << std::quoted(s.message) << ",\"type_counts\":[";
  for (size_t i = 0; i < s.types.size(); ++i) {
    if (i)
      f << ',';
    f << s.types[i];
  }
  f << "],\"read_issues\":[";
  for (size_t i = 0; i < s.readIssues.size(); ++i) {
    if (i)
      f << ',';
    f << std::quoted(s.readIssues[i]);
  }
  f << "],\"objects\":[";
  for (size_t i = 0; i < s.objects.size(); ++i) {
    if (i)
      f << ',';
    const auto &o = s.objects[i];
    f << "{\"guid\":\"" << o.guid << "\",\"type\":" << o.type
      << ",\"entry\":" << o.entry
      << ",\"coldflame_trigger\":" << (o.coldflame() ? "true" : "false");
    if (o.positioned)
      f << ",\"position\":[" << o.position.x << ',' << o.position.y << ','
        << o.position.z << "],\"facing\":" << o.facing;
    else
      f << ",\"position\":null,\"spell_id\":" << o.spell
        << ",\"caster_guid\":\"" << o.caster << "\",\"radius\":" << o.radius;
    if (o.entry == 36612) {
      f << ",\"boss_state_valid\":" << (o.bossStateValid ? "true" : "false")
        << ",\"bone_storm\":" << (o.boneStorm ? "true" : "false")
        << ",\"health\":" << o.health << ",\"unit_flags\":" << o.unitFlags
        << ",\"combat_reach\":" << o.combatReach << ",\"auras\":[";
      for (size_t j = 0; j < o.auras.size(); ++j) {
        if (j)
          f << ',';
        f << o.auras[j];
      }
      f << ']';
    }
    f << '}';
  }
  f << "]}";
}
