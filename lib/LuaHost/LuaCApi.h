#ifndef LUACAPI_BRIDGE_H
#define LUACAPI_BRIDGE_H
// Lua 5.5 stock luaconf.h defines `#define LUA_API extern` unconditionally on
// GCC builds (the __declspec branches only cover Windows). Including lua.h /
// lualib.h / lauxlib.h directly from C++ therefore mangles every lua_* symbol
// and the linker cannot resolve them against the C-compiled Lua sources.
//
// This shim: when compiling as C++, enter an extern "C" block, delete the
// `extern` keyword from LUA_API's expansion context by pre-defining
// LUA_API_BARE and using X-macros? Simously simpler: include-with-C-linkage
// pattern that survives luaconf.h's unconditional #define: undef LUA_API
// BEFORE luaconf.h is parsed will not survive. Instead luaconf.h registers
// its define *after* our wrapped block; so use the official escape hatch:
// __improper bridge-- include the C headers through an auxiliary trick:
//     extern "C" {
//       #include "lua.h" ...
//     }
// That fails under MSVC "nvcc" style only; GCC accepts it (nested extern "C"
// around stock luaconf.h's plain `extern` yields correct C linkage symbols).
#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#ifdef __cplusplus
}
#endif

#endif // LUACAPI_BRIDGE_H
