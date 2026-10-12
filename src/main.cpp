// =============================================================================
//  Yet Another Zombie Defense HD — external trainer.
//
//  A transparent, topmost DX11 + ImGui overlay covers the monitor the game is
//  on. INSERT opens the menu (the overlay then takes mouse input); with the
//  menu closed every click passes through to the game. ESP draws regardless.
//  Ctrl+END quits and restores everything the trainer changed.
// =============================================================================
#include "game.h"
#include "log.h"
#include "mem.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <filesystem>
#include <cmath>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#ifndef TRAINER_TITLE
#define TRAINER_TITLE "YAZD HD"
#endif

namespace {

HWND                    g_hwnd = nullptr;
ID3D11Device*           g_dev  = nullptr;
ID3D11DeviceContext*    g_ctx  = nullptr;
IDXGISwapChain*         g_swap = nullptr;
ID3D11RenderTargetView* g_rtv  = nullptr;
bool                    g_open = true, g_interactive = false;
RECT                    g_mon{};          // overlay rect in screen coordinates

// ---------------------------------------------------------------------------
//  Window / device
// ---------------------------------------------------------------------------
void CreateRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (g_dev && wp != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            CreateRTV();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool CreateOverlay() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"YAZDTrainerOverlay";
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_mon = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"YAZD Trainer", WS_POPUP,
        g_mon.left, g_mon.top, g_mon.right - g_mon.left, g_mon.bottom - g_mon.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    MARGINS m{ -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    ShowWindow(g_hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx))) return false;
    CreateRTV();
    return true;
}

// Click-through unless the menu is open.
void SetInteractive(bool on) {
    if (on == g_interactive) return;
    g_interactive = on;
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    ex = on ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
    if (on) SetForegroundWindow(g_hwnd);
}

// Keep the overlay on the monitor that shows the game.
void FollowGameMonitor(const game::Snapshot& s) {
    if (s.gameW <= 0) return;
    POINT c{ s.gameX + s.gameW / 2, s.gameY + s.gameH / 2 };
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) return;
    if (EqualRect(&mi.rcMonitor, &g_mon)) return;
    g_mon = mi.rcMonitor;
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_mon.left, g_mon.top,
        g_mon.right - g_mon.left, g_mon.bottom - g_mon.top, SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
//  Theme (crimson / slate, same palette as the Terraria trainer)
// ---------------------------------------------------------------------------
const ImVec4 Red  (0.78f, 0.13f, 0.16f, 1.f);
const ImVec4 RedHi(0.92f, 0.22f, 0.24f, 1.f);

void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 3; s.FrameRounding = 2; s.GrabRounding = 2; s.PopupRounding = 2;
    s.ScrollbarRounding = 2; s.ChildRounding = 3; s.TabRounding = 2;
    s.WindowBorderSize = 1; s.FrameBorderSize = 1; s.ChildBorderSize = 1;
    s.WindowPadding = ImVec2(10, 10); s.ItemSpacing = ImVec2(8, 6);
    s.FramePadding = ImVec2(8, 5); s.IndentSpacing = 14;

    const ImVec4 bg(0.085f, 0.088f, 0.095f, 0.98f);
    const ImVec4 panel(0.13f, 0.135f, 0.145f, 1.f);
    const ImVec4 panelHi(0.17f, 0.175f, 0.185f, 1.f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.105f, 0.115f, 1.f);
    c[ImGuiCol_PopupBg] = bg;
    c[ImGuiCol_Border] = ImVec4(0.30f, 0.10f, 0.11f, 0.55f);
    c[ImGuiCol_Text] = ImVec4(0.85f, 0.86f, 0.88f, 1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.49f, 0.52f, 1.f);
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = panelHi;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.10f, 0.11f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.10f, 0.11f, 0.75f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.13f, 0.15f, 0.85f);
    c[ImGuiCol_HeaderActive] = Red;
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.14f, 0.16f, 1.f);
    c[ImGuiCol_ButtonActive] = Red;
    c[ImGuiCol_CheckMark] = RedHi;
    c[ImGuiCol_SliderGrab] = Red;
    c[ImGuiCol_SliderGrabActive] = RedHi;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.25f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.32f, 1.f);
    c[ImGuiCol_ScrollbarGrabHovered] = Red;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.10f, 0.11f, 0.55f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.09f, 0.10f, 1.f);
}

// ---------------------------------------------------------------------------
//  Widgets bound to the worker's atomics
// ---------------------------------------------------------------------------
bool Check(const char* label, std::atomic<bool>& v) {
    bool b = v.load();
    if (ImGui::Checkbox(label, &b)) { v = b; return true; }
    return false;
}

bool SliderF(const char* label, std::atomic<float>& v, float lo, float hi, const char* fmt) {
    float f = v.load();
    if (ImGui::SliderFloat(label, &f, lo, hi, fmt)) { v = f; return true; }
    return false;
}

void Section(const char* label) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.82f, 0.30f, 0.32f, 1.f), "%s", label);
    ImGui::Separator();
    ImGui::Spacing();
}

void Swatch(ImU32 col) {   // small colored square for the legend
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x + h * 0.6f, p.y + h * 0.8f), col);
    ImGui::Dummy(ImVec2(h * 0.6f, h));
}

// ---------------------------------------------------------------------------
//  Menu
// ---------------------------------------------------------------------------
int         g_tab = 0;
const char* kTabs[] = { "PLAYER", "WEAPONS", "AIM", "ENEMIES", "DEFENSE", "VISUALS", "GAME" };
constexpr int kTabVisuals = 5;
bool        g_quit = false;
std::string g_cfgMsg;
float       g_cashInput = 100000.f;
int         g_scoreInput = 1000000, g_nightInput = 10;

void DrawBanner() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + 30), 0, 0, 0x33282CC8, 0x33282CC8);

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
    ImGui::TextColored(RedHi, TRAINER_TITLE);
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    bool on = game::Attached();
    ImVec4 col = on ? ImVec4(0.30f, 0.95f, 0.45f, 1.f) : ImVec4(0.95f, 0.65f, 0.25f, 1.f);
    // status dot
    ImVec2 d = ImGui::GetCursorScreenPos();
    float th = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(14, th));
    if (on) dl->AddCircleFilled(ImVec2(d.x + 9, d.y + th * 0.5f + 1), 4.f, ImGui::GetColorU32(col));
    else    dl->AddCircle(ImVec2(d.x + 9, d.y + th * 0.5f + 1), 4.f, ImGui::GetColorU32(col), 0, 1.5f);
    ImGui::SameLine();
    std::string st = on ? std::string("attached") : game::Status();
    ImGui::TextColored(col, "%s", st.c_str());
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 52);
    ImGui::TextColored(ImVec4(0.50f, 0.51f, 0.54f, 1.f), "[Insert]");
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Separator();
    ImGui::Spacing();
}

void DrawTabBar() {
    const int n = IM_ARRAYSIZE(kTabs);
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float tw = (ImGui::GetContentRegionAvail().x - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        bool active = g_tab == i;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? Red : ImVec4(0.14f, 0.145f, 0.155f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? RedHi : ImVec4(0.40f, 0.13f, 0.15f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, RedHi);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(1.f, 0.95f, 0.95f, 1.f) : ImVec4(0.62f, 0.63f, 0.66f, 1.f));
        if (ImGui::Button(kTabs[i], ImVec2(tw, 30))) g_tab = i;
        ImGui::PopStyleColor(4);
        if (i < n - 1) ImGui::SameLine();
    }
}

void TabPlayer(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("SURVIVAL");
    Check("God Mode (no damage / death)", c.god);
    Check("Infinite Health", c.infHealth);
    if (s.online) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Online: God Mode / Infinite Health only work when you host.\nAs a client the host decides your HP.");
    Check("Max All Stats (level 4)", c.maxStats);
    ImGui::TextDisabled("Reload speed, movement, toughness, regen, pickup range.");

    Section("POWERUPS (held active)");
    Check("Speed Boost", c.pwSpeed);
    Check("Invulnerability", c.pwInvuln);
    Check("Magnet (pickup range)", c.pwMagnet);
    Check("Fast Reload", c.pwReload);
    Check("Invisibility", c.pwInvis);

    Section("QUICK ACTIONS");
    if (ImGui::Button("Full Heal")) game::FullHeal();
    ImGui::SameLine();
    if (ImGui::Button("+5 Stat Points")) game::AddStatPoints(5);
    if (s.havePlayer)
        ImGui::TextDisabled("HP %.0f%%   stat points %d", s.maxHp > 0 ? s.hp / s.maxHp * 100.f : 0.f, s.statPoints);
}

void TabWeapons(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("CURRENT WEAPON");
    Check("Infinite Ammo (never empties)", c.infAmmo);
    Check("Rapid Fire (no cooldown)", c.rapidFire);
    Check("Instant Reload", c.instantReload);
    Check("Infinite Clips", c.infClips);
    ImGui::TextDisabled("Applies to whatever weapon is in your hands.");
    if (s.havePlayer && s.weaponId >= 0)
        ImGui::TextDisabled("magazine %.0f%%   spare clips %d", s.ammo * 100.f, s.clips);

    Section("EXPLOSIVE ROUNDS");
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 0.35f, 0.35f, 1.f));
    Check("Swap Bullet Type", c.bulletSwap);
    ImGui::PopStyleColor();
    static const char* kBulletTypes[] = { "Rocket (explodes)", "Freeze shot", "Blaster bolt", "Sniper round (pierces)" };
    int bt = c.bulletType;
    ImGui::SetNextItemWidth(-70);
    if (ImGui::Combo("Bullets", &bt, kBulletTypes, IM_ARRAYSIZE(kBulletTypes))) c.bulletType = bt;
    ImGui::TextDisabled("Pistol, SMG, shotgun, minigun, sniper, blaster and freeze gun\nfire these instead. Fire rate stays your gun's own.");
}

void TabEnemies(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("ENEMIES");
    Check("Freeze Enemies (lock in place)", c.freezeEnemies);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 0.35f, 0.35f, 1.f));
    Check("One-Hit Kills", c.oneHit);
    ImGui::PopStyleColor();
    ImGui::TextWrapped("Kills still go through the game, so cash, score and drops are awarded normally.");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.10f, 0.10f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.15f, 0.15f, 1.f));
    if (ImGui::Button("Weaken All Enemies (once, 1 HP)", ImVec2(-1, 0))) game::WeakenAll();
    ImGui::PopStyleColor(2);
    ImGui::TextDisabled("Enemies alive: %d", (int)s.enemies.size());

    Section("REWARDS");
    Check("Kill Cash Multiplier", c.cashMultOn);
    ImGui::SetNextItemWidth(-70);
    SliderF("x cash", c.cashMult, 1.f, 20.f, "%.1fx");
    ImGui::TextDisabled("Scales the cash each enemy pays out when killed.");
}

const char* kAimKeys[] = { "Always", "Right Mouse", "Shift", "Mouse 4", "Mouse 5", "Alt" };

void TabAim(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("AIMBOT");
    Check("Aimbot (moves your crosshair)", c.aimbot);
    int k = c.aimKey;
    ImGui::SetNextItemWidth(-70);
    if (ImGui::Combo("Hold key", &k, kAimKeys, IM_ARRAYSIZE(kAimKeys))) c.aimKey = k;
    ImGui::SetNextItemWidth(-70);
    SliderF("FOV", c.aimFov, 20.f, 600.f, "%.0f px");
    ImGui::SetNextItemWidth(-70);
    SliderF("Smooth", c.aimSmooth, 1.f, 20.f, "%.1f");
    Check("Draw FOV circle", c.aimDrawFov);

    Section("TRIGGERBOT");
    Check("Triggerbot (fires when aimed at an enemy)", c.triggerbot);
    ImGui::SetNextItemWidth(-70);
    SliderF("Width", c.trigWidth, 0.2f, 1.5f, "%.2f m");
    ImGui::TextDisabled("Holds fire while the line you face passes this close to an enemy.");

    Section("BULLETS");
    Check("Silent Aim", c.silentAim);
    ImGui::TextDisabled("Shots go to the closest enemies (spread across the crowd). Your aim isn't moved.");
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 0.35f, 0.35f, 1.f));
    Check("Magic Bullet (homing)", c.magicBullet);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    Check("Instant hit", c.magicInstant);
    ImGui::TextDisabled("Shots in flight steer to the nearest enemy; instant hit puts them on target.");

    Section("SHARED");
    ImGui::SetNextItemWidth(-70);
    SliderF("Range", c.aimRange, 5.f, 80.f, "%.0f m");
    if (s.mode != game::Mode::Night) ImGui::TextDisabled("Aim features run during the night (combat) phase.");
}

void TabDefense(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("BARRICADES");
    Check("Indestructible Barricades", c.godBarricades);
    ImGui::TextDisabled("Wooden, steel and electric barricades keep full HP.");
    Section("TURRETS");
    Check("Turret Infinite Ammo", c.turretAmmo);
    Check("Turret Rapid Fire", c.turretRapid);
    if (s.valid) ImGui::TextDisabled("placed: %d barricades, %d turrets", s.barricades, s.turrets);
}

void TabVisuals() {
    auto& c = game::Cfg();
    Section("ESP");
    Check("2D Box ESP (master)", c.esp);
    Check("Snaplines", c.espLines);
    Check("Entity Info Text", c.espInfo);
    Check("Health Bars", c.espHealth);
    ImGui::SetNextItemWidth(-70);
    SliderF("Range", c.espMaxDist, 10.f, 100.f, "%.0f m");

    Section("TARGETS");
    Check("Enemies", c.espEnemies);
    Check("Other Players", c.espPlayers);
    Check("Local Player", c.espLocal);

    Section("DROPS");
    Check("Drop ESP (cash, ammo, health, powerups)", c.espDrops);
    Check("Despawn timer", c.espDropTimer);

    Section("LEGEND");
    Swatch(IM_COL32(255, 77, 77, 255));  ImGui::SameLine(); ImGui::TextDisabled("enemy");
    ImGui::SameLine(); Swatch(IM_COL32(255, 60, 200, 255)); ImGui::SameLine(); ImGui::TextDisabled("boss");
    ImGui::SameLine(); Swatch(IM_COL32(90, 200, 255, 255)); ImGui::SameLine(); ImGui::TextDisabled("frozen");
    Swatch(IM_COL32(255, 215, 60, 255)); ImGui::SameLine(); ImGui::TextDisabled("cash");
    ImGui::SameLine(); Swatch(IM_COL32(70, 230, 110, 255)); ImGui::SameLine(); ImGui::TextDisabled("health");
    ImGui::SameLine(); Swatch(IM_COL32(190, 110, 255, 255)); ImGui::SameLine(); ImGui::TextDisabled("powerup");
    Swatch(IM_COL32(255, 150, 40, 255)); ImGui::SameLine(); ImGui::TextDisabled("player / ammo");
    ImGui::SameLine(); Swatch(IM_COL32(60, 255, 120, 255)); ImGui::SameLine(); ImGui::TextDisabled("you");
}

void TabGame(const game::Snapshot& s) {
    auto& c = game::Cfg();
    Section("CASH");
    ImGui::SetNextItemWidth(140);
    ImGui::InputFloat("##cash", &g_cashInput, 1000.f, 10000.f, "%.0f");
    ImGui::SameLine(); if (ImGui::Button("Set")) game::SetCash(g_cashInput);
    ImGui::SameLine(); if (ImGui::Button("+10k")) game::AddCash(10000.f);
    ImGui::SameLine(); if (ImGui::Button("+100k")) game::AddCash(100000.f);
    Check("Never Below", c.lockCash);
    ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
    SliderF("##lockcash", c.lockCashValue, 1000.f, 1000000.f, "$%.0f");

    Section("SCORE & NIGHT");
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("##score", &g_scoreInput, 1000, 100000);
    ImGui::SameLine(); if (ImGui::Button("Set Score")) game::SetScore(g_scoreInput);
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("##night", &g_nightInput, 1, 5);
    ImGui::SameLine(); if (ImGui::Button("Set Night")) game::SetNight(g_nightInput);
    ImGui::TextDisabled("Night can be changed during the day (shop) phase.");
    if (s.valid)
        ImGui::TextDisabled("cash $%.0f   score %d   night %d", s.cash, s.score, s.night);

    Section("CONFIG");
    if (ImGui::Button("Save Settings")) g_cfgMsg = game::SaveConfig();
    ImGui::SameLine();
    if (ImGui::Button("Load Settings")) g_cfgMsg = game::LoadConfig();
    if (!g_cfgMsg.empty()) ImGui::TextWrapped("%s", g_cfgMsg.c_str());
    ImGui::TextDisabled("Settings auto-load on attach.  Ctrl+End also quits.");
}

void DrawFooter(const game::Snapshot& s) {
    ImGui::Separator();
    if (!game::Attached()) { ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1), "Start Yet Another Zombie Defense HD."); return; }
    const char* m = "main menu";
    switch (s.mode) {
    case game::Mode::Day: m = "day (shop)"; break;
    case game::Mode::Night: m = "night"; break;
    case game::Mode::GameOver: m = "game over"; break;
    default: break;
    }
    if (s.online) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f), "ONLINE (test mode, logging)  |  %s", m);
    else ImGui::TextColored(ImVec4(0.45f, 0.46f, 0.49f, 1.f), "attached  |  %s  |  enemies %d", m, (int)s.enemies.size());
}

void DrawExitButton() {
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 60);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.10f, 0.10f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.15f, 0.15f, 1.f));
    if (ImGui::Button("EXIT", ImVec2(60, 0))) g_quit = true;
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restore everything and close the trainer");
}

void DrawMenu(const game::Snapshot& s) {
    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::Begin("##trainer", &g_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    DrawBanner();
    DrawTabBar();
    ImGui::Spacing();
    ImGui::BeginChild("##content", ImVec2(0, -34), ImGuiChildFlags_Borders);
    // Online lobby: everything that writes game memory is locked; ESP is read-only.
    const bool locked = false;   // online lock off for manual online testing
    if (locked) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.6f, 0.3f, 1.f));
        ImGui::TextWrapped("Online game detected. Features that change the game are disabled here "
                           "(they desync or crash online sessions). ESP still works.");
        ImGui::PopStyleColor();
        ImGui::BeginDisabled();
    }
    switch (g_tab) {
    case 0: TabPlayer(s); break;
    case 1: TabWeapons(s); break;
    case 2: TabAim(s); break;
    case 3: TabEnemies(s); break;
    case 4: TabDefense(s); break;
    case 5: TabVisuals(); break;
    case 6: TabGame(s); break;
    }
    if (locked) ImGui::EndDisabled();
    ImGui::EndChild();
    DrawFooter(s);
    DrawExitButton();
    ImGui::End();
}

// ---------------------------------------------------------------------------
//  ESP
// ---------------------------------------------------------------------------
bool Project(const game::Snapshot& s, float x, float y, float z, ImVec2& out) {
    const float* M = s.viewProj;                 // column-major
    float cx = M[0] * x + M[4] * y + M[8] * z + M[12];
    float cy = M[1] * x + M[5] * y + M[9] * z + M[13];
    float cw = M[3] * x + M[7] * y + M[11] * z + M[15];
    if (cw < 0.01f) return false;
    float nx = cx / cw, ny = cy / cw;
    out.x = s.gameX - g_mon.left + (nx * 0.5f + 0.5f) * s.gameW;
    out.y = s.gameY - g_mon.top + (0.5f - ny * 0.5f) * s.gameH;
    return true;
}

float EnemyHeight(int type) {
    switch (type) {
    case 3: return 1.1f;    // Cerberus (dog)
    case 6: return 2.8f;    // Demon Lord
    case 7: return 2.4f;    // Reaper
    default: return 1.8f;
    }
}

void DrawEsp(const game::Snapshot& s) {
    auto& c = game::Cfg();
    if (!c.esp || !s.matrixOk || s.gameW <= 0) return;
    if (s.mode != game::Mode::Night && s.mode != game::Mode::Day) return;
    HWND fg = GetForegroundWindow();
    if (IsIconic(GetAncestor(fg, GA_ROOT)) ) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = 13.f, lh = 13.f;
    game::Vec3 me{};
    for (auto& p : s.players) if (p.local) me = p.pos;
    ImVec2 origin(s.gameX - g_mon.left + s.gameW * 0.5f, s.gameY - g_mon.top + (float)s.gameH);
    float t = (float)ImGui::GetTime();

    auto draw = [&](const game::Entity& e, ImU32 col, float height) {
        float dx = e.pos.x - me.x, dz = e.pos.z - me.z;
        float dist = std::sqrt(dx * dx + dz * dz);
        if (!e.local && dist > c.espMaxDist) return;
        ImVec2 feet, head;
        if (!Project(s, e.pos.x, e.pos.y, e.pos.z, feet) || !Project(s, e.pos.x, e.pos.y + height, e.pos.z, head)) return;
        float h = std::fabs(feet.y - head.y);
        if (h < 4.f) h = 4.f;
        float w = h * 0.55f;
        float top = (std::min)(feet.y, head.y);
        ImVec2 a(feet.x - w * 0.5f, top), b(feet.x + w * 0.5f, top + h);
        if (b.x < 0 || a.x > (g_mon.right - g_mon.left) || b.y < 0 || a.y > (g_mon.bottom - g_mon.top)) return;

        if (c.espLines) dl->AddLine(origin, ImVec2(feet.x, top + h * 0.5f), (col & 0x00FFFFFF) | 0x66000000, 1.2f);
        dl->AddRect(a, b, col, 0.f, 0, 1.4f);

        if (c.espHealth && e.maxHp > 0) {
            float f = (std::max)(0.f, (std::min)(1.f, e.hp / e.maxHp));
            ImVec2 ba(a.x - 5, a.y), bb(a.x - 2, b.y);
            dl->AddRectFilled(ba, bb, 0xC0000000);
            ImU32 hc = ImGui::GetColorU32(ImVec4(1.f - f, f, 0.15f, 1.f));
            dl->AddRectFilled(ImVec2(ba.x, bb.y - (bb.y - ba.y) * f), bb, hc);
        }
        if (c.espInfo) {
            char l1[64], l2[48];
            snprintf(l1, sizeof(l1), "%s", e.name.c_str());
            if (e.local) l2[0] = 0;
            else snprintf(l2, sizeof(l2), "%.0f%%  %.0fm", e.maxHp > 0 ? e.hp / e.maxHp * 100.f : 0.f, dist);
            const char* lines[2] = { l1, l2 };
            int n = l2[0] ? 2 : 1;
            float ty0 = a.y - 3 - n * lh;
            for (int i = 0; i < n; ++i) {
                ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0, lines[i]);
                ImVec2 tp(feet.x - sz.x * 0.5f, ty0 + i * lh);
                dl->AddText(font, fs, ImVec2(tp.x + 1, tp.y + 1), 0xE0000000, lines[i]);
                dl->AddText(font, fs, tp, i == 0 ? col : 0xFFCFCFCF, lines[i]);
            }
        }
    };

    if (c.espEnemies) {
        for (auto& e : s.enemies) {
            ImU32 col = IM_COL32(255, 77, 77, 255);
            if (e.type == 6 || e.type == 7) {    // bosses: animated rainbow
                float hue = std::fmod(t * 0.35f + e.pos.x * 0.03f, 1.f);
                float r, g, b; ImGui::ColorConvertHSVtoRGB(hue, 0.8f, 1.f, r, g, b);
                col = ImGui::GetColorU32(ImVec4(r, g, b, 1.f));
            } else if (e.frozen) col = IM_COL32(90, 200, 255, 255);
            draw(e, col, EnemyHeight(e.type));
        }
    }
    if (c.espDrops) {
        static const char* kPw[] = { "Speed Boost", "Invulnerability", "Magnet", "Fast Reload", "Invisibility" };
        for (auto& d : s.drops) {
            float dx = d.pos.x - me.x, dz = d.pos.z - me.z;
            float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > c.espMaxDist) continue;
            ImVec2 sp;
            if (!Project(s, d.pos.x, d.pos.y + 0.3f, d.pos.z, sp)) continue;
            ImU32 col; const char* name;
            switch (d.type) {
            case 0:  col = IM_COL32(255, 215, 60, 255);  name = "Cash"; break;
            case 1:  col = IM_COL32(255, 150, 40, 255);  name = "Ammo"; break;
            case 2:  col = IM_COL32(70, 230, 110, 255);  name = "Health"; break;
            default: col = IM_COL32(190, 110, 255, 255); name = (d.sub >= 0 && d.sub < 5) ? kPw[d.sub] : "Powerup"; break;
            }
            // blink during the last 3 seconds before it despawns
            bool fading = d.timeLeft < 3.f && std::fmod(t * 4.f, 1.f) < 0.5f;
            ImU32 fill = fading ? (col & 0x00FFFFFF) | 0x50000000 : (col & 0x00FFFFFF) | 0xC0000000;
            const float r = 5.f;
            ImVec2 pts[4] = { {sp.x, sp.y - r}, {sp.x + r, sp.y}, {sp.x, sp.y + r}, {sp.x - r, sp.y} };
            dl->AddConvexPolyFilled(pts, 4, fill);
            dl->AddPolyline(pts, 4, 0xE0000000, ImDrawFlags_Closed, 1.f);
            if (c.espLines) dl->AddLine(origin, sp, (col & 0x00FFFFFF) | 0x40000000, 1.f);
            char lbl[48];
            if (c.espDropTimer) snprintf(lbl, sizeof(lbl), "%s  %.0fs", name, d.timeLeft);
            else snprintf(lbl, sizeof(lbl), "%s", name);
            ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0, lbl);
            ImVec2 tp(sp.x - sz.x * 0.5f, sp.y - r - 2 - lh);
            dl->AddText(font, fs, ImVec2(tp.x + 1, tp.y + 1), 0xE0000000, lbl);
            dl->AddText(font, fs, tp, col, lbl);
        }
    }
    for (auto& p : s.players) {
        if (p.local && !c.espLocal) continue;
        if (!p.local && !c.espPlayers) continue;
        draw(p, p.local ? IM_COL32(60, 255, 120, 255) : IM_COL32(255, 150, 40, 255), 1.8f);
    }
}

// ---------------------------------------------------------------------------
//  Aimbot / triggerbot (input side, offline night only)
//
//  The game turns the player toward the point where the mouse ray meets the
//  plane y = 1 (Player crosshair code), blending rotation every frame. So the
//  aimbot moves the real cursor onto the enemy's y = 1 point and the game does
//  the turning itself; nothing fights the game's own aim code. The triggerbot
//  holds the left button (the game fires while it is down) when the line the
//  player faces passes within trigWidth of an enemy.
// ---------------------------------------------------------------------------
bool g_trigDown = false;

void SendLeft(bool down) {
    log_::Info("[test] triggerbot %s", down ? "fire" : "release");
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
    g_trigDown = down;
}

bool AimKeyHeld(int k) {
    static const int vk[] = { 0, VK_RBUTTON, VK_SHIFT, VK_XBUTTON1, VK_XBUTTON2, VK_MENU };
    if (k <= 0 || k >= (int)IM_ARRAYSIZE(vk)) return true;
    return (GetAsyncKeyState(vk[k]) & 0x8000) != 0;
}

void AimAssist(const game::Snapshot& s) {
    auto& c = game::Cfg();
    HWND gw = mem::GameWindow();
    bool active = !g_open && s.havePlayer && s.mode == game::Mode::Night && s.matrixOk &&
                  gw && GetAncestor(GetForegroundWindow(), GA_ROOT) == gw;
    if (!active || !c.triggerbot) { if (g_trigDown) SendLeft(false); }
    if (!active) return;

    game::Vec3 me{};
    for (auto& p : s.players) if (p.local) me = p.pos;
    float range = c.aimRange;

    if (c.aimbot) {
        POINT cur; GetCursorPos(&cur);
        float fov = c.aimFov, best = fov;
        ImVec2 tgt{};
        bool found = false;
        for (auto& e : s.enemies) {
            float dx = e.pos.x - me.x, dz = e.pos.z - me.z;
            if (dx * dx + dz * dz > range * range || e.hp <= 0.f) continue;
            ImVec2 sp;
            if (!Project(s, e.pos.x, 1.f, e.pos.z, sp)) continue;
            sp.x += g_mon.left; sp.y += g_mon.top;          // overlay -> screen
            float d = std::hypot(sp.x - cur.x, sp.y - cur.y);
            if (d < best) { best = d; tgt = sp; found = true; }
        }
        if (c.aimDrawFov) {
            ImGui::GetBackgroundDrawList()->AddCircle(ImVec2((float)(cur.x - g_mon.left), (float)(cur.y - g_mon.top)),
                fov, found ? IM_COL32(255, 77, 77, 140) : IM_COL32(255, 255, 255, 60), 64, 1.2f);
        }
        if (found && AimKeyHeld(c.aimKey)) {
            float sm = (std::max)(1.f, c.aimSmooth.load());
            float mx = (tgt.x - cur.x) / sm, my = (tgt.y - cur.y) / sm;
            if (std::fabs(mx) < 1.f && std::fabs(tgt.x - cur.x) >= 1.f) mx = mx < 0 ? -1.f : 1.f;
            if (std::fabs(my) < 1.f && std::fabs(tgt.y - cur.y) >= 1.f) my = my < 0 ? -1.f : 1.f;
            SetCursorPos(cur.x + (int)std::lround(mx), cur.y + (int)std::lround(my));
        }
    }

    if (c.triggerbot) {
        float fx = std::cos(s.rot), fz = std::sin(s.rot), w = c.trigWidth;
        bool onTarget = false;
        for (auto& e : s.enemies) {
            float dx = e.pos.x - me.x, dz = e.pos.z - me.z;
            float along = dx * fx + dz * fz;                    // distance ahead
            if (along <= 0.f || along > range || e.hp <= 0.f) continue;
            float side = std::fabs(dx * fz - dz * fx);          // distance off the aim line
            if (side <= w) { onTarget = true; break; }
        }
        // don't release a button the user is holding themselves
        if (onTarget && !g_trigDown && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) SendLeft(true);
        else if (!onTarget && g_trigDown) SendLeft(false);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc >= 3 && wcscmp(argv[1], L"--selftest") == 0) {
            int rc = game::SelfTest(argv[2]);
            LocalFree(argv);
            return rc;
        }
        LocalFree(argv);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        log_::Init((std::filesystem::path(buf).parent_path() / L"trainer.log").c_str());
        log_::Info("==================== YAZD HD trainer  build " __DATE__ " " __TIME__);
    }
    if (!CreateOverlay()) { log_::Error("overlay creation failed"); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ApplyStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    SetInteractive(true);
    game::StartWorker();

    bool prevIns = false, running = true;
    MSG msg;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (ins && !prevIns) {
            g_open = !g_open;
            if (!g_open) if (HWND gw = FindWindowW(nullptr, L"Yet Another Zombie Defense HD")) SetForegroundWindow(gw);
        }
        prevIns = ins;
        if (g_quit || ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_END) & 0x8000))) break;

        game::Snapshot s = game::GetSnapshot();
        FollowGameMonitor(s);
        SetInteractive(g_open);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawEsp(s);
        AimAssist(s);
        if (g_open) DrawMenu(s);
        ImGui::Render();
        const float clear[4] = { 0, 0, 0, 0 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    if (g_trigDown) SendLeft(false);
    game::StopWorker();       // restores everything we changed
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(g_hwnd);
    log_::Info("clean exit");
    log_::Shutdown();
    return 0;
}
