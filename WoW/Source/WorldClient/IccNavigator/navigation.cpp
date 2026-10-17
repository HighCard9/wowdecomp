#include "navigation.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <iomanip>
#include <sstream>

static_assert(sizeof(dtLink) == 16 && alignof(dtLink) == 4,
              "Legacy ICC tile links require a 16-byte, 4-aligned layout");

namespace {
std::vector<unsigned char> readFile(const std::filesystem::path &p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
    return {};
  auto size = f.tellg();
  if (size <= 0 || size > 64 * 1024 * 1024)
    return {};
  std::vector<unsigned char> b(static_cast<size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char *>(b.data()), b.size());
  return f ? b : std::vector<unsigned char>{};
}
float distance(Position a, Position b) {
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                   (a.z - b.z) * (a.z - b.z));
}
bool valid(Position p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
         std::abs(p.x) < 100000 && std::abs(p.y) < 100000 &&
         std::abs(p.z) < 100000;
}
size_t aligned(size_t n) { return (n + 3) & ~size_t(3); }
} // namespace
Navigation::~Navigation() { clear(); }
void Navigation::clear() {
  if (query_)
    dtFreeNavMeshQuery(query_);
  if (mesh_)
    dtFreeNavMesh(mesh_);
  query_ = nullptr;
  mesh_ = nullptr;
  tiles_ = polygons_ = 0;
}
bool Navigation::load(const std::filesystem::path &dir, std::string &error,
                      int mapId) {
  clear();
  error.clear();
  auto fail = [&](std::string s) {
    error = std::move(s);
    clear();
    return false;
  };
  if (mapId != 0 && mapId != 631)
    return fail("Unsupported map selection.");
  std::ostringstream name;
  name << std::setfill('0') << std::setw(3) << mapId;
  const std::string prefix = name.str();
  auto file = readFile(dir / (prefix + ".mmap"));
  if (file.size() != sizeof(dtNavMeshParams))
    return fail("Missing or invalid data/mmaps/" + prefix +
                ".mmap. Extract the whole ZIP first.");
  dtNavMeshParams params{};
  std::memcpy(&params, file.data(), sizeof(params));
  if (params.maxTiles < 1 || params.maxTiles > 4096 ||
      !std::isfinite(params.tileWidth) || params.tileWidth <= 0 ||
      !std::isfinite(params.tileHeight) || params.tileHeight <= 0)
    return fail("Invalid navigation map parameters.");
  mesh_ = dtAllocNavMesh();
  if (!mesh_ || dtStatusFailed(mesh_->init(&params)))
    return fail("Could not initialize Detour navigation.");
  std::vector<std::filesystem::path> paths;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    auto p = it->path();
    auto n = p.filename().string();
    if (n.size() == 14 && n.rfind(prefix, 0) == 0 && p.extension() == ".mmtile")
      paths.push_back(p);
  }
  std::sort(paths.begin(), paths.end());
  if (paths.empty())
    return fail("No navigation tiles found for map " + prefix + ".");
  for (const auto &p : paths) {
    auto bytes = readFile(p);
    if (bytes.size() < 20 + sizeof(dtMeshHeader))
      return fail("Truncated tile: " + p.filename().string());
    uint32_t fields[4];
    std::memcpy(fields, bytes.data(), 16);
    // This package deliberately supports its pinned legacy TC generator-v5 data
    // only.
    if (fields[0] != 0x4d4d4150 || fields[1] != DT_NAVMESH_VERSION ||
        fields[2] != 5 || fields[3] != bytes.size() - 20)
      return fail("Unsupported tile format: " + p.filename().string() +
                  ". Use the bundled dataset.");
    dtMeshHeader h{};
    std::memcpy(&h, bytes.data() + 20, sizeof(h));
    if (h.magic != DT_NAVMESH_MAGIC || h.version != DT_NAVMESH_VERSION)
      return fail("Wrong Detour tile version.");
    int counts[] = {h.polyCount,       h.vertCount,       h.maxLinkCount,
                    h.detailMeshCount, h.detailVertCount, h.detailTriCount,
                    h.bvNodeCount,     h.offMeshConCount};
    for (int n : counts)
      if (n < 0 || n > 1000000)
        return fail("Invalid tile element count.");
    if (h.offMeshBase < 0 || h.offMeshBase > h.polyCount ||
        h.detailMeshCount > h.polyCount)
      return fail("Invalid tile polygon layout.");
    const size_t expected =
        aligned(sizeof(h)) + aligned(sizeof(float) * 3 * h.vertCount) +
        aligned(sizeof(dtPoly) * h.polyCount) +
        aligned(sizeof(dtLink) * h.maxLinkCount) +
        aligned(sizeof(dtPolyDetail) * h.detailMeshCount) +
        aligned(sizeof(float) * 3 * h.detailVertCount) +
        aligned(4 * h.detailTriCount) +
        aligned(sizeof(dtBVNode) * h.bvNodeCount) +
        aligned(sizeof(dtOffMeshConnection) * h.offMeshConCount);
    if (expected != fields[3])
      return fail("Tile layout/ref-width mismatch: " + p.filename().string());
    auto *data =
        static_cast<unsigned char *>(dtAlloc(fields[3], DT_ALLOC_PERM));
    if (!data)
      return fail("Out of memory loading tiles.");
    std::memcpy(data, bytes.data() + 20, fields[3]);
    auto st = mesh_->addTile(data, fields[3], DT_TILE_FREE_DATA, 0, nullptr);
    if (dtStatusFailed(st)) {
      dtFree(data);
      return fail("Detour rejected tile " + p.filename().string());
    }
    ++tiles_;
    polygons_ += h.polyCount;
  }
  query_ = dtAllocNavMeshQuery();
  if (!query_ || dtStatusFailed(query_->init(mesh_, 32768)))
    return fail("Could not initialize route query.");
  return true;
}
RouteResult Navigation::route(Position start, Position end) {
  RouteResult r;
  if (!query_) {
    r.message = "Navigation data is not loaded.";
    return r;
  }
  if (!valid(start) || !valid(end)) {
    r.message = "Coordinates must be finite world coordinates.";
    return r;
  }
  auto a = toNav(start), b = toNav(end);
  dtQueryFilter filter;
  filter.setIncludeFlags(1);
  filter.setExcludeFlags(0);
  // Small vertical extent avoids snapping to another floor.
  const float ext[3] = {3.0f, 2.5f, 3.0f};
  float sa[3]{}, sb[3]{};
  dtPolyRef ra = 0, rb = 0;
  auto sta = query_->findNearestPoly(a.data(), ext, &filter, &ra, sa);
  auto stb = query_->findNearestPoly(b.data(), ext, &filter, &rb, sb);
  if (dtStatusFailed(sta) || !ra) {
    r.message = "Start is outside the ground mesh on this floor.";
    return r;
  }
  if (dtStatusFailed(stb) || !rb) {
    r.message = "Destination is outside the ground mesh on this floor.";
    return r;
  }
  r.startSnap = distance(start, toWorld(sa));
  r.endSnap = distance(end, toWorld(sb));
  if (r.startSnap > 3.5f || r.endSnap > 3.5f) {
    r.message = "Endpoint is too far from walkable ground.";
    return r;
  }
  std::vector<dtPolyRef> corridor(4096);
  int count = 0;
  auto st = query_->findPath(ra, rb, sa, sb, &filter, corridor.data(), &count,
                             corridor.size());
  if (dtStatusFailed(st) || dtStatusDetail(st, DT_OUT_OF_NODES) ||
      dtStatusDetail(st, DT_BUFFER_TOO_SMALL) ||
      dtStatusDetail(st, DT_PARTIAL_RESULT) || count <= 0 ||
      corridor[count - 1] != rb) {
    r.message = "No complete ground route. A teleport, elevator, off-mesh link "
                "or missing connection may be needed.";
    return r;
  }
  for (int i = 0; i < count; ++i) {
    const dtMeshTile *tile;
    const dtPoly *poly;
    if (dtStatusFailed(mesh_->getTileAndPolyByRef(corridor[i], &tile, &poly)) ||
        poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
      r.message = "Route requires an unsupported off-mesh transition.";
      return r;
    }
  }
  std::vector<float> straight(4096 * 3);
  std::vector<unsigned char> flags(4096);
  std::vector<dtPolyRef> refs(4096);
  int n = 0;
  st = query_->findStraightPath(sa, sb, corridor.data(), count, straight.data(),
                                flags.data(), refs.data(), &n, 4096,
                                DT_STRAIGHTPATH_ALL_CROSSINGS);
  if (dtStatusFailed(st) || dtStatusDetail(st, DT_BUFFER_TOO_SMALL) || n < 1 ||
      !(flags[n - 1] & DT_STRAIGHTPATH_END)) {
    r.message = "Route waypoint buffer incomplete.";
    return r;
  }
  for (int i = 0; i < n; ++i)
    r.points.push_back(toWorld(&straight[i * 3]));
  if (!offsetRoute(r.points)) {
    r.points.clear();
    r.message =
        "Cannot fit the wall-clearance route here. Choose a destination "
        "in a wider passage or away from the wall.";
    return r;
  }
  for (size_t i = 1; i < r.points.size(); ++i)
    r.distance += distance(r.points[i - 1], r.points[i]);
  r.complete = true;
  r.message = "Ground route with 1.25-yard target wall clearance (tapered at "
              "endpoints). Doors and moving obstacles are not evaluated.";
  return r;
}

bool Navigation::ground(Position p, dtPolyRef &ref, Position &snapped) const {
  if (!query_ || !valid(p))
    return false;
  auto a = toNav(p);
  float q[3], ext[3] = {0.3f, 1.5f, 0.3f};
  dtQueryFilter filter;
  filter.setIncludeFlags(1);
  if (dtStatusFailed(
          query_->findNearestPoly(a.data(), ext, &filter, &ref, q)) ||
      !ref)
    return false;
  snapped = toWorld(q);
  return std::hypot(snapped.x - p.x, snapped.y - p.y) < 0.08f &&
         std::abs(snapped.z - p.z) < 1.5f;
}
float Navigation::wallClearance(Position p, float radius) const {
  dtPolyRef ref;
  Position on;
  if (!ground(p, ref, on) || !std::isfinite(radius) || radius <= 0)
    return -1;
  auto a = toNav(on);
  float d, hit[3], normal[3];
  dtQueryFilter filter;
  filter.setIncludeFlags(1);
  const auto status = query_->findDistanceToWall(ref, a.data(), radius, &filter,
                                                 &d, hit, normal);
  return dtStatusFailed(status) || dtStatusDetail(status, DT_OUT_OF_NODES) ? -1
                                                                           : d;
}
bool Navigation::walkableSegment(Position start, Position end,
                                 float margin) const {
  if (!valid(start) || !valid(end) || !std::isfinite(margin) || margin < 0)
    return false;
  dtPolyRef ref;
  Position on;
  if (!ground(start, ref, on))
    return false;
  auto a = toNav(on), b = toNav(end);
  dtQueryFilter filter;
  filter.setIncludeFlags(1);
  float t, normal[3];
  dtPolyRef visited[256];
  int count = 0;
  auto status = query_->raycast(ref, a.data(), b.data(), &filter, &t, normal,
                                visited, &count, 256);
  if (dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL) ||
      t < 1 || count < 1)
    return false;
  float closest[3];
  if (dtStatusFailed(query_->closestPointOnPoly(visited[count - 1], b.data(),
                                                closest, nullptr)) ||
      std::hypot(closest[0] - b[0], closest[2] - b[2]) > 0.08f ||
      std::abs(closest[1] - end.z) > 1.5f)
    return false;
  if (margin <= 0)
    return true;
  int samples = std::max(1, int(std::ceil(distance(start, end) / 0.3f)));
  for (int i = 0; i <= samples; ++i) {
    float u = float(i) / samples;
    Position p{start.x + (end.x - start.x) * u, start.y + (end.y - start.y) * u,
               start.z + (end.z - start.z) * u};
    if (wallClearance(p, margin + 0.1f) < margin)
      return false;
  }
  return true;
}
bool Navigation::offsetRoute(std::vector<Position> &points) const {
  if (points.size() < 2 || distance(points.front(), points.back()) < 0.05f)
    return true;
  const auto start = points.front(), end = points.back();
  // Captured endpoints remain exact. Ease out of/into their existing clearance.
  const float startWall = wallClearance(start), endWall = wallClearance(end);
  if (startWall < 0 || endWall < 0)
    return false;
  auto required = [&](Position p) {
    return std::min({wallMargin, startWall + 0.5f * distance(start, p),
                     endWall + 0.5f * distance(end, p)});
  };
  std::vector<Position> dense{start};
  for (size_t i = 1; i < points.size(); ++i) {
    const auto a = points[i - 1], b = points[i];
    int count = std::max(1, int(std::ceil(distance(a, b) / 0.6f)));
    if (dense.size() + count > 8192)
      return false;
    for (int j = 1; j <= count; ++j) {
      float t = float(j) / count;
      dense.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                       a.z + (b.z - a.z) * t});
    }
  }
  dtQueryFilter filter;
  filter.setIncludeFlags(1);
  auto adjust = [&](Position &p) {
    for (int iteration = 0; iteration < 24; ++iteration) {
      dtPolyRef ref;
      Position on;
      if (!ground(p, ref, on)) {
        return false;
      }
      p = on;
      auto a = toNav(p);
      float d, hit[3], normal[3];
      auto status = query_->findDistanceToWall(ref, a.data(), wallMargin + 0.3f,
                                               &filter, &d, hit, normal);
      if (dtStatusFailed(status) || dtStatusDetail(status, DT_OUT_OF_NODES))
        return false;
      const float wanted = required(p);
      if (d >= wanted + 0.03f)
        break;
      if (!std::isfinite(normal[0]) || !std::isfinite(normal[2]) ||
          std::hypot(normal[0], normal[2]) < 0.5f) {
        // A funnel corner can lie exactly on a wall, giving no distance normal.
        const dtMeshTile *tile;
        const dtPoly *poly;
        if (dtStatusFailed(mesh_->getTileAndPolyByRef(ref, &tile, &poly)))
          return false;
        float cx = 0, cz = 0;
        for (int k = 0; k < poly->vertCount; ++k) {
          cx += tile->verts[poly->verts[k] * 3];
          cz += tile->verts[poly->verts[k] * 3 + 2];
        }
        normal[0] = cx / poly->vertCount - a[0];
        normal[2] = cz / poly->vertCount - a[2];
        float length = std::hypot(normal[0], normal[2]);
        if (length < 0.001f)
          return false;
        normal[0] /= length;
        normal[2] /= length;
      }
      const float push = std::min(0.4f, wanted + 0.07f - d);
      float desired[3] = {a[0] + normal[0] * push, a[1],
                          a[2] + normal[2] * push},
            moved[3];
      dtPolyRef visited[32];
      int count = 0;
      status = query_->moveAlongSurface(ref, a.data(), desired, &filter, moved,
                                        visited, &count, 32);
      if (dtStatusFailed(status) ||
          dtStatusDetail(status, DT_BUFFER_TOO_SMALL) || count < 1)
        return false;
      float onPoly[3];
      if (dtStatusFailed(query_->closestPointOnPoly(visited[count - 1], moved,
                                                    onPoly, nullptr)))
        return false;
      p = toWorld(onPoly);
    }
    return wallClearance(p) + 0.1f >= required(p);
  };
  for (size_t i = 1; i + 1 < dense.size(); ++i)
    if (!adjust(dense[i]))
      return false;
  auto clear = [&](Position a, Position b) {
    if (!walkableSegment(a, b))
      return false;
    int samples = std::max(1, int(std::ceil(distance(a, b) / 0.3f)));
    for (int i = 0; i <= samples; ++i) {
      float t = float(i) / samples;
      Position p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                 a.z + (b.z - a.z) * t};
      if (wallClearance(p, wallMargin + 0.2f) + 0.1f < required(p))
        return false;
    }
    return true;
  };
  // Offsetting a convex corner can leave a chord inside the desired margin.
  // Refine that chord on the mesh and project its midpoint outwards as well.
  for (int pass = 0; pass < 6; ++pass) {
    std::vector<Position> refined{dense.front()};
    bool changed = false;
    for (size_t i = 1; i < dense.size(); ++i) {
      auto a = dense[i - 1], b = dense[i];
      if (!clear(a, b)) {
        if (!walkableSegment(a, b))
          return false;
        Position mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f,
                     (a.z + b.z) * 0.5f};
        if (!adjust(mid))
          return false;
        refined.push_back(mid);
        changed = true;
      }
      refined.push_back(b);
    }
    if (refined.size() > 8192)
      return false;
    dense = std::move(refined);
    if (!changed)
      break;
  }
  // Keep only shortcuts whose whole segment fits; never smooth across a wall.
  std::vector<Position> output{dense.front()};
  for (size_t i = 0; i + 1 < dense.size();) {
    size_t next = std::min(i + 24, dense.size() - 1);
    while (next > i + 1 && !clear(dense[i], dense[next]))
      --next;
    if (!clear(dense[i], dense[next])) {
      return false;
    }
    if (distance(output.back(), dense[next]) > 0.02f)
      output.push_back(dense[next]);
    i = next;
  }
  points = std::move(output);
  return true;
}
