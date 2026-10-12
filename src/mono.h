// =============================================================================
//  mono.h — name-based resolver for the game's Mono runtime.
//
//  At attach time a tiny x64 stub is written into the game and run on a short-
//  lived remote thread to call the public mono embedding API
//  (mono_class_from_name, mono_field_get_offset, ...). Each call attaches the
//  thread to the root domain and detaches it again. Results are plain numbers
//  (field offsets, static-data addresses, class pointers); everything after
//  that is ordinary ReadProcessMemory / WriteProcessMemory.
// =============================================================================
#pragma once
#include <cstdint>
#include <string>

namespace mono {

bool     Init();                 // find mono exports, allocate stub, get root domain
void     Shutdown();             // free the remote page
bool     Ready();

uint64_t Image(const char* assembly);                       // MonoImage*
uint64_t Class(uint64_t image, const char* ns, const char* name);
uint64_t NestedClass(uint64_t outer, const char* name);
uint64_t Parent(uint64_t klass);
// Instance field offset (includes the 0x10 object header); -1 when missing.
int32_t  FieldOffset(uint64_t klass, const char* field);
// Absolute address of a static field's storage; 0 when missing.
uint64_t StaticAddr(uint64_t klass, const char* field);

const std::string& LastError();

} // namespace mono
