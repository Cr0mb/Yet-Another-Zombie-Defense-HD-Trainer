<div align="center">

# Yet Another Zombie Defense HD — External Trainer

### A fully external ImGui (DX11) overlay trainer for Yet Another Zombie Defense HD

Offsets are looked up by name · one native x64 executable · nothing is injected

</div>

---

## About

This trainer is **fully external**. Every feature reads and writes the game's memory from a
separate process with `ReadProcessMemory` / `WriteProcessMemory`. A transparent DirectX 11 ImGui
overlay is drawn on top of the game window.

The game is a **Unity 2019.4 (Mono)** title. Its scripting assembly (`Assembly-CSharp.dll`) is
managed code, so class layouts and static locations are **looked up by name at runtime** through
the game's own Mono runtime. Game updates usually don't require finding offsets again by hand.

### Name-resolved offsets (`mono.cpp`)

When the trainer attaches, it writes a small x64 stub (64 bytes) into the game and runs it on
short-lived remote threads. The stub calls these public Mono embedding API functions:

* `mono_get_root_domain`
* `mono_thread_attach` / `mono_thread_detach`
* `mono_image_loaded`
* `mono_class_from_name`
* `mono_class_get_field_from_name`
* `mono_field_get_offset`
* `mono_class_vtable`
* `mono_vtable_get_static_field_data`
* `mono_class_get_nested_types` (for nested classes such as `ShootingWeapon.Bullet`)

The calls return plain numbers: field offsets, class pointers and the addresses of static fields.
After resolution, nothing more runs inside the game. All further access is plain RPM/WPM.

### Type-checked writes

Every object is identified by its Mono class (`object → vtable → klass`) before anything is written
to it:

* Enemies must be `Enemy`.
* Weapons must be `ShootingWeapon` or one of its 13 concrete subclasses.
* Barricades must be `WoodenObstacle`, `SteelObstacle` or `ElectroObstacle`.
* Turrets must be `Turret`.

`List<T>` sizes and array bounds are checked before indexing. If an object doesn't match, it is
skipped.

### Validated camera matrix (ESP)

The ESP reads Unity's native `Camera` world-to-clip matrix (`m_CachedPtr + 0xDC`). That matrix is
only used if it equals `projection × view`, which are read from the same native camera
(`+0x9C`, `+0x5C`). If the layout ever moves, ESP turns itself off instead of drawing garbage.

### Online safety

The trainer watches `YAZDMultiplayer.isOnlineMultiplayer` all the time. When you join or host an
**online** game:

* **Every write feature is disabled.** These features would desync or crash network sessions.
  Their tabs are greyed out and show a notice.
* **Values the trainer changed are restored.** This covers immortality, stats and powerup timers.
* **Queued one-shot actions are dropped.**
* **ESP keeps working.** It only reads memory.

If the multiplayer state can't be read while a game is loaded, the trainer treats the game as
online. A failed read never turns writes on. Single-player and local co-op work normally.

### Auto-attach

There is no attach screen. A background thread looks for `YAZD_HD.exe` every 1.5 s. Once the game
is found, the trainer resolves offsets, loads the saved settings and the banner switches to
**attached**. If the game closes, the trainer goes back to waiting and attaches again when the
game is next launched.

---

## Building

Requires **Visual Studio 2026** (MSVC v145) and **CMake 3.20+**.

```bat
build.cmd
```

Output: `build\Release\YAZDTrainer.exe`. The CRT is linked statically, so no other DLLs are
needed.

ImGui 1.92.9 (DX11 + Win32 backends) is vendored in `third_party/imgui`.

### Read-only self-test

With the game running, this command resolves everything, takes one snapshot and writes a report.
It never writes game memory and opens no overlay:

```bat
build\Release\YAZDTrainer.exe --selftest report.txt
```

The report covers:

* resolution status
* game mode
* whether the camera matrix passed validation
* cash, score and night
* the local player
* enemies
* whether an online game is detected

---

## Usage

1. Launch Yet Another Zombie Defense HD.
2. Run `YAZDTrainer.exe`. It attaches by itself.
3. Press **`Insert`** to open or close the menu.
   * While the menu is closed, every click goes through to the game.
   * The game pauses while the menu has focus.
4. Close the trainer with the **EXIT** button or **`Ctrl+End`**. Either one restores everything
   the trainer changed.

Settings can be saved from the GAME tab. They are stored in `yazd_trainer.cfg` next to the exe and
load automatically when the trainer attaches.

---

# Features

## Player

* God Mode: you can't die, and health is kept full
* Infinite Health
* Max All Stats (all five stats at level 4): reload speed, movement speed, toughness, health
  regen and pickup range
* Powerups held active indefinitely: Speed Boost, Invulnerability, Magnet, Fast Reload,
  Invisibility
* Quick actions: Full Heal, +5 Stat Points

## Weapons (the weapon in your hands)

* Infinite Ammo: the magazine never empties
* Rapid Fire: no cooldown between shots
* Instant Reload: reloads finish immediately and still use up a clip
* Infinite Clips

## Aim

All four aim features work only offline and only during the night (combat) phase. They switch
themselves off while the menu is open or the game is not the foreground window. **Range** (5–80 m,
default 40 m) is shared by all four: enemies further away are ignored.

| Feature | What it does | How it works | Settings |
|---|---|---|---|
| **Aimbot** | Pulls your crosshair onto the enemy nearest the cursor while the hold key is down. | The game turns the player toward the point where the mouse ray meets the ground plane (`y = 1`). The aimbot projects the enemy's `y = 1` point to the screen and moves the real cursor there with `SetCursorPos`. The game then rotates the player by itself, so the aimbot never fights the game's per-frame rotation blending. | Hold key (Always, Right Mouse, Shift, Mouse 4, Mouse 5, Alt; default Right Mouse), FOV radius in pixels around the cursor (20–600, default 150), Smooth (1 = snap, up to 20; default 4), Draw FOV circle (red when a target is inside) |
| **Triggerbot** | Fires automatically while you are facing an enemy. | Reads `Player.rotation` and checks whether the line you face passes within *Width* of an enemy ahead of you. While it does, the left mouse button is held down with `SendInput`, since the game fires while the button is down. The button is released when no enemy is on the line. It never releases a button you are physically holding. | Width (0.2–1.5 m, default 0.5 m) |
| **Silent Aim** | New shots fly at an enemy even when you aren't aiming at it. Your visible aim isn't moved. | Each weapon keeps its bullets in a pool (`ShootingWeapon.bulletsList`). When one of your bullets is fired (`lifeTime < 0.15 s`), its `movementVector` is turned toward the enemy nearest your facing direction, keeping its speed. Each shot is redirected only once. | Cone (10–360°, default 60°) around the direction you face |
| **Magic Bullet** | Your bullets home on enemies. | Every tick, each of your bullets in flight has its `movementVector` steered toward a target. A new shot goes to the Silent Aim target; after that, each bullet chases the enemy nearest it. **Instant hit** also moves the bullet to just in front of its target once. The game's next segment test (`previousPosition → position`) then runs through the enemy, so the hit, damage, kill and rewards are processed by the game itself. | Instant hit (on/off) |

### Known limitations

* **Ranged weapons only.** Silent Aim and Magic Bullet only affect projectile bullets that live in
  a weapon's `bulletsList`. They have no effect on these weapons:
  * hitscan weapons (laser, tesla coil)
  * melee damage (chainsaw)
* **Only your own bullets are changed.** A bullet is used only if its `owner` is your player.
  Turrets and other players' bullets are never touched.
* **Lobbed shots keep their arc.** Only the horizontal (X/Z) direction is changed, so grenades still
  follow their normal height.
* **Sniper rounds can still pierce.** They keep flying after a hit, as normal. Homing follows a
  bullet for up to 3 s.
* **The aimbot needs a camera.** It uses the validated camera matrix like ESP does. If the matrix
  fails validation, the aimbot does nothing.
* **Disabled online.** Like every other gameplay feature, all four are blocked in online games.
* **Not yet tested in a live game.** The bullet and rotation field offsets are looked up by name
  and the build compiles. The in-game behaviour still has to be tried. The self-test reports
  whether the new offsets were found.

## Enemies

* Freeze Enemies
* One-Hit Kills: kills still go through the game, so cash, score and drops are awarded normally
* Weaken All Enemies: one-shot, sets every enemy to 1 HP
* Kill Cash Multiplier (1x–20x): scales the cash each enemy pays out

## Defense

* Indestructible Barricades: wooden, steel and electric barricades
* Turret Infinite Ammo
* Turret Rapid Fire

## Visuals (ESP)

* 2D boxes
* Snaplines
* Name and distance labels
* Health bars
* Range limit
* Boss highlight: the Demon Lord and the Reaper get an animated rainbow outline
* Frozen enemies are shown in cyan
* Filters: enemies, other players, local player

## Game

* Cash: set an amount, add +10k or +100k, or set a minimum it never drops below
* Score: set it
* Night number: set it during the day (shop) phase
* Config: save and load settings

---

## Technical notes

* **World space:** the arena lies on Unity's **X/Z** plane, with Y pointing up.
  `YAZDObject.position` is the world position.
* **Unused player slots:** `Level.players` always holds four pooled player slots. Unused slots
  are also flagged `isLocalPlayer`. The trainer uses the game's own `IsConnected` rule
  (`playerInfo.playerId != 0`) to find the real player.
* **Kill Cash Multiplier:** enemies are pooled. The game resets `cashAmount` whenever an enemy is
  respawned, so the multiplier detects the reset and re-applies itself. When it is switched off,
  the original value is put back.
* **Aim and bullets:** `Player.rotation` is in radians, and the shot direction is
  `(cos r, 0, sin r)` plus a random spread. Each frame, `YAZDObject.Update` sets
  `previousPosition = position`, then adds `movementVector × min(dt, 0.1)` to the position. After
  that, `Bullet.Update` tests the segment from `previousPosition` (pulled back 0.5) to `position`
  against each enemy's circle (`collisionRadius`).
* **Ammo values:** `ShootingWeapon.ammoLeft` is a fraction of a full magazine, where `1.0` is
  full. Spare clips are counted in the store as `Store.StoreItem.availableAmmo`.

---

## Project layout

```
src/main.cpp    overlay window, menu (GVoid theme), ESP
src/game.cpp    offset resolution, feature worker, snapshot, config, self-test
src/mono.cpp    remote Mono API resolver (stub + remote thread)
src/mem.cpp     process attach, RPM/WPM, module + window lookup
re/decomp/      decompiled Assembly-CSharp (reference)
re/proto/       Python prototypes used during reverse engineering
```

Not affiliated with Awesome Games Studio.
