// =============================================================================
//  game.h — Yet Another Zombie Defense HD model + feature worker.
//
//  Offsets are resolved by name through mono:: when the game is attached, so
//  nothing here is hardcoded except Unity's native camera matrix location,
//  which is validated against the camera's own view and projection matrices
//  before use.
// =============================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace game {

enum class Mode { None, Menu, Day, Night, GameOver };

struct Vec3 { float x, y, z; };

struct Entity {
    Vec3  pos;
    float hp, maxHp;
    int   type;          // enemy type, -1 for players
    bool  local;
    bool  frozen;
    std::string name;
};

struct Drop {
    Vec3  pos;
    int   type;          // 0 cash, 1 ammo, 2 health, 3 powerup
    int   sub;           // powerup type for powerups, else -1
    float timeLeft;      // seconds until it despawns
};

struct Snapshot {
    bool  valid = false;
    Mode  mode = Mode::None;
    float viewProj[16] = {};       // Unity column-major worldToClip
    bool  matrixOk = false;
    int   gameW = 0, gameH = 0;    // client size of the game window
    int   gameX = 0, gameY = 0;    // client origin in screen coordinates
    std::vector<Entity> players;
    std::vector<Entity> enemies;
    std::vector<Drop>   drops;
    // local player / economy readouts for the UI
    float hp = 0, maxHp = 0, cash = 0, ammo = 0;
    int   score = 0, night = 0, statPoints = 0, weaponId = -1, clips = 0;
    int   gameplayMode = 0;
    bool  havePlayer = false;
    float rot = 0;                 // local Player.rotation (radians, X/Z plane)
    bool  online = false;          // online multiplayer: writes are locked out
    int   barricades = 0, turrets = 0;
};

// Feature state. The UI writes these; the worker applies them.
struct Config {
    // player
    std::atomic<bool>  god{false}, infHealth{false}, maxStats{false};
    std::atomic<float> hpMult{1.f};
    std::atomic<bool>  pwSpeed{false}, pwInvuln{false}, pwMagnet{false}, pwReload{false}, pwInvis{false};
    // weapons
    std::atomic<bool>  infAmmo{false}, rapidFire{false}, instantReload{false}, infClips{false};
    // enemies
    std::atomic<bool>  freezeEnemies{false}, oneHit{false};
    std::atomic<bool>  cashMultOn{false};
    std::atomic<float> cashMult{3.f};
    // defenses
    std::atomic<bool>  godBarricades{false}, turretAmmo{false}, turretRapid{false};
    // aim (all offline only)
    std::atomic<bool>  aimbot{false}, aimDrawFov{true};
    std::atomic<int>   aimKey{1};                 // 0 always, 1 RMB, 2 Shift, 3 Mouse4, 4 Mouse5, 5 Alt
    std::atomic<float> aimFov{150.f};             // pixels around the cursor
    std::atomic<float> aimSmooth{4.f};            // 1 = snap
    std::atomic<float> aimRange{40.f};            // world units, shared by all aim features
    std::atomic<bool>  triggerbot{false};
    std::atomic<float> trigWidth{0.5f};           // how close the aim line must pass to an enemy
    std::atomic<bool>  silentAim{false};
    std::atomic<float> silentFov{60.f};           // degrees around the facing direction
    std::atomic<bool>  magicBullet{false}, magicInstant{false};
    // bullet swap: your guns fire another weapon's bullets
    std::atomic<bool>  bulletSwap{false};
    std::atomic<int>   bulletType{0};             // 0 rocket, 1 freeze, 2 blaster, 3 sniper
    // economy
    std::atomic<bool>  lockCash{false};
    std::atomic<float> lockCashValue{50000.f};
    // esp
    std::atomic<bool>  esp{false}, espLines{false}, espInfo{true}, espHealth{true};
    std::atomic<bool>  espEnemies{true}, espPlayers{true}, espLocal{false};
    std::atomic<float> espMaxDist{60.f};
    std::atomic<bool>  espDrops{true}, espDropTimer{true};
};

Config& Cfg();

// lifecycle (auto-attach thread lives in here)
void StartWorker();
void StopWorker();          // restores anything we changed, then stops
bool Attached();
bool Online();              // true in an online lobby (all writes disabled)
std::string Status();       // short attach status line for the banner

// thread-safe copy of the latest world state
Snapshot GetSnapshot();

// one-shot actions (queued to the worker)
void SetCash(float v);
void AddCash(float v);
void SetScore(int v);
void SetNight(int v);
void AddStatPoints(int n);
void FullHeal();
void WeakenAll();           // every live enemy to 1 HP

// config file next to the exe
std::string SaveConfig();
std::string LoadConfig();

const char* EnemyName(int type);

// Read-only check against the running game: resolve + one snapshot, no writes.
int SelfTest(const wchar_t* reportPath);

} // namespace game
