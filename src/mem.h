// =============================================================================
//  mem.h — external process access (ReadProcessMemory / WriteProcessMemory).
// =============================================================================
#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace mem {

bool     Open(const wchar_t* exeName);   // attach to the first matching process
void     Close();
bool     Alive();                        // process still running
HANDLE   Handle();
DWORD    Pid();
uint64_t ModuleBase(const wchar_t* name, std::wstring* path = nullptr);
HWND     GameWindow();                   // main visible window of the target

bool ReadRaw(uint64_t addr, void* out, size_t n);
bool WriteRaw(uint64_t addr, const void* in, size_t n);
void WriteCounts(unsigned& ok, unsigned& failed);   // since last call (test log)

template <class T> T Read(uint64_t addr, T def = T{}) {
    T v{};
    return ReadRaw(addr, &v, sizeof(T)) ? v : def;
}
template <class T> bool Write(uint64_t addr, const T& v) { return WriteRaw(addr, &v, sizeof(T)); }

// Plausible user-mode heap pointer (filters garbage before following it).
inline bool Ptr(uint64_t p) { return p >= 0x10000 && p < 0x7FFFFFFF0000ull && (p & 7) == 0; }

} // namespace mem
