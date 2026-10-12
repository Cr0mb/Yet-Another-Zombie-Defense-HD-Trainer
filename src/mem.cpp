#include "mem.h"
#include <atomic>
#include <TlHelp32.h>
#include <Psapi.h>

namespace mem {
namespace {
HANDLE g_h   = nullptr;
DWORD  g_pid = 0;
}

bool Open(const wchar_t* exeName) {
    Close();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName) == 0) { g_pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    if (!g_pid) return false;
    g_h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                      PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, g_pid);
    if (!g_h) { g_pid = 0; return false; }
    return true;
}

void Close() {
    if (g_h) CloseHandle(g_h);
    g_h = nullptr; g_pid = 0;
}

bool Alive() {
    return g_h && WaitForSingleObject(g_h, 0) == WAIT_TIMEOUT;
}

HANDLE Handle() { return g_h; }
DWORD  Pid()    { return g_pid; }

uint64_t ModuleBase(const wchar_t* name, std::wstring* path) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{ sizeof(me) };
    uint64_t base = 0;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsicmp(me.szModule, name) == 0) {
            base = (uint64_t)me.modBaseAddr;
            if (path) *path = me.szExePath;
            break;
        }
    }
    CloseHandle(snap);
    return base;
}

namespace {
struct FindWnd { DWORD pid; HWND best; };
BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* f = (FindWnd*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->best = h; return FALSE; }
    return TRUE;
}
}

HWND GameWindow() {
    if (!g_pid) return nullptr;
    FindWnd f{ g_pid, nullptr };
    EnumWindows(EnumProc, (LPARAM)&f);
    return f.best;
}

bool ReadRaw(uint64_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return g_h && addr && ReadProcessMemory(g_h, (LPCVOID)addr, out, n, &got) && got == n;
}

static std::atomic<unsigned> g_wOk{0}, g_wFail{0};

bool WriteRaw(uint64_t addr, const void* in, size_t n) {
    SIZE_T put = 0;
    bool ok = g_h && addr && WriteProcessMemory(g_h, (LPVOID)addr, in, n, &put) && put == n;
    (ok ? g_wOk : g_wFail)++;
    return ok;
}

void WriteCounts(unsigned& ok, unsigned& failed) { ok = g_wOk.exchange(0); failed = g_wFail.exchange(0); }

} // namespace mem
