#include "mono.h"
#include "mem.h"
#include <Windows.h>
#include <cstring>
#include <map>
#include <mutex>

namespace mono {
namespace {

// Remote stub. RCX = ctx:
//   +00 fn  +08 a1  +10 a2  +18 a3  +20 a4  +28 ret
//   +30 attachFn  +38 attachArg  +40 detachFn  +48 (scratch: MonoThread*)
// Calls attachFn(attachArg), fn(a1..a4) -> ret, detachFn(thread).
const uint8_t kStub[] = {
    0x53,                               // push rbx
    0x48, 0x83, 0xEC, 0x30,             // sub  rsp, 30h
    0x48, 0x89, 0xCB,                   // mov  rbx, rcx
    0x48, 0x8B, 0x4B, 0x38,             // mov  rcx, [rbx+38h]
    0xFF, 0x53, 0x30,                   // call [rbx+30h]
    0x48, 0x89, 0x43, 0x48,             // mov  [rbx+48h], rax
    0x48, 0x8B, 0x4B, 0x08,             // mov  rcx, [rbx+08h]
    0x48, 0x8B, 0x53, 0x10,             // mov  rdx, [rbx+10h]
    0x4C, 0x8B, 0x43, 0x18,             // mov  r8,  [rbx+18h]
    0x4C, 0x8B, 0x4B, 0x20,             // mov  r9,  [rbx+20h]
    0xFF, 0x13,                         // call [rbx]
    0x48, 0x89, 0x43, 0x28,             // mov  [rbx+28h], rax
    0x48, 0x8B, 0x4B, 0x48,             // mov  rcx, [rbx+48h]
    0xFF, 0x53, 0x40,                   // call [rbx+40h]
    0x48, 0x83, 0xC4, 0x30,             // add  rsp, 30h
    0x5B,                               // pop  rbx
    0x31, 0xC0,                         // xor  eax, eax
    0xC3,                               // ret
};

constexpr uint64_t kPage    = 0x4000;
constexpr uint64_t kCtx     = 0x100;
constexpr uint64_t kIter    = 0x180;
constexpr uint64_t kStrings = 0x200;

std::mutex  g_mx;
uint64_t    g_page = 0, g_domain = 0;
std::string g_err;
std::map<std::string, uint64_t> g_fn;

const char* kExports[] = {
    "mono_get_root_domain", "mono_thread_attach", "mono_thread_detach",
    "mono_image_loaded", "mono_class_from_name", "mono_class_get_field_from_name",
    "mono_field_get_offset", "mono_class_vtable", "mono_vtable_get_static_field_data",
    "mono_class_get_nested_types", "mono_class_get_name", "mono_class_get_parent",
};

bool ResolveExports(uint64_t base) {
    IMAGE_DOS_HEADER dos = mem::Read<IMAGE_DOS_HEADER>(base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS64 nt = mem::Read<IMAGE_NT_HEADERS64>(base + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE) return false;
    auto dd = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY ed = mem::Read<IMAGE_EXPORT_DIRECTORY>(base + dd.VirtualAddress);
    for (DWORD i = 0; i < ed.NumberOfNames; ++i) {
        DWORD nameRva = mem::Read<DWORD>(base + ed.AddressOfNames + i * 4);
        char name[64] = {};
        mem::ReadRaw(base + nameRva, name, sizeof(name) - 1);
        for (const char* want : kExports) {
            if (strcmp(name, want) != 0) continue;
            WORD ord = mem::Read<WORD>(base + ed.AddressOfNameOrdinals + i * 2);
            DWORD rva = mem::Read<DWORD>(base + ed.AddressOfFunctions + ord * 4);
            g_fn[want] = base + rva;
        }
    }
    for (const char* want : kExports)
        if (!g_fn.count(want)) { g_err = std::string("missing export ") + want; return false; }
    return true;
}

// Run fn(a1..a4) on a fresh remote thread; returns RAX.
uint64_t Call(const char* fn, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0) {
    uint64_t ctx[10] = {};
    ctx[0] = g_fn[fn]; ctx[1] = a1; ctx[2] = a2; ctx[3] = a3; ctx[4] = a4;
    if (g_domain) {
        ctx[6] = g_fn["mono_thread_attach"]; ctx[7] = g_domain; ctx[8] = g_fn["mono_thread_detach"];
    } else {   // bootstrap: no domain yet, so "attach"/"detach" are harmless getters
        ctx[6] = g_fn["mono_get_root_domain"]; ctx[8] = g_fn["mono_get_root_domain"];
    }
    if (!mem::WriteRaw(g_page + kCtx, ctx, sizeof(ctx))) return 0;
    HANDLE t = CreateRemoteThread(mem::Handle(), nullptr, 0,
        (LPTHREAD_START_ROUTINE)g_page, (LPVOID)(g_page + kCtx), 0, nullptr);
    if (!t) { g_err = "CreateRemoteThread failed"; return 0; }
    DWORD w = WaitForSingleObject(t, 10000);
    CloseHandle(t);
    if (w != WAIT_OBJECT_0) { g_err = "remote call timed out"; return 0; }
    return mem::Read<uint64_t>(g_page + kCtx + 0x28);
}

uint64_t Str(const char* s) {               // one string slot is enough per call site
    static uint64_t cursor = 0;
    size_t n = strlen(s) + 1;
    if (cursor == 0 || cursor + n > g_page + kPage) cursor = g_page + kStrings;
    uint64_t p = cursor;
    mem::WriteRaw(p, s, n);
    cursor += (n + 7) & ~7ull;
    return p;
}

} // namespace

bool Init() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_fn.clear(); g_domain = 0; g_err.clear();
    uint64_t base = mem::ModuleBase(L"mono-2.0-bdwgc.dll");
    if (!base) { g_err = "mono runtime not loaded yet"; return false; }
    if (!ResolveExports(base)) return false;
    g_page = (uint64_t)VirtualAllocEx(mem::Handle(), nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_page) { g_err = "VirtualAllocEx failed"; return false; }
    mem::WriteRaw(g_page, kStub, sizeof(kStub));
    g_domain = Call("mono_get_root_domain");
    if (!g_domain) { g_err = "no root domain"; Shutdown(); return false; }
    return true;
}

void Shutdown() {
    if (g_page && mem::Handle()) VirtualFreeEx(mem::Handle(), (LPVOID)g_page, 0, MEM_RELEASE);
    g_page = 0; g_domain = 0;
}

bool Ready() { return g_page && g_domain; }

uint64_t Image(const char* assembly) {
    std::lock_guard<std::mutex> lk(g_mx);
    return Call("mono_image_loaded", Str(assembly));
}

uint64_t Class(uint64_t image, const char* ns, const char* name) {
    std::lock_guard<std::mutex> lk(g_mx);
    uint64_t k = Call("mono_class_from_name", image, Str(ns), Str(name));
    if (!k) g_err = std::string("class not found: ") + name;
    return k;
}

uint64_t NestedClass(uint64_t outer, const char* name) {
    std::lock_guard<std::mutex> lk(g_mx);
    mem::Write<uint64_t>(g_page + kIter, 0);
    for (int i = 0; i < 256; ++i) {
        uint64_t k = Call("mono_class_get_nested_types", outer, g_page + kIter);
        if (!k) break;
        uint64_t np = Call("mono_class_get_name", k);
        char buf[64] = {};
        mem::ReadRaw(np, buf, sizeof(buf) - 1);
        if (strcmp(buf, name) == 0) return k;
    }
    g_err = std::string("nested class not found: ") + name;
    return 0;
}

uint64_t Parent(uint64_t klass) {
    std::lock_guard<std::mutex> lk(g_mx);
    return Call("mono_class_get_parent", klass);
}

int32_t FieldOffset(uint64_t klass, const char* field) {
    std::lock_guard<std::mutex> lk(g_mx);
    uint64_t f = Call("mono_class_get_field_from_name", klass, Str(field));
    if (!f) { g_err = std::string("field not found: ") + field; return -1; }
    return (int32_t)(uint32_t)Call("mono_field_get_offset", f);
}

uint64_t StaticAddr(uint64_t klass, const char* field) {
    std::lock_guard<std::mutex> lk(g_mx);
    uint64_t f = Call("mono_class_get_field_from_name", klass, Str(field));
    if (!f) { g_err = std::string("static not found: ") + field; return 0; }
    uint32_t off = (uint32_t)Call("mono_field_get_offset", f);
    uint64_t vt = Call("mono_class_vtable", g_domain, klass);
    if (!vt) { g_err = "no vtable"; return 0; }
    uint64_t data = Call("mono_vtable_get_static_field_data", vt);
    return data ? data + off : 0;
}

const std::string& LastError() { return g_err; }

} // namespace mono
