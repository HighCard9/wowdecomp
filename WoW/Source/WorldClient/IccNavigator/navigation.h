#pragma once
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include <array>
#include <filesystem>
#include <string>
#include <vector>

struct Position {
  float x = 0, y = 0, z = 0;
};
struct RouteResult {
  bool complete = false;
  std::string message;
  std::vector<Position> points;
  float distance = 0;
  float startSnap = 0, endSnap = 0;
};
class Navigation {
public:
  Navigation() = default;
  ~Navigation();
  Navigation(const Navigation &) = delete;
  Navigation &operator=(const Navigation &) = delete;
  bool load(const std::filesystem::path &directory, std::string &error,
            int mapId = 631);
  RouteResult route(Position start, Position end);
  static constexpr float wallMargin = 1.25f;
  float wallClearance(Position point, float radius = 3.0f) const;
  bool walkableSegment(Position start, Position end, float margin = 0) const;
  int tileCount() const { return tiles_; }
  int polygonCount() const { return polygons_; }
  const dtNavMesh *mesh() const { return mesh_; }
  static std::array<float, 3> toNav(Position p) { return {p.y, p.z, p.x}; }
  static Position toWorld(const float *p) { return {p[2], p[0], p[1]}; }

private:
  dtNavMesh *mesh_ = nullptr;
  dtNavMeshQuery *query_ = nullptr;
  int tiles_ = 0, polygons_ = 0;
  void clear();
  bool ground(Position point, dtPolyRef &ref, Position &snapped) const;
  bool offsetRoute(std::vector<Position> &points) const;
};
