#include "game.h"
#include "log.h"
#include "mem.h"
#include "mono.h"

#include <Windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace game {
namespace {

// ---------------------------------------------------------------------------
//  Offsets (filled by Resolve(), by name)
// ---------------------------------------------------------------------------
struct Off {
    // statics (absolute addresses of the static slots)
    uint64_t sGame = 0, sStore = 0, sNight = 0, sDay = 0, sGameOver = 0, sMulti = 0;
    uint64_t kBarricade[3] = {}, kTurret = 0;
    int mOnline = -1, lMulti = -1, lObstacles = -1, obHp = -1, tWeapon = -1, eCash = -1;
    // classes
    uint64_t kPlayer = 0, kEnemy = 0, kNight = 0, kDay = 0, kGameOver = 0;
    std::set<uint64_t> kShooting;            // ShootingWeapon + all subclasses
    // TheGame
    int gLevel = -1, gCamera = -1, gMode = -1, gGameplay = -1;
    // Level
    int lPlayers = -1, lEnemies = -1;
    // Store / StoreItem
    int stCash = -1, stScore = -1, stItems = -1, siId = -1, siAmmo = -1;
    // NightMode
    int nCurrent = -1;
    // YAZDObject / Character
    int oPos = -1, cHp = -1, cMaxHp = -1, cImmortal = -1, cDead = -1, cFreeze = -1;
    // Player / PlayerInfo
    int pLocal = -1, pInfo = -1, piId = -1, piName = -1, pName = -1;
    int pWeapons = -1, pCurWeapon = -1, pPowerups = -1, pStats = -1, pStatPts = -1;
    // Weapon / ShootingWeapon
    int wId = -1, wAmmo = -1, wCooldown = -1, wReload = -1, wReloadTime = -1;
    // Enemy
    int eType = -1;
    // aim: YAZDObject motion + ShootingWeapon.Bullet
    int oPrev = -1, oMove = -1, oRot = -1, oLife = -1, oCollide = -1;
    int wBullets = -1, bActive = -1, bOwner = -1;
};

// Unity 2019.4 native Camera: worldToClip (column-major float4x4) and the
// two matrices it is built from. Validated as VP == P * V on every read.
constexpr int kCamView = 0x5C, kCamProj = 0x9C, kCamVP = 0xDC;

struct BoolKey { const char* k; std::atomic<bool>* v; };
Off              g_o;
Config           g_cfg;
std::atomic<bool> g_run{false}, g_attached{false};
std::thread      g_thread;
std::mutex       g_snapMx, g_qMx, g_statusMx;
Snapshot         g_snap;
std::string      g_status = "waiting for game";
std::vector<std::function<void()>> g_queue;

void SetStatus(const std::string& s) { std::lock_guard<std::mutex> lk(g_statusMx); g_status = s; }

// ---------------------------------------------------------------------------
//  Managed object helpers
// ---------------------------------------------------------------------------
uint64_t Klass(uint64_t obj) {
    uint64_t vt = mem::Read<uint64_t>(obj);
    return mem::Ptr(vt) ? mem::Read<uint64_t>(vt) : 0;
}

// List<T>: _items @0x10, _size @0x18; T[] data @0x20, length @0x18
template <class F> void ForList(uint64_t list, F&& f) {
    if (!mem::Ptr(list)) return;
    uint64_t arr = mem::Read<uint64_t>(list + 0x10);
    int n = mem::Read<int>(list + 0x18);
    if (!mem::Ptr(arr) || n <= 0 || n > 4096) return;
    int cap = (int)mem::Read<uint64_t>(arr + 0x18);
    if (n > cap) return;
    std::vector<uint64_t> items(n);
    if (!mem::ReadRaw(arr + 0x20, items.data(), n * 8)) return;
    for (int i = 0; i < n; ++i) if (mem::Ptr(items[i])) f(items[i]);
}

uint64_t ArrayElem(uint64_t arr, int idx, int size) {
    if (!mem::Ptr(arr)) return 0;
    uint64_t len = mem::Read<uint64_t>(arr + 0x18);
    if ((uint64_t)idx >= len) return 0;
    return arr + 0x20 + (uint64_t)idx * size;
}

std::string MonoStr(uint64_t s) {
    if (!mem::Ptr(s)) return {};
    int n = mem::Read<int>(s + 0x10);
    if (n <= 0 || n > 64) return {};
    wchar_t buf[65] = {};
    mem::ReadRaw(s + 0x14, buf, n * 2);
    char out[130] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, n, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
//  Resolve
// ---------------------------------------------------------------------------
bool Resolve() {
    if (!mono::Init()) { SetStatus("mono: " + mono::LastError()); return false; }
    uint64_t img = mono::Image("Assembly-CSharp");
    if (!img) { SetStatus("Assembly-CSharp not loaded yet"); return false; }

    Off o;
    bool ok = true;
    auto C = [&](const char* n) { uint64_t k = mono::Class(img, "", n); if (!k) { ok = false; log_::Error("class %s missing", n); } return k; };
    auto F = [&](uint64_t k, const char* n) { int v = k ? mono::FieldOffset(k, n) : -1; if (v <= 0) { ok = false; log_::Error("field %s missing", n); } return v; };
    auto S = [&](uint64_t k, const char* n) { uint64_t a = k ? mono::StaticAddr(k, n) : 0; if (!a) { ok = false; log_::Error("static %s missing", n); } return a; };

    uint64_t kGame = C("TheGame"), kLevel = C("Level"), kStore = C("Store"), kObj = C("YAZDObject");
    uint64_t kChar = C("Character"), kInfo = C("PlayerInfo"), kWeapon = C("Weapon"), kShoot = C("ShootingWeapon");
    o.kPlayer = C("Player"); o.kEnemy = C("Enemy");
    o.kNight = C("NightMode"); o.kDay = C("DayMode"); o.kGameOver = C("GameOverMode");
    uint64_t kItem = kStore ? mono::NestedClass(kStore, "StoreItem") : 0;
    if (!kItem) { ok = false; log_::Error("StoreItem missing"); }

    o.sGame = S(kGame, "instance"); o.sStore = S(kStore, "instance");
    o.sNight = S(o.kNight, "instance"); o.sDay = S(o.kDay, "instance");

    o.gLevel = F(kGame, "Level"); o.gCamera = F(kGame, "camera");
    o.gMode = F(kGame, "currentMode"); o.gGameplay = F(kGame, "gameplayMode");
    o.lPlayers = F(kLevel, "players"); o.lEnemies = F(kLevel, "enemies");
    o.stCash = F(kStore, "cash"); o.stScore = F(kStore, "score"); o.stItems = F(kStore, "items");
    o.siId = F(kItem, "id"); o.siAmmo = F(kItem, "availableAmmo");
    o.nCurrent = F(o.kNight, "currentNight");
    o.oPos = F(kObj, "position");
    o.cHp = F(kChar, "lifePoints"); o.cMaxHp = F(kChar, "maxLifePoints");
    o.cImmortal = F(kChar, "isImmortal"); o.cDead = F(kChar, "isDead"); o.cFreeze = F(kChar, "freezeAmount");
    o.pLocal = F(o.kPlayer, "isLocalPlayer"); o.pInfo = F(o.kPlayer, "playerInfo");
    o.piId = F(kInfo, "playerId"); o.piName = F(kInfo, "userName"); o.pName = F(o.kPlayer, "playerName");
    o.pWeapons = F(o.kPlayer, "weapons"); o.pCurWeapon = F(o.kPlayer, "currentWeapon");
    o.pPowerups = F(o.kPlayer, "powerupTimesLeft"); o.pStats = F(o.kPlayer, "playerStats");
    o.pStatPts = F(o.kPlayer, "numAvailableStatPoints");
    o.wId = F(kWeapon, "id");
    o.wAmmo = F(kShoot, "ammoLeft"); o.wCooldown = F(kShoot, "cooldown");
    o.wReload = F(kShoot, "reloadProgress"); o.wReloadTime = F(kShoot, "reloadTime");
    o.eType = F(o.kEnemy, "type");
    o.eCash = F(o.kEnemy, "cashAmount");
    uint64_t kMulti = C("YAZDMultiplayer"), kObst = C("Obstacle");
    o.sMulti = S(kMulti, "instance");
    o.mOnline = F(kMulti, "isOnlineMultiplayer");
    o.lMulti = F(kLevel, "yazdMultiplayer");
    o.lObstacles = F(kLevel, "obstacles");
    o.obHp = F(kObst, "lifePoints");
    o.kBarricade[0] = C("WoodenObstacle"); o.kBarricade[1] = C("SteelObstacle"); o.kBarricade[2] = C("ElectroObstacle");
    o.kTurret = C("Turret");
    o.tWeapon = F(o.kTurret, "weapon");
    o.oPrev = F(kObj, "previousPosition"); o.oMove = F(kObj, "movementVector");
    o.oRot = F(kObj, "rotation"); o.oLife = F(kObj, "lifeTime"); o.oCollide = F(kObj, "collisionEnabled");
    o.wBullets = F(kShoot, "bulletsList");
    uint64_t kBullet = kShoot ? mono::NestedClass(kShoot, "Bullet") : 0;
    if (!kBullet) { ok = false; log_::Error("ShootingWeapon.Bullet missing"); }
    o.bActive = F(kBullet, "IsActive"); o.bOwner = F(kBullet, "owner");

    // Every concrete ShootingWeapon. A weapon object is only written to after
    // its class is confirmed to be one of these.
    if (kShoot) o.kShooting.insert(kShoot);
    for (const char* n : { "Blaster", "Chainsaw", "ClusterGrenade", "Flamethrower", "FreezeGun", "Laser",
                           "Minigun", "Pistol", "RocketLauncher", "SMG", "Shotgun", "SniperRifle", "TeslaCoil" }) {
        uint64_t k = mono::Class(img, "", n);
        if (k && mono::Parent(k) == kShoot) o.kShooting.insert(k);
        else log_::Info("weapon class %s not a direct ShootingWeapon (skipped)", n);
    }

    if (!ok) { SetStatus("resolve failed: " + mono::LastError()); return false; }
    g_o = o;
    log_::Info("resolved: game=%llX store=%llX hp=+%X pos=+%X ammo=+%X weapons=%zu",
        o.sGame, o.sStore, o.cHp, o.oPos, o.wAmmo, o.kShooting.size());
    return true;
}

// ---------------------------------------------------------------------------
//  World access
// ---------------------------------------------------------------------------
uint64_t GameObj()  { return mem::Read<uint64_t>(g_o.sGame); }
uint64_t StoreObj() { return mem::Read<uint64_t>(g_o.sStore); }
uint64_t NightObj() { return mem::Read<uint64_t>(g_o.sNight); }

uint64_t LevelObj() {
    uint64_t g = GameObj();
    return mem::Ptr(g) ? mem::Read<uint64_t>(g + g_o.gLevel) : 0;
}

Mode CurrentMode() {
    uint64_t g = GameObj();
    if (!mem::Ptr(g)) return Mode::None;
    uint64_t m = mem::Read<uint64_t>(g + g_o.gMode);
    if (!mem::Ptr(m)) return Mode::Menu;
    uint64_t k = Klass(m);
    if (k == g_o.kNight) return Mode::Night;
    if (k == g_o.kDay) return Mode::Day;
    if (k == g_o.kGameOver) return Mode::GameOver;
    return Mode::Menu;
}

std::atomic<bool> g_online{false};

// Online lobby? Reads the YAZDMultiplayer singleton (or the Level's reference
// to it). If it cannot be read while a game is loaded, assume online so a
// failed read never enables writes.
bool IsOnline() {
    uint64_t m = mem::Read<uint64_t>(g_o.sMulti);
    if (!mem::Ptr(m)) { uint64_t l = LevelObj(); m = mem::Ptr(l) ? mem::Read<uint64_t>(l + g_o.lMulti) : 0; }
    if (!mem::Ptr(m)) { Mode md = CurrentMode(); return md != Mode::Menu && md != Mode::None; }
    return mem::Read<bool>(m + g_o.mOnline);
}

// Player.IsConnected: playerInfo.playerId != 0. The level keeps 4 pooled
// player slots; unused ones are also flagged local but have no id.
bool Connected(uint64_t p) {
    uint64_t info = mem::Read<uint64_t>(p + g_o.pInfo);
    return mem::Ptr(info) && mem::Read<uint64_t>(info + g_o.piId) != 0;
}

uint64_t LocalPlayer() {
    uint64_t found = 0;
    ForList(mem::Read<uint64_t>(LevelObj() + g_o.lPlayers), [&](uint64_t p) {
        if (!found && Klass(p) == g_o.kPlayer && mem::Read<bool>(p + g_o.pLocal) && Connected(p)) found = p;
    });
    return found;
}

uint64_t CurrentWeapon(uint64_t player) {
    int idx = mem::Read<int>(player + g_o.pCurWeapon);
    uint64_t list = mem::Read<uint64_t>(player + g_o.pWeapons);
    uint64_t w = 0, i = 0;
    ForList(list, [&](uint64_t x) { if ((int)i++ == idx) w = x; });
    return (w && g_o.kShooting.count(Klass(w))) ? w : 0;
}

uint64_t StoreItemById(int id) {
    uint64_t found = 0;
    ForList(mem::Read<uint64_t>(StoreObj() + g_o.stItems), [&](uint64_t it) {
        if (!found && mem::Read<int>(it + g_o.siId) == id) found = it;
    });
    return found;
}

bool ReadMatrix(uint64_t gameObj, float out[16]) {
    uint64_t cam = mem::Read<uint64_t>(gameObj + g_o.gCamera);
    if (!mem::Ptr(cam)) return false;
    uint64_t nat = mem::Read<uint64_t>(cam + 0x10);        // UnityEngine.Object.m_CachedPtr
    if (!mem::Ptr(nat)) return false;
    float V[16], P[16];
    if (!mem::ReadRaw(nat + kCamView, V, 64) || !mem::ReadRaw(nat + kCamProj, P, 64) ||
        !mem::ReadRaw(nat + kCamVP, out, 64)) return false;
    // VP must equal P*V (column-major). Rejects the matrix if the layout ever moves.
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += P[k * 4 + r] * V[c * 4 + k];
            float d = out[c * 4 + r];
            if (!std::isfinite(d) || std::fabs(s - d) > 1e-3f + 1e-3f * std::fabs(d)) return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
//  Feature application (one pass)
// ---------------------------------------------------------------------------
struct Saved {               // originals so features restore cleanly
    bool haveStats = false; int stats[5] = {};
    bool immortalSet = false;
    bool powerSet[5] = {};
} g_saved;

void ApplyPlayer(uint64_t p) {
    auto& c = g_cfg;
    float maxHp = mem::Read<float>(p + g_o.cMaxHp);
    bool dead = mem::Read<bool>(p + g_o.cDead);

    if (c.god) {
        if (!mem::Read<bool>(p + g_o.cImmortal)) mem::Write<bool>(p + g_o.cImmortal, true);
        g_saved.immortalSet = true;
    } else if (g_saved.immortalSet) {
        mem::Write<bool>(p + g_o.cImmortal, false);
        g_saved.immortalSet = false;
    }
    if ((c.god || c.infHealth) && !dead && maxHp > 0 && mem::Read<float>(p + g_o.cHp) < maxHp)
        mem::Write<float>(p + g_o.cHp, maxHp);

    // stats: 4 = max level for every stat
    uint64_t stats = mem::Read<uint64_t>(p + g_o.pStats);
    if (c.maxStats) {
        if (!g_saved.haveStats) {
            for (int i = 0; i < 5; ++i) if (uint64_t e = ArrayElem(stats, i, 4)) g_saved.stats[i] = mem::Read<int>(e);
            g_saved.haveStats = true;
        }
        for (int i = 0; i < 5; ++i) if (uint64_t e = ArrayElem(stats, i, 4)) if (mem::Read<int>(e) != 4) mem::Write<int>(e, 4);
    } else if (g_saved.haveStats) {
        for (int i = 0; i < 5; ++i) if (uint64_t e = ArrayElem(stats, i, 4)) mem::Write<int>(e, g_saved.stats[i]);
        g_saved.haveStats = false;
    }

    // powerups: hold the timer at 30 s; on disable let it run out (-1 = idle)
    uint64_t pw = mem::Read<uint64_t>(p + g_o.pPowerups);
    const bool want[5] = { c.pwSpeed, c.pwInvuln, c.pwMagnet, c.pwReload, c.pwInvis };
    for (int i = 0; i < 5; ++i) {
        uint64_t e = ArrayElem(pw, i, 4);
        if (!e) continue;
        if (want[i] && !dead) {
            if (mem::Read<float>(e) < 25.f) mem::Write<float>(e, 30.f);
            g_saved.powerSet[i] = true;
        } else if (g_saved.powerSet[i]) {
            mem::Write<float>(e, -1.f);
            g_saved.powerSet[i] = false;
        }
    }

    // current weapon
    if (uint64_t w = CurrentWeapon(p)) {
        if (c.infAmmo && mem::Read<float>(w + g_o.wAmmo) < 1.f) mem::Write<float>(w + g_o.wAmmo, 1.f);
        if (c.rapidFire && mem::Read<float>(w + g_o.wCooldown) > 0.f) mem::Write<float>(w + g_o.wCooldown, 0.f);
        if (c.instantReload) {
            float rp = mem::Read<float>(w + g_o.wReload);
            float rt = mem::Read<float>(w + g_o.wReloadTime);
            if (rp > 0.f && rt > 0.f && rp < rt) mem::Write<float>(w + g_o.wReload, rt);
        }
        if (c.infClips) {
            if (uint64_t it = StoreItemById(mem::Read<int>(w + g_o.wId)))
                if (mem::Read<int>(it + g_o.siAmmo) < 9) mem::Write<int>(it + g_o.siAmmo, 9);
        }
    }
}

void ApplyEnemies(uint64_t level) {
    auto& c = g_cfg;
    if (!c.freezeEnemies && !c.oneHit) return;
    ForList(mem::Read<uint64_t>(level + g_o.lEnemies), [&](uint64_t e) {
        if (Klass(e) != g_o.kEnemy || mem::Read<bool>(e + g_o.cDead)) return;
        if (c.freezeEnemies && mem::Read<float>(e + g_o.cFreeze) < 3.f) mem::Write<float>(e + g_o.cFreeze, 3.5f);
        if (c.oneHit) {
            float hp = mem::Read<float>(e + g_o.cHp);
            if (hp > 0.01f) mem::Write<float>(e + g_o.cHp, 0.01f);
        }
    });
}

// Kill reward: the game resets cashAmount when an enemy is (re)spawned from the
// pool, so any value other than the one we wrote is that enemy's base reward.
std::unordered_map<uint64_t, std::pair<float, float>> g_cash;   // obj -> (base, written)
void ApplyCashMult(uint64_t level) {
    if (!g_cfg.cashMultOn) {
        for (auto& [e, bw] : g_cash)
            if (mem::Read<float>(e + g_o.eCash) == bw.second) mem::Write<float>(e + g_o.eCash, bw.first);
        g_cash.clear();
        return;
    }
    float mult = g_cfg.cashMult;
    ForList(mem::Read<uint64_t>(level + g_o.lEnemies), [&](uint64_t e) {
        if (Klass(e) != g_o.kEnemy) return;
        float cur = mem::Read<float>(e + g_o.eCash);
        auto it = g_cash.find(e);
        if (it == g_cash.end() || cur != it->second.second) {
            if (cur <= 0.f || cur > 100000.f) return;
            g_cash[e] = { cur, cur * mult };
            mem::Write<float>(e + g_o.eCash, cur * mult);
        } else if (it->second.first * mult != cur) {               // slider moved
            it->second.second = it->second.first * mult;
            mem::Write<float>(e + g_o.eCash, it->second.second);
        }
    });
}

// Barricades: remember each one's highest HP and hold it there.
std::unordered_map<uint64_t, float> g_barricadeHp;
void ApplyDefenses(uint64_t level) {
    auto& c = g_cfg;
    if (!c.godBarricades) g_barricadeHp.clear();
    if (!c.godBarricades && !c.turretAmmo && !c.turretRapid) return;
    ForList(mem::Read<uint64_t>(level + g_o.lObstacles), [&](uint64_t ob) {
        uint64_t k = Klass(ob);
        if (c.godBarricades && (k == g_o.kBarricade[0] || k == g_o.kBarricade[1] || k == g_o.kBarricade[2])) {
            float hp = mem::Read<float>(ob + g_o.obHp);
            float& best = g_barricadeHp[ob];
            if (hp > best) best = hp;
            if (best > 0.f && hp > 0.f && hp < best) mem::Write<float>(ob + g_o.obHp, best);
        }
        if (k == g_o.kTurret) {
            uint64_t w = mem::Read<uint64_t>(ob + g_o.tWeapon);
            if (!mem::Ptr(w) || !g_o.kShooting.count(Klass(w))) return;
            if (c.turretAmmo && mem::Read<float>(w + g_o.wAmmo) < 1.f) mem::Write<float>(w + g_o.wAmmo, 1.f);
            if (c.turretRapid && mem::Read<float>(w + g_o.wCooldown) > 0.f) mem::Write<float>(w + g_o.wCooldown, 0.f);
        }
    });
}

// ---------------------------------------------------------------------------
//  Silent aim / magic bullet
//
//  Bullets are pooled per weapon (ShootingWeapon.bulletsList). Every frame the
//  game does previousPosition = position; position += movementVector * dt and
//  then tests the segment previousPosition(-0.5) -> position against enemy
//  circles. So steering movementVector redirects a shot, and placing position
//  just short of an enemy makes the next frame's segment pass through it.
//  Only x/z are touched; y (lobbed grenades) is left to the game.
// ---------------------------------------------------------------------------
struct AimTarget { float x, z; };
std::unordered_map<uint64_t, float> g_bulletLife;   // bullet -> lifeTime last seen
std::unordered_set<uint64_t> g_redirected, g_teleported;
std::atomic<unsigned> g_nSteer{0}, g_nTeleport{0};

float AngleWrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

std::vector<AimTarget> AliveEnemies(uint64_t level) {
    std::vector<AimTarget> v;
    ForList(mem::Read<uint64_t>(level + g_o.lEnemies), [&](uint64_t e) {
        if (Klass(e) != g_o.kEnemy || mem::Read<bool>(e + g_o.cDead)) return;
        if (mem::Read<float>(e + g_o.cHp) <= 0.f || !mem::Read<bool>(e + g_o.oCollide)) return;
        Vec3 p{}; mem::ReadRaw(e + g_o.oPos, &p, sizeof(p));
        v.push_back({ p.x, p.z });
    });
    return v;
}

// Enemy closest to direction `ang` (radians) seen from (ox, oz), inside the cone and range.
const AimTarget* BestInCone(const std::vector<AimTarget>& es, float ox, float oz, float ang, float halfFov, float range) {
    const AimTarget* best = nullptr; float bestScore = 1e9f;
    for (auto& e : es) {
        float dx = e.x - ox, dz = e.z - oz, d = std::sqrt(dx * dx + dz * dz);
        if (d < 0.05f || d > range) continue;
        float diff = std::fabs(AngleWrap(std::atan2(dz, dx) - ang));
        if (diff > halfFov) continue;
        float score = diff * 10.f + d * 0.02f;
        if (score < bestScore) { bestScore = score; best = &e; }
    }
    return best;
}

void ApplyAim(uint64_t player, uint64_t level) {
    auto& c = g_cfg;
    if (!c.silentAim && !c.magicBullet) { g_bulletLife.clear(); g_redirected.clear(); g_teleported.clear(); return; }
    auto enemies = AliveEnemies(level);
    if (enemies.empty()) return;
    Vec3 me{}; mem::ReadRaw(player + g_o.oPos, &me, sizeof(me));
    float rot = mem::Read<float>(player + g_o.oRot);
    float range = c.aimRange;
    const float kDeg = 0.01745329f;
    const AimTarget* aimT = BestInCone(enemies, me.x, me.z, rot, c.silentFov * 0.5f * kDeg, range);

    ForList(mem::Read<uint64_t>(player + g_o.pWeapons), [&](uint64_t w) {
        if (!g_o.kShooting.count(Klass(w))) return;
        ForList(mem::Read<uint64_t>(w + g_o.wBullets), [&](uint64_t b) {
            if (!mem::Read<bool>(b + g_o.bActive) || !mem::Read<bool>(b + g_o.oCollide)) return;
            if (mem::Read<uint64_t>(b + g_o.bOwner) != player) return;
            // pooled: a lifeTime that went backwards is a new shot from the same object
            float life = mem::Read<float>(b + g_o.oLife);
            auto it = g_bulletLife.find(b);
            if (it != g_bulletLife.end() && life < it->second) { g_redirected.erase(b); g_teleported.erase(b); }
            g_bulletLife[b] = life;
            if (life > 3.f) return;

            Vec3 pos{}, mv{};
            mem::ReadRaw(b + g_o.oPos, &pos, sizeof(pos));
            mem::ReadRaw(b + g_o.oMove, &mv, sizeof(mv));
            float spd = std::sqrt(mv.x * mv.x + mv.z * mv.z);
            if (spd < 0.5f) return;

            const AimTarget* t = nullptr;
            if (c.magicBullet) {
                // fresh shots go to the aim target; after that, home on whatever is nearest the bullet
                if (aimT && !g_redirected.count(b) && life < 0.15f) t = aimT;
                else t = BestInCone(enemies, pos.x, pos.z, std::atan2(mv.z, mv.x), 3.1416f, range);
            } else if (c.silentAim && life < 0.15f && !g_redirected.count(b)) {
                t = aimT;
            }
            if (!t) return;
            float dx = t->x - pos.x, dz = t->z - pos.z, d = std::sqrt(dx * dx + dz * dz);
            if (d < 0.01f) return;
            dx /= d; dz /= d;
            mem::Write<float>(b + g_o.oMove, dx * spd);
            mem::Write<float>(b + g_o.oMove + 8, dz * spd);
            g_redirected.insert(b);
            g_nSteer++;
            if (c.magicBullet && c.magicInstant && !g_teleported.count(b) && d > 0.6f) {
                // park it just short of the target; next frame's segment runs through it
                mem::Write<float>(b + g_o.oPos, t->x - dx * 0.35f);
                mem::Write<float>(b + g_o.oPos + 8, t->z - dz * 0.35f);
                g_teleported.insert(b);
                g_nTeleport++;
            }
        });
    });
    if (g_bulletLife.size() > 4096) { g_bulletLife.clear(); g_redirected.clear(); g_teleported.clear(); }
}

void ApplyEconomy() {
    if (!g_cfg.lockCash) return;
    uint64_t s = StoreObj();
    if (!mem::Ptr(s)) return;
    float v = g_cfg.lockCashValue;
    if (mem::Read<float>(s + g_o.stCash) < v) mem::Write<float>(s + g_o.stCash, v);
}

void RestoreAll() {
    if (!g_cash.empty()) { bool on = g_cfg.cashMultOn; g_cfg.cashMultOn = false; ApplyCashMult(LevelObj()); g_cfg.cashMultOn = on; }
    uint64_t p = LocalPlayer();
    if (!p) return;
    // Temporarily run ApplyPlayer with everything off so saved state is put back.
    auto& c = g_cfg;
    bool god = c.god, stats = c.maxStats, s0 = c.pwSpeed, s1 = c.pwInvuln, s2 = c.pwMagnet, s3 = c.pwReload, s4 = c.pwInvis;
    bool ammo = c.infAmmo, rf = c.rapidFire, ir = c.instantReload, cl = c.infClips, ih = c.infHealth;
    c.god = c.maxStats = c.pwSpeed = c.pwInvuln = c.pwMagnet = c.pwReload = c.pwInvis = false;
    c.infAmmo = c.rapidFire = c.instantReload = c.infClips = c.infHealth = false;
    ApplyPlayer(p);
    c.god = god; c.maxStats = stats; c.pwSpeed = s0; c.pwInvuln = s1; c.pwMagnet = s2; c.pwReload = s3; c.pwInvis = s4;
    c.infAmmo = ammo; c.rapidFire = rf; c.instantReload = ir; c.infClips = cl; c.infHealth = ih;
}

// ---------------------------------------------------------------------------
//  Snapshot (ESP + UI readouts)
// ---------------------------------------------------------------------------
Entity ReadEntity(uint64_t o) {
    Entity e{};
    mem::ReadRaw(o + g_o.oPos, &e.pos, sizeof(Vec3));
    e.hp = mem::Read<float>(o + g_o.cHp);
    e.maxHp = mem::Read<float>(o + g_o.cMaxHp);
    e.frozen = mem::Read<float>(o + g_o.cFreeze) > 1.f;
    return e;
}

void BuildSnapshot() {
    Snapshot s;
    s.valid = true;
    uint64_t g = GameObj();
    s.mode = CurrentMode();
    if (mem::Ptr(g)) {
        s.gameplayMode = mem::Read<int>(g + g_o.gGameplay);
        s.matrixOk = ReadMatrix(g, s.viewProj);
    }
    if (HWND h = mem::GameWindow()) {
        RECT rc{}; GetClientRect(h, &rc);
        POINT tl{ 0, 0 }; ClientToScreen(h, &tl);
        s.gameX = tl.x; s.gameY = tl.y; s.gameW = rc.right; s.gameH = rc.bottom;
    }
    if (uint64_t st = StoreObj(); mem::Ptr(st)) {
        s.cash = mem::Read<float>(st + g_o.stCash);
        s.score = mem::Read<int>(st + g_o.stScore);
    }
    if (uint64_t n = NightObj(); mem::Ptr(n)) s.night = mem::Read<int>(n + g_o.nCurrent);
    s.online = g_online;
    if (uint64_t l = LevelObj(); mem::Ptr(l))
        ForList(mem::Read<uint64_t>(l + g_o.lObstacles), [&](uint64_t ob) {
            uint64_t k = Klass(ob);
            if (k == g_o.kTurret) s.turrets++;
            else if (k == g_o.kBarricade[0] || k == g_o.kBarricade[1] || k == g_o.kBarricade[2]) s.barricades++;
        });

    uint64_t lvl = LevelObj();
    if (mem::Ptr(lvl) && (s.mode == Mode::Night || s.mode == Mode::Day)) {
        ForList(mem::Read<uint64_t>(lvl + g_o.lPlayers), [&](uint64_t p) {
            if (Klass(p) != g_o.kPlayer) return;
            uint64_t info = mem::Read<uint64_t>(p + g_o.pInfo);
            if (!Connected(p)) return;                         // pooled, unused slot
            bool local = mem::Read<bool>(p + g_o.pLocal) && !s.havePlayer;
            if (mem::Read<bool>(p + g_o.cDead) && !local) return;
            Entity e = ReadEntity(p);
            e.type = -1; e.local = local;
            e.name = MonoStr(mem::Read<uint64_t>(p + g_o.pName));
            if (e.name.empty() && mem::Ptr(info)) e.name = MonoStr(mem::Read<uint64_t>(info + g_o.piName));
            if (e.name.empty()) e.name = local ? "You" : "Player";
            if (local) {
                s.rot = mem::Read<float>(p + g_o.oRot);
                s.havePlayer = true; s.hp = e.hp; s.maxHp = e.maxHp;
                s.statPoints = mem::Read<int>(p + g_o.pStatPts);
                if (uint64_t w = CurrentWeapon(p)) {
                    s.weaponId = mem::Read<int>(w + g_o.wId);
                    s.ammo = mem::Read<float>(w + g_o.wAmmo);
                    if (uint64_t it = StoreItemById(s.weaponId)) s.clips = mem::Read<int>(it + g_o.siAmmo);
                }
            }
            s.players.push_back(std::move(e));
        });
        ForList(mem::Read<uint64_t>(lvl + g_o.lEnemies), [&](uint64_t en) {
            if (Klass(en) != g_o.kEnemy || mem::Read<bool>(en + g_o.cDead)) return;
            Entity e = ReadEntity(en);
            e.type = mem::Read<int>(en + g_o.eType);
            e.local = false;
            e.name = EnemyName(e.type);
            s.enemies.push_back(std::move(e));
        });
    }
    std::lock_guard<std::mutex> lk(g_snapMx);
    g_snap = std::move(s);
}

// ---------------------------------------------------------------------------
//  Worker
// ---------------------------------------------------------------------------
void Detach(const char* why) {
    log_::Info("detached: %s", why);
    mono::Shutdown();
    mem::Close();
    g_attached = false;
    g_saved = Saved{};
    g_cash.clear(); g_barricadeHp.clear(); g_online = false;
    g_bulletLife.clear(); g_redirected.clear(); g_teleported.clear();
    { std::lock_guard<std::mutex> lk(g_snapMx); g_snap = Snapshot{}; }
    SetStatus("waiting for game");
}

// ---------------------------------------------------------------------------
//  Test log: toggle changes immediately, plus a status line every 2 s with what
//  the trainer wrote and the state the game reports back. Lets online sessions
//  be checked afterwards (did values stick, did the game crash right after X).
// ---------------------------------------------------------------------------
}  // namespace
std::vector<BoolKey> BoolKeys();
namespace {
void TestLog(bool online) {
    using namespace std::chrono;
    static std::unordered_map<std::string, bool> prev;
    static auto next = steady_clock::now();
    for (auto& b : BoolKeys()) {
        bool v = b.v->load();
        auto it = prev.find(b.k);
        if (it == prev.end()) { prev[b.k] = v; continue; }
        if (it->second != v) { log_::Info("[test] %s %s %s", b.k, v ? "ON" : "off", online ? "(online)" : ""); it->second = v; }
    }
    if (steady_clock::now() < next) return;
    next = steady_clock::now() + seconds(2);
    std::string on;
    for (auto& b : BoolKeys()) if (b.v->load() && strncmp(b.k, "esp", 3) != 0) { on += b.k; on += ' '; }
    unsigned wOk = 0, wFail = 0; mem::WriteCounts(wOk, wFail);
    Mode m = CurrentMode();
    if (m != Mode::Night && m != Mode::Day && on.empty()) return;
    float hp = 0, mx = 0, ammo = -1, cash = 0; bool imm = false;
    int alive = 0;
    if (uint64_t p = LocalPlayer()) {
        hp = mem::Read<float>(p + g_o.cHp); mx = mem::Read<float>(p + g_o.cMaxHp); imm = mem::Read<bool>(p + g_o.cImmortal);
        if (uint64_t w = CurrentWeapon(p)) ammo = mem::Read<float>(w + g_o.wAmmo);
    }
    if (uint64_t st = StoreObj(); mem::Ptr(st)) cash = mem::Read<float>(st + g_o.stCash);
    ForList(mem::Read<uint64_t>(LevelObj() + g_o.lEnemies), [&](uint64_t e) {
        if (Klass(e) == g_o.kEnemy && !mem::Read<bool>(e + g_o.cDead)) alive++;
    });
    log_::Info("[test] %s mode=%d hp=%.2f/%.2f imm=%d ammo=%.2f cash=%.0f enemies=%d | writes ok=%u fail=%u steer=%u tp=%u | on: %s",
        online ? "ONLINE" : "offline", (int)m, hp, mx, imm, ammo, cash, alive, wOk, wFail,
        g_nSteer.exchange(0), g_nTeleport.exchange(0), on.empty() ? "-" : on.c_str());
}

void Loop() {
    using namespace std::chrono;
    auto nextAttach = steady_clock::now();
    auto nextSnap = steady_clock::now();
    while (g_run) {
        if (!g_attached) {
            if (steady_clock::now() >= nextAttach) {
                nextAttach = steady_clock::now() + milliseconds(1500);
                if (mem::Open(L"YAZD_HD.exe")) {
                    // give a just-launched game time to load its assemblies
                    if (Resolve()) {
                        g_attached = true;
                        SetStatus("attached");
                        LoadConfig();
                    } else {
                        mono::Shutdown();
                        mem::Close();
                    }
                }
            }
            std::this_thread::sleep_for(milliseconds(50));
            continue;
        }
        if (!mem::Alive()) { Detach("process exited"); continue; }

        // queued one-shot actions
        std::vector<std::function<void()>> q;
        { std::lock_guard<std::mutex> lk(g_qMx); q.swap(g_queue); }

        // Online lobby: drop every write, put back what we changed, read only.
        bool online = IsOnline();
        if (online != g_online) {
            log_::Info(online ? "[test] ONLINE game detected (features stay enabled for testing)" : "[test] offline");
            g_online = online;
        }
        TestLog(online);

        Mode m = CurrentMode();
        {
            for (auto& f : q) f();
            if (m == Mode::Night || m == Mode::Day) {
                uint64_t lvl = LevelObj();
                if (uint64_t p = LocalPlayer()) ApplyPlayer(p);
                if (m == Mode::Night) { ApplyEnemies(lvl); ApplyCashMult(lvl); }
                if (m == Mode::Night) if (uint64_t p = LocalPlayer()) ApplyAim(p, lvl);
                ApplyDefenses(lvl);
                ApplyEconomy();
            }
        }
        if (steady_clock::now() >= nextSnap) {
            BuildSnapshot();
            nextSnap = steady_clock::now() + milliseconds(8);
        }
        std::this_thread::sleep_for(milliseconds(2));
    }
    if (g_attached) {
        RestoreAll();
        mono::Shutdown();
        mem::Close();
        g_attached = false;
    }
}

void Enqueue(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(g_qMx);
    g_queue.push_back(std::move(f));
}

std::filesystem::path ConfigPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path() / L"yazd_trainer.cfg";
}

} // namespace

Config& Cfg() { return g_cfg; }

void StartWorker() {
    if (g_run) return;
    g_run = true;
    g_thread = std::thread(Loop);
}

void StopWorker() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

bool Attached() { return g_attached; }
bool Online() { return g_attached && g_online; }

std::string Status() { std::lock_guard<std::mutex> lk(g_statusMx); return g_status; }

Snapshot GetSnapshot() { std::lock_guard<std::mutex> lk(g_snapMx); return g_snap; }

void SetCash(float v) {
    Enqueue([v] { if (uint64_t s = StoreObj(); mem::Ptr(s)) mem::Write<float>(s + g_o.stCash, (std::max)(0.f, v)); });
}
void AddCash(float v) {
    Enqueue([v] { if (uint64_t s = StoreObj(); mem::Ptr(s)) mem::Write<float>(s + g_o.stCash, mem::Read<float>(s + g_o.stCash) + v); });
}
void SetScore(int v) {
    Enqueue([v] { if (uint64_t s = StoreObj(); mem::Ptr(s)) mem::Write<int>(s + g_o.stScore, (std::max)(0, v)); });
}
void SetNight(int v) {
    // Only between nights: the next night started uses this number.
    Enqueue([v] { if (CurrentMode() == Mode::Day) if (uint64_t n = NightObj(); mem::Ptr(n)) mem::Write<int>(n + g_o.nCurrent, (std::max)(1, v)); });
}
void AddStatPoints(int n) {
    Enqueue([n] { if (uint64_t p = LocalPlayer()) mem::Write<int>(p + g_o.pStatPts, (std::max)(0, mem::Read<int>(p + g_o.pStatPts) + n)); });
}
void FullHeal() {
    Enqueue([] {
        if (uint64_t p = LocalPlayer(); p && !mem::Read<bool>(p + g_o.cDead))
            mem::Write<float>(p + g_o.cHp, mem::Read<float>(p + g_o.cMaxHp));
    });
}
void WeakenAll() {
    Enqueue([] {
        if (CurrentMode() != Mode::Night) return;
        ForList(mem::Read<uint64_t>(LevelObj() + g_o.lEnemies), [](uint64_t e) {
            if (Klass(e) == g_o.kEnemy && !mem::Read<bool>(e + g_o.cDead) && mem::Read<float>(e + g_o.cHp) > 0.01f)
                mem::Write<float>(e + g_o.cHp, 0.01f);
        });
    });
}

int SelfTest(const wchar_t* path) {
    FILE* f = _wfopen(path, L"w");
    if (!f) return 3;
    int rc = 0;
    if (!mem::Open(L"YAZD_HD.exe")) { fprintf(f, "FAIL game not running\n"); fclose(f); return 1; }
    if (!Resolve()) { fprintf(f, "FAIL resolve: %s\n", Status().c_str()); rc = 2; }
    else {
        fprintf(f, "OK resolve  weapons classes=%zu\n", g_o.kShooting.size());
        BuildSnapshot();
        Snapshot s = GetSnapshot();
        static const char* modes[] = { "none", "menu", "day", "night", "gameover" };
        fprintf(f, "mode=%s gameplay=%d matrixOk=%d window=%dx%d@%d,%d\n", modes[(int)s.mode], s.gameplayMode,
            s.matrixOk, s.gameW, s.gameH, s.gameX, s.gameY);
        fprintf(f, "cash=%.0f score=%d night=%d online=%d barricades=%d turrets=%d\n", s.cash, s.score, s.night, IsOnline(), s.barricades, s.turrets);
        fprintf(f, "player=%d hp=%.2f/%.2f statPts=%d weapon=%d ammo=%.2f clips=%d\n", s.havePlayer, s.hp, s.maxHp,
            s.statPoints, s.weaponId, s.ammo, s.clips);
        for (auto& p : s.players) fprintf(f, "  player '%s' local=%d pos=(%.2f %.2f %.2f) hp=%.2f/%.2f\n",
            p.name.c_str(), p.local, p.pos.x, p.pos.y, p.pos.z, p.hp, p.maxHp);
        int shown = 0;
        for (auto& e : s.enemies) if (shown++ < 8) fprintf(f, "  enemy %s pos=(%.2f %.2f %.2f) hp=%.2f/%.2f frozen=%d\n",
            e.name.c_str(), e.pos.x, e.pos.y, e.pos.z, e.hp, e.maxHp, e.frozen);
        fprintf(f, "enemies=%zu\n", s.enemies.size());
        if (s.matrixOk) for (auto& p : s.players) if (p.local) {
            const float* M = s.viewProj;
            float cw = M[3]*p.pos.x + M[7]*p.pos.y + M[11]*p.pos.z + M[15];
            fprintf(f, "local ndc=(%.3f, %.3f)\n", (M[0]*p.pos.x + M[4]*p.pos.y + M[8]*p.pos.z + M[12]) / cw,
                (M[1]*p.pos.x + M[5]*p.pos.y + M[9]*p.pos.z + M[13]) / cw);
        }
    }
    mono::Shutdown();
    mem::Close();
    fclose(f);
    return rc;
}

const char* EnemyName(int t) {
    static const char* n[] = { "Zombie", "Ghoul", "Barghast", "Cerberus", "Mummy", "Vampire", "Demon Lord", "Reaper", "Workshop" };
    return (t >= 0 && t < 9) ? n[t] : "Enemy";
}

// --- config ---------------------------------------------------------------
std::vector<BoolKey> BoolKeys() {
    auto& c = g_cfg;
    return { {"god",&c.god},{"infHealth",&c.infHealth},{"maxStats",&c.maxStats},
             {"pwSpeed",&c.pwSpeed},{"pwInvuln",&c.pwInvuln},{"pwMagnet",&c.pwMagnet},{"pwReload",&c.pwReload},{"pwInvis",&c.pwInvis},
             {"infAmmo",&c.infAmmo},{"rapidFire",&c.rapidFire},{"instantReload",&c.instantReload},{"infClips",&c.infClips},
             {"freezeEnemies",&c.freezeEnemies},{"oneHit",&c.oneHit},{"lockCash",&c.lockCash},
             {"cashMultOn",&c.cashMultOn},{"aimbot",&c.aimbot},{"aimDrawFov",&c.aimDrawFov},{"triggerbot",&c.triggerbot},
             {"silentAim",&c.silentAim},{"magicBullet",&c.magicBullet},{"magicInstant",&c.magicInstant},{"godBarricades",&c.godBarricades},{"turretAmmo",&c.turretAmmo},{"turretRapid",&c.turretRapid},
             {"esp",&c.esp},{"espLines",&c.espLines},{"espInfo",&c.espInfo},{"espHealth",&c.espHealth},
             {"espEnemies",&c.espEnemies},{"espPlayers",&c.espPlayers},{"espLocal",&c.espLocal} };
}

std::string SaveConfig() {
    FILE* f = _wfopen(ConfigPath().c_str(), L"w");
    if (!f) return "Could not write settings file.";
    for (auto& b : BoolKeys()) fprintf(f, "%s=%d\n", b.k, b.v->load() ? 1 : 0);
    fprintf(f, "lockCashValue=%g\nespMaxDist=%g\ncashMult=%g\n", g_cfg.lockCashValue.load(), g_cfg.espMaxDist.load(), g_cfg.cashMult.load());
    fprintf(f, "aimKey=%d\naimFov=%g\naimSmooth=%g\naimRange=%g\ntrigWidth=%g\nsilentFov=%g\n", g_cfg.aimKey.load(),
        g_cfg.aimFov.load(), g_cfg.aimSmooth.load(), g_cfg.aimRange.load(), g_cfg.trigWidth.load(), g_cfg.silentFov.load());
    fclose(f);
    return "Settings saved.";
}

std::string LoadConfig() {
    FILE* f = _wfopen(ConfigPath().c_str(), L"r");
    if (!f) return "No saved settings yet.";
    char line[128];
    auto keys = BoolKeys();
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* val = eq + 1;
        for (auto& b : keys) if (strcmp(line, b.k) == 0) b.v->store(atoi(val) != 0);
        if (strcmp(line, "lockCashValue") == 0) g_cfg.lockCashValue = (float)atof(val);
        if (strcmp(line, "espMaxDist") == 0) g_cfg.espMaxDist = (float)atof(val);
        if (strcmp(line, "cashMult") == 0) g_cfg.cashMult = (float)atof(val);
        if (strcmp(line, "aimKey") == 0) g_cfg.aimKey = atoi(val);
        if (strcmp(line, "aimFov") == 0) g_cfg.aimFov = (float)atof(val);
        if (strcmp(line, "aimSmooth") == 0) g_cfg.aimSmooth = (float)atof(val);
        if (strcmp(line, "aimRange") == 0) g_cfg.aimRange = (float)atof(val);
        if (strcmp(line, "trigWidth") == 0) g_cfg.trigWidth = (float)atof(val);
        if (strcmp(line, "silentFov") == 0) g_cfg.silentFov = (float)atof(val);
    }
    fclose(f);
    return "Settings loaded.";
}

} // namespace game
