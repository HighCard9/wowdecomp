#include "IccObserver.h"
#include "IccNavigator/marrowgar.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/World.h"
#include <fstream>

// Preview only. No movement, targeting, spell casting or input bridge calls.
// Native object pointers are used only during the current render-thread scan.
namespace {
CVar *enabled = nullptr, *statusVar = nullptr;
Navigation navigation;
FlameTrails trails;
marrowgar::EscapeController escape;
marrowgar::Plan proposal;
NearbySnapshot snapshot;
ClientState player;
uint64_t lastScan = 0, observeUntil = 0, session = 0;
bool mapAttempted = false, mapLoaded = false;
std::string mapError, lastStatus;
std::ofstream trace;
unsigned traceLines = 0;
constexpr unsigned MaxLines = 4096;
CGxVertexPC vertices[MaxLines * 4];
uint16_t indices[MaxLines * 6];
unsigned lineCount = 0;

Position PositionOf(const CGObject_C* unit) {
    auto p = unit->GetPosition();
    return {p.x, p.y, p.z};
}
void Status(const std::string& value) {
    if (value == lastStatus) return;
    lastStatus = value;
    statusVar->SetReadOnly(false);
    statusVar->InternalSet(value.c_str(), true, false, false, false);
    statusVar->SetReadOnly(true);
    if (trace && traceLines++ < 10000) {
        trace << GetTickCount64() << '\t' << value << '\n';
        trace.flush();
    }
}
void Reset() {
    trails.clear(); escape.reset(); proposal = {}; snapshot = {}; player = {};
    session = lastScan = observeUntil = 0;
}
int32_t Collect(WOWGUID guid, void*) {
    if (++snapshot.visited > 5000) {
        snapshot.encounterComplete = false;
        return 0;
    }
    auto object = ClntObjMgrObjectPtr(guid, TYPEMASK_UNIT, __FILE__, __LINE__);
    if (!object || object->m_typeId != ID_UNIT || !object->m_obj) return 1;
    const auto entry = object->m_obj->m_entryID;
    if (entry != 36612 && entry != 36672) return 1;
    auto unit = static_cast<CGUnit_C*>(object);
    NearbyObject o;
    o.guid = guid.m_guid; o.type = 3; o.entry = entry;
    o.position = PositionOf(unit); o.facing = unit->GetFacing();
    o.positioned = validWorldPosition(o.position) && std::isfinite(o.facing)
        && !unit->GetTransportGUID();
    if (!o.positioned || !unit->m_unit || snapshot.objects.size() >= 512) {
        snapshot.encounterComplete = false;
        return 1;
    }
    o.health = static_cast<uint32_t>(std::max(0, unit->m_unit->m_unitFieldHealth));
    o.unitFlags = unit->m_unit->m_unitFieldFlags;
    o.combatReach = unit->m_unit->m_unitFieldCombatreach;
    if (entry == 36612) {
        auto count = unit->GetAuraCount();
        o.bossStateValid = count <= 256 && std::isfinite(o.combatReach)
            && o.combatReach > 0 && o.combatReach < 50;
        if (o.bossStateValid) {
            for (uint32_t i = 0; i < count; ++i) {
                const int32_t spell = unit->GetAuraSpellId(i);
                if (spell > 0) o.auras.push_back(static_cast<uint32_t>(spell));
                if (spell == 69076) o.boneStorm = true;
            }
        }
    }
    snapshot.objects.push_back(std::move(o));
    return 1;
}
void Scan(uint64_t now, CGPlayer_C* active) {
    const uint64_t guid = active->m_obj->m_guid.m_guid;
    const Position position = PositionOf(active);
    if (session != guid || (lastScan && (now - lastScan > 1000
        || objectDistanceSquared(player.position, position) > 10000))) {
        Reset(); session = guid; observeUntil = now + 5000;
    }
    lastScan = now;
    player = {};
    player.guid = guid; player.position = position; player.facing = active->GetFacing();
    player.transportGuid = active->GetTransportGUID();
    player.health = static_cast<uint32_t>(std::max(0, active->m_unit->m_unitFieldHealth));
    player.valid = guid && validWorldPosition(position) && std::isfinite(player.facing)
        && !player.transportGuid && player.health;
    proposal = {};
    if (!player.valid) {
        trails.clear(); escape.reset(); observeUntil = now + 5000;
        snapshot = {};
        Status("Waiting: living player on the ground required (check Bone Spike).");
        return;
    }
    snapshot = {};
    snapshot.valid = snapshot.encounterComplete = true;
    snapshot.timeMs = now; snapshot.playerGuid = guid; snapshot.origin = position;
    ClntObjMgrEnumVisibleObjects(Collect, nullptr);
    trails.add(snapshot);
    const auto boss = marrowgar::boss(&snapshot);
    const std::string phase = boss ? (boss->boneStorm ? "Bone Storm" : "Normal") : "No boss";
    const auto prefix = phase + " | triggers " + std::to_string(snapshot.objects.size() - (boss ? 1 : 0)) + " | ";
    if (!snapshot.encounterComplete) {
        observeUntil = now + 5000; escape.reset();
        Status(prefix + "incomplete observations; route hidden."); return;
    }
    const auto error = marrowgar::observationError(&snapshot, player, now);
    if (!error.empty()) { escape.reset(); Status(prefix + error); return; }
    if (!mapLoaded) { Status(prefix + mapError); return; }
    if (now < observeUntil) { Status(prefix + "collecting five seconds of flame history."); return; }
    // Bounded synchronous preview; never run the input driver.
    marrowgar::Hazards hazards(trails.visible(now, 5000), now);
    auto walk = [](Position a, Position b) {
        return navigation.walkableSegment(a, b, Navigation::wallMargin);
    };
    proposal = escape.step(player, *boss, hazards, walk, now);
    if (!proposal.emergency) proposal = marrowgar::plan(position, *boss, hazards, walk);
    Status(prefix + proposal.status);
}
void Line(Position a, Position b, float width, uint8_t r, uint8_t g, uint8_t blue, uint8_t alpha) {
    if (lineCount >= MaxLines) return;
    float dx = b.x-a.x, dy = b.y-a.y, length = std::hypot(dx,dy);
    if (length < .01f) { b.x += .05f; dx = .05f; length = .05f; }
    float x = -dy/length*width*.5f, y = dx/length*width*.5f;
    auto base = lineCount*4;
    vertices[base].p = C3Vector(a.x+x,a.y+y,a.z+.15f);
    vertices[base+1].p = C3Vector(a.x-x,a.y-y,a.z+.15f);
    vertices[base+2].p = C3Vector(b.x+x,b.y+y,b.z+.15f);
    vertices[base+3].p = C3Vector(b.x-x,b.y-y,b.z+.15f);
    for (unsigned i=0;i<4;++i) {
        auto& c = vertices[base+i].c;
        c.r=r; c.g=g; c.b=blue; c.a=alpha;
    }
    const unsigned order[] = {0,1,2,2,1,3};
    for (unsigned i=0;i<6;++i) indices[lineCount*6+i]=static_cast<uint16_t>(base+order[i]);
    ++lineCount;
}
void Draw(uint64_t now) {
    lineCount = 0;
    // Reserve space for the route; old trails are display estimates only.
    for (size_t i=1;i<proposal.points.size();++i)
        Line(proposal.points[i-1],proposal.points[i],.16f,70,255,110,240);
    if (!proposal.points.empty()) {
        auto p=proposal.points.back();
        Line({p.x-.4f,p.y-.4f,p.z},{p.x+.4f,p.y+.4f,p.z},.12f,255,240,80,255);
        Line({p.x-.4f,p.y+.4f,p.z},{p.x+.4f,p.y-.4f,p.z},.12f,255,240,80,255);
    }
    for (const auto& s: trails.visible(now,5000)) {
        const auto alpha=static_cast<uint8_t>(40+150*(1.0f-float(now-s.toMs)/5000));
        Line(s.from,s.to,.20f,70,170,255,alpha);
    }
    if (!lineCount || !g_theGxDevicePtr) return;
    g_theGxDevicePtr->RsPush();
    C44Matrix world; world.Translate(-CWorldScene::s_activeWorldView);
    g_theGxDevicePtr->XformPushAndSet(GxXform_World,world);
    GxRsSet(GxRs_VertexShader,static_cast<void*>(nullptr));
    GxRsSet(GxRs_PixelShader,static_cast<void*>(nullptr));
    for (uint32_t i=0;i<8;++i) GxRsSet(static_cast<EGxRenderState>(GxRs_Texture0+i),static_cast<void*>(nullptr));
    GxRsSet(GxRs_Lighting,0); GxRsSet(GxRs_Fog,0);
    GxRsSet(GxRs_DepthTest,1); GxRsSet(GxRs_DepthWrite,0);
    GxRsSet(GxRs_Culling,0); GxRsSet(GxRs_ColorWrite,15); GxRsSet(GxRs_BlendingMode,2);
    GxUpdateAlphaRef(GxRs_AlphaRef);
    GxPrimVertexPtr(lineCount*4,&vertices[0].p,sizeof(CGxVertexPC),nullptr,0,
        &vertices[0].c,sizeof(CGxVertexPC),nullptr,0,nullptr,0);
    GxPrimIndexPtr(lineCount*6,indices);
    CGxBatch batch; batch.m_primType=GxPrim_Triangles; batch.m_start=0;
    batch.m_count=lineCount*6; batch.m_minIndex=0;
    batch.m_maxIndex=static_cast<uint16_t>(lineCount*4-1);
    g_theGxDevicePtr->Draw(&batch,1);
    GxXformPop(GxXform_World); g_theGxDevicePtr->RsPop();
}
}
void IccObserverInitialize() {
    enabled=CVar::Register("iccObserver","Preview Marrowgar navigation",0,"0",nullptr,CATEGORY_GRAPHICS,false,nullptr,false);
    statusVar=CVar::Register("iccObserverStatus","ICC preview status",0,"Disabled",nullptr,CATEGORY_GRAPHICS,false,nullptr,false);
    statusVar->SetReadOnly(true);
}
void IccObserverRender() {
    if (!enabled || !statusVar) return;
    if (!enabled->m_intValue) {
        Reset(); mapAttempted=false; Status("Disabled");
        if (trace.is_open()) trace.close();
        return;
    }
    auto active=ClntObjMgrGetCurrent() ? ClntObjMgrGetActivePlayerObj() : nullptr;
    if (!active || !active->m_obj || !active->m_unit || World::QueryMapId()!=631) {
        Reset(); Status("Waiting: enter Icecrown Citadel (map 631)."); return;
    }
    if (!mapAttempted) {
        mapAttempted=true;
        wchar_t path[32768]{};
        const DWORD length=GetModuleFileNameW(nullptr,path,32768);
        if (!length || length>=32768) { mapLoaded=false; mapError="Executable path unavailable."; }
        else {
            auto directory=std::filesystem::path(path).parent_path();
            mapLoaded=navigation.load(directory/L"data"/L"mmaps",mapError,631);
            std::error_code ec; std::filesystem::create_directories(directory/L"Logs",ec);
            trace.open(directory/L"Logs"/L"ICCObserver.log",std::ios::trunc);
            traceLines=0;
        }
    }
    const uint64_t now=GetTickCount64();
    // Preview updates at 4 Hz. Frame drawing uses copied observations only.
    if (!lastScan || now-lastScan>=250) Scan(now,active);
    Draw(now);
}
