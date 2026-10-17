#include "MobRangeCircles.h"
#include <algorithm>
#include <cmath>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/World.h"

namespace {
constexpr uint32_t Segments = 64;
constexpr uint32_t MaxMobs = 32;
constexpr float Tau = 6.283185307179586f;
CVar* enabled = nullptr;
CVar* radius = nullptr;
CVar* distanceLimit = nullptr;

// Fixed storage, reused each frame. No object pointers survive enumeration.
struct RingBatch {
    CGxVertexPC vertices[MaxMobs * Segments * 2];
    uint16_t indices[MaxMobs * Segments * 6];
    uint32_t count = 0;
    CGPlayer_C* player = nullptr;
    C3Vector playerPosition;
    float radius = 5.0f;
    float distanceSquared = 3600.0f;
};
RingBatch rings;

float Bounded(float value, float fallback, float low, float high) {
    return std::isfinite(value) ? std::max(low, std::min(value, high)) : fallback;
}

int32_t CollectMob(WOWGUID guid, void* context) {
    RingBatch& batch = *static_cast<RingBatch*>(context);
    if (batch.count >= MaxMobs) return 0;
    CGObject_C* object = ClntObjMgrObjectPtr(guid, TYPEMASK_UNIT, __FILE__, __LINE__);
    if (!object || object->m_typeId != ID_UNIT) return 1;
    CGUnit_C* unit = static_cast<CGUnit_C*>(object);
    if (unit->IsDeadOrGhost() || !unit->CanBeTargetted() || !batch.player->CanAttack(unit)) return 1;
    const C3Vector center = unit->GetPosition();
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z)) return 1;
    const C3Vector delta = center - batch.playerPosition;
    const float distance2 = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (distance2 > batch.distanceSquared) return 1;
    const bool inside = distance2 <= batch.radius * batch.radius;
    CImVector color;
    color.r = inside ? 50 : 255;
    color.g = inside ? 230 : 155;
    color.b = inside ? 100 : 45;
    color.a = 230;
    const uint32_t base = batch.count * Segments * 2;
    const uint32_t indexBase = batch.count * Segments * 6;
    // A horizontal distance guide at the mob's feet; not a terrain decal or
    // a claim about server melee reach, aggro radius, or spell hit detection.
    for (uint32_t i = 0; i < Segments; ++i) {
        const float angle = Tau * static_cast<float>(i) / Segments;
        const float x = std::cos(angle);
        const float y = std::sin(angle);
        for (uint32_t edge = 0; edge < 2; ++edge) {
            const float r = batch.radius + (edge ? 0.06f : -0.06f);
            CGxVertexPC& vertex = batch.vertices[base + i * 2 + edge];
            vertex.p = C3Vector(center.x + x * r, center.y + y * r, center.z + 0.12f);
            vertex.c = color;
        }
        const uint16_t a = static_cast<uint16_t>(base + i * 2);
        const uint16_t b = static_cast<uint16_t>(base + ((i + 1) % Segments) * 2);
        uint16_t* indices = &batch.indices[indexBase + i * 6];
        indices[0] = a; indices[1] = a + 1; indices[2] = b;
        indices[3] = b; indices[4] = a + 1; indices[5] = b + 1;
    }
    ++batch.count;
    return 1;
}
}

void MobRangeCirclesInitialize() {
    enabled = CVar::Register("mobRangeCircles", "Show NPC distance-guide circles", 0, "1", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    radius = CVar::Register("mobRangeRadius", "NPC guide radius in yards (1-40)", 0, "5", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
    distanceLimit = CVar::Register("mobRangeDistance", "NPC guide drawing distance (5-100 yards)", 0, "60", nullptr, CATEGORY_GRAPHICS, false, nullptr, false);
}

void MobRangeCirclesRender() {
    if (!enabled || !radius || !distanceLimit || !enabled->m_intValue || !g_theGxDevicePtr || !ClntObjMgrGetCurrent()) return;
    CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();
    if (!player) return;
    rings.count = 0;
    rings.player = player;
    rings.playerPosition = player->GetPosition();
    rings.radius = Bounded(radius->m_floatValue, 5.0f, 1.0f, 40.0f);
    const float distance = Bounded(distanceLimit->m_floatValue, 60.0f, 5.0f, 100.0f);
    rings.distanceSquared = distance * distance;
    ClntObjMgrEnumVisibleObjects(CollectMob, &rings);
    rings.player = nullptr;
    if (!rings.count) return;

    g_theGxDevicePtr->RsPush();
    C44Matrix world;
    world.Translate(-CWorldScene::s_activeWorldView);
    g_theGxDevicePtr->XformPushAndSet(GxXform_World, world);
    GxRsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
    GxRsSet(GxRs_PixelShader, static_cast<void*>(nullptr));
    for (uint32_t i = 0; i < 8; ++i) {
        GxRsSet(static_cast<EGxRenderState>(GxRs_Texture0 + i), static_cast<void*>(nullptr));
    }
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_BlendingMode, 2);
    GxUpdateAlphaRef(GxRs_AlphaRef);
    const uint32_t vertexCount = rings.count * Segments * 2;
    const uint32_t indexCount = rings.count * Segments * 6;
    GxPrimVertexPtr(vertexCount, &rings.vertices[0].p, sizeof(CGxVertexPC), nullptr, 0,
        &rings.vertices[0].c, sizeof(CGxVertexPC), nullptr, 0, nullptr, 0);
    GxPrimIndexPtr(indexCount, rings.indices);
    CGxBatch draw;
    draw.m_primType = GxPrim_Triangles;
    draw.m_start = 0;
    draw.m_count = indexCount;
    draw.m_minIndex = 0;
    draw.m_maxIndex = static_cast<uint16_t>(vertexCount - 1);
    g_theGxDevicePtr->Draw(&draw, 1);
    GxXformPop(GxXform_World);
    g_theGxDevicePtr->RsPop();
}
