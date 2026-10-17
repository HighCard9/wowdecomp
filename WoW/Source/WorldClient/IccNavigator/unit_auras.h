#pragma once
#include "client_state.h"
#include <array>
#include <vector>

// Read-only layout verified in the supplied 12340 binary at 4F8850,
// 4F8870 and 556E10. Never call these client functions.
struct UnitAuras {
  bool valid = false;
  std::vector<uint32_t> spells;
};
inline UnitAuras readUnitAuras(const MemoryRead &read, uint32_t unit) {
  UnitAuras result;
  auto get = [&](uint64_t address, auto &value) {
    return address >= 0x10000 && address + sizeof(value) <= 0x100000000ull &&
           read(uint32_t(address), &value, sizeof(value));
  };
  uint32_t mode = 0, count = 0, table = 0;
  if (!get(uint64_t(unit) + 0xdd0, mode))
    return result;
  count = mode;
  uint64_t data = uint64_t(unit) + 0xc50;
  if (mode == 0xffffffff) {
    if (!get(uint64_t(unit) + 0xc54, count) ||
        !get(uint64_t(unit) + 0xc58, table) || (count && !pointerOK(table)))
      return result;
    data = table;
  } else if (count > 16)
    return result;
  if (count > 256)
    return result;
  // Two matching samples reject changing tables, including unchanged counts.
  std::vector<uint32_t> first;
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<uint32_t> spells;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t spell = 0;
      if (!get(data + i * 24 + 8, spell) || spell > 1000000)
        return result;
      spells.push_back(spell);
    }
    if (!pass)
      first = spells;
    else if (first != spells)
      return result;
  }
  uint32_t endMode = 0, endCount = 0, endTable = 0;
  if (!get(uint64_t(unit) + 0xdd0, endMode) || endMode != mode)
    return result;
  if (mode == 0xffffffff &&
      (!get(uint64_t(unit) + 0xc54, endCount) || endCount != count ||
       !get(uint64_t(unit) + 0xc58, endTable) || endTable != table))
    return result;
  result.valid = true;
  result.spells = std::move(first);
  return result;
}
