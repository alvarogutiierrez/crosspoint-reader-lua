#include "LuaCApi.h"
#include "ScriptHost.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "HalStorage.h"

#include <NetworkClient.h>

#include <BatteryMonitor.h>
#include <HalClock.h>
#include <HalPowerManager.h>

// ---------------------------------------------------------------------------
// Embedded Lua 5.5.1 app host implementation.
//
// Runtime contract:
//   * The script lives at /apps/<AppName>/main.lua (fallback index.lua) and
//     defines optional global lifecycle callbacks: on_load(), on_enter(),
//     on_input(ev), on_draw(), on_tick(dt_ms), on_leave(), on_unload().
//   * `g` is a global drawing table that queues frame ops (never touches the
//     renderer from the loop task; LuaGfx::apply runs under RenderLock).
//   * `ctx` exposes screen size, tick rate, invalidate, and a per-app
//     persistent state table (ctx.state <-> /apps/<App>/state.json).
// ---------------------------------------------------------------------------

namespace {

constexpr size_t MAX_SCRIPT_BYTES = 192 * 1024;
constexpr size_t MAX_REQUIRE_BYTES = 96 * 1024;
constexpr size_t MAX_STATE_SERIALIZED = 12 * 1024;
constexpr uint32_t TICK_MIN_MS = 33;  // doc: "realtime" = 33 ms

ScriptHost* hostOf(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "x4_host");
  ScriptHost* h = static_cast<ScriptHost*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return h;
}

struct ReadCtx {
  HalFile file;
};

const char* storageReader(lua_State*, void* data, size_t* size) {
  static char buf[512];
  ReadCtx* ctx = static_cast<ReadCtx*>(data);
  if (!ctx->file.isOpen()) {
    *size = 0;
    return nullptr;
  }
  int n = ctx->file.read(buf, sizeof(buf));
  if (n <= 0) {
    *size = 0;
    return nullptr;
  }
  *size = static_cast<size_t>(n);
  return buf;
}

void* luaAlloc(void*, void* ptr, size_t, size_t nsize) {
  if (nsize == 0) {
    free(ptr);
    return nullptr;
  }
  return realloc(ptr, nsize);
}

int panicHandler(lua_State* L) {
  const char* msg = lua_tostring(L, -1);
  LOG_ERR("LUAHOST", " PANIC: %s", msg ? msg : "(no message)");
  return 0;
}

// --- g bindings (graphics draw queue) ---------------------------------------

// Calls may be dot-style g.rect(...) or colon-style g:rect(...): when arg 1 is
// the g table itself, numeric args start at index 2.
static int gfxArgBase(lua_State* L) {
  return lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
}

// Layer handles are userdata carrying a LuaGfx layer id (see ctx.layers).
int layerIdFromArg(lua_State* L, int idx);
void pushReadOnly(lua_State* L, int nargs);

// XTApp color model: gray level 0..15. The device draws 0..7 as WHITE and
// 8..15 as BLACK (its 4-level hardware gray is not exposed through this API),
// so ink is "level >= 8" — NOT "level != 0": an app passing 1..7 still asks for
// paper. Default when omitted is 15.
static bool gfxInk(float level) { return level >= 8.0f; }

// Reads a color level from an optional argument (number or {color=} table).
static float gfxColorArg(lua_State* L, int idx, float fallback) {
  if (lua_istable(L, idx)) {
    lua_getfield(L, idx, "color");
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
  }
  if (lua_isnumber(L, idx)) return static_cast<float>(lua_tonumber(L, idx));
  return fallback;
}

// {invert = true} flips an image's ink/paper polarity.
static bool gfxFlagArg(lua_State* L, int idx, const char* key) {
  if (!lua_istable(L, idx)) return false;
  lua_getfield(L, idx, key);
  const bool v = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return v;
}

// g:image / ctx.assets:handle accept a plain key or a handle table.
static const char* gfxImageKey(lua_State* L, int idx) {
  if (lua_istable(L, idx)) {
    lua_getfield(L, idx, "asset_key");
    if (!lua_isstring(L, -1)) {
      lua_pop(L, 1);
      // ctx.package:image object (2.x): the table carries its asset key.
      lua_getfield(L, idx, "key");
    }
    if (!lua_isstring(L, -1)) {
      lua_pop(L, 1);
      lua_getfield(L, idx, "handle");
    }
    const char* k = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
    return k;  // caller keeps the table on the stack until it is done
  }
  return lua_isstring(L, idx) ? lua_tostring(L, idx) : nullptr;
}

// --- 2.x options tables (contract api-2.1.3, sections 6-7) ------------------
//
// 2.x passes ONE OPTIONS TABLE where 0.8 passed positional colour/mode
// arguments. Both shapes are accepted by the same binding: when the slot 0.8
// uses for the mode/colour holds a table, the 2.x rules apply. Nothing here
// forks the API on the manifest version — an app gets the semantics of the
// shape it actually wrote.
struct GfxStyle {
  bool fill = false;
  int radius = 0;
  int lineWidth = 1;
  float color = 15.0f;
};

static GfxStyle gfxStyleTable(lua_State* L, int idx) {
  GfxStyle o;
  if (!lua_istable(L, idx)) return o;
  lua_getfield(L, idx, "fill");
  if (lua_isboolean(L, -1)) o.fill = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  lua_getfield(L, idx, "radius");
  if (lua_isnumber(L, -1)) o.radius = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, idx, "line_width");
  if (lua_isnumber(L, -1)) o.lineWidth = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, idx, "color");
  if (lua_isnumber(L, -1)) o.color = static_cast<float>(lua_tonumber(L, -1));
  lua_pop(L, 1);
  if (o.radius < 0) o.radius = 0;  // contract: radius 0..255
  if (o.radius > 255) o.radius = 255;
  if (o.lineWidth < 1) o.lineWidth = 1;  // contract: line_width 1..255
  if (o.lineWidth > 255) o.lineWidth = 255;
  return o;
}

// opts.font: a ctx.fonts entry (table carrying `id`) or a raw font id. 0 means
// "no font requested" and the caller keeps its default.
static int gfxFontField(lua_State* L, int idx) {
  if (!lua_istable(L, idx)) return 0;
  lua_getfield(L, idx, "font");
  int id = 0;
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "id");
    if (lua_isnumber(L, -1)) id = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
  } else if (lua_isnumber(L, -1)) {
    id = static_cast<int>(lua_tointeger(L, -1));
  }
  lua_pop(L, 1);
  return id;
}

// A text-option string field ("center", "middle", ...). The returned pointer is
// owned by the table at `idx`, which the caller keeps on the stack.
static const char* gfxStrField(lua_State* L, int idx, const char* key) {
  if (!lua_istable(L, idx)) return nullptr;
  lua_getfield(L, idx, key);
  const char* v = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
  return v;
}

int g_clear(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  host->gfx.addClear(gfxInk(gfxColorArg(L, base, 15.0f)));
  return 0;
}

int g_text(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // 0.8: g:text(x, y, str[, color])
  // 2.x: g:text(x, y, str, {color, align, valign, font}) — (x, y) anchors the
  // LINE BOX (measure wide, line_height tall, baseline `ascent` below its top),
  // so align/valign shift the anchor before queueing. The contract aligns the
  // box, not the ink, which is why two different strings in one font share a
  // baseline.
  int x = static_cast<int>(luaL_checkinteger(L, base));
  int y = static_cast<int>(luaL_checkinteger(L, base + 1));
  const char* s = luaL_checkstring(L, base + 2);
  int fontId = host->defaultFontId;
  float color = 15.0f;  // default: black
  if (lua_istable(L, base + 3)) {
    color = gfxColorArg(L, base + 3, 15.0f);
    const int optFont = gfxFontField(L, base + 3);
    if (optFont != 0) fontId = optFont;
    const char* align = gfxStrField(L, base + 3, "align");
    if (align != nullptr && (strcmp(align, "center") == 0 || strcmp(align, "right") == 0)) {
      const int tw = host->textWidth(fontId, s);
      if (strcmp(align, "center") == 0) {
        x -= tw / 2;
      } else {
        x -= tw;
      }
    }
    // "justify" is a multi-line alignment; the contract has g:text treat it as left.
    const char* valign = gfxStrField(L, base + 3, "valign");
    if (valign != nullptr) {
      if (strcmp(valign, "middle") == 0) {
        y -= host->lineHeightOf(fontId) / 2;
      } else if (strcmp(valign, "baseline") == 0) {
        y -= host->ascenderOf(fontId);
      } else if (strcmp(valign, "bottom") == 0) {
        y -= host->lineHeightOf(fontId);
      }
    }
  } else {
    color = gfxColorArg(L, base + 3, 15.0f);
  }
  // Text queueing needs a font that exists in the renderer's font map; without
  // one drawText() logs "Font N not found" and draws nothing.
  if (fontId == 0) {
    LOG_ERR("LUAHOST", "g:text called with no default font set; text dropped");
    return 0;
  }
  host->gfx.addText(fontId, x, y, s, gfxInk(color));
  return 0;
}

int g_rect(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:rect(x, y, w, h[, "fill"|"stroke"][, color])
  int x = static_cast<int>(luaL_checkinteger(L, base));
  int y = static_cast<int>(luaL_checkinteger(L, base + 1));
  int w = static_cast<int>(luaL_checkinteger(L, base + 2));
  int h = static_cast<int>(luaL_checkinteger(L, base + 3));
  // 2.x: {fill, radius, line_width, color}. The stroke grows inward and the
  // radius clamps to half the short side (handled in the rasterizer).
  if (lua_istable(L, base + 4)) {
    const GfxStyle o = gfxStyleTable(L, base + 4);
    host->gfx.addRect(o.fill ? LuaGfx::Kind::FillRect : LuaGfx::Kind::DrawRect, x, y, w, h, o.lineWidth,
                      gfxInk(o.color), o.radius);
    return 0;
  }
  const char* mode = "stroke";
  if (lua_isstring(L, base + 4)) mode = lua_tostring(L, base + 4);
  const int colorIdx = lua_isstring(L, base + 4) ? base + 5 : base + 4;
  const float color = gfxColorArg(L, colorIdx, lua_isstring(L, base + 4) ? 15.0f : 15.0f);
  const bool filled = strcmp(mode, "fill") == 0;
  host->gfx.addRect(filled ? LuaGfx::Kind::FillRect : LuaGfx::Kind::DrawRect, x, y, w, h, 1,
                    gfxInk(color));
  return 0;
}

int g_line(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:line(x1, y1, x2, y2[, color])
  int x1 = static_cast<int>(luaL_checkinteger(L, base));
  int y1 = static_cast<int>(luaL_checkinteger(L, base + 1));
  int x2 = static_cast<int>(luaL_checkinteger(L, base + 2));
  int y2 = static_cast<int>(luaL_checkinteger(L, base + 3));
  // 2.x: {color, line_width}. line_width 1 is Bresenham (both ends drawn);
  // thicker lines cover the pixels nearer than line_width / 2 to the segment.
  if (lua_istable(L, base + 4)) {
    const GfxStyle o = gfxStyleTable(L, base + 4);
    host->gfx.addLine(x1, y1, x2, y2, gfxInk(o.color), o.lineWidth);
    return 0;
  }
  host->gfx.addLine(x1, y1, x2, y2, gfxInk(gfxColorArg(L, base + 4, 15.0f)));
  return 0;
}

int g_circle(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:circle(cx, cy, r[, "fill"|"stroke"][, color])
  int cx = static_cast<int>(luaL_checkinteger(L, base));
  int cy = static_cast<int>(luaL_checkinteger(L, base + 1));
  int r = static_cast<int>(luaL_checkinteger(L, base + 2));
  // 2.x: {fill, line_width, color}.
  if (lua_istable(L, base + 3)) {
    const GfxStyle o = gfxStyleTable(L, base + 3);
    host->gfx.addCircle(o.fill ? LuaGfx::Kind::FillCircle : LuaGfx::Kind::DrawCircle, cx, cy, r,
                        gfxInk(o.color), o.lineWidth);
    return 0;
  }
  const char* mode = "stroke";
  if (lua_isstring(L, base + 3)) mode = lua_tostring(L, base + 3);
  const int colorIdx2 = lua_isstring(L, base + 3) ? base + 4 : base + 3;
  const bool filled = strcmp(mode, "fill") == 0;
  host->gfx.addCircle(filled ? LuaGfx::Kind::FillCircle : LuaGfx::Kind::DrawCircle, cx, cy, r,
                      gfxInk(gfxColorArg(L, colorIdx2, 15.0f)));
  return 0;
}

int g_image(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:image(key_or_handle, x, y[, {color = level, invert = bool}]).
  // Assets come from preload_assets (manifest.json) — XIC files preloaded at
  // app load; a handle from ctx.assets:handle(key) is accepted too.
  const char* key = gfxImageKey(L, base);
  if (key == nullptr) return luaL_error(L, "g:image: expected an asset key or handle");
  const int x = static_cast<int>(luaL_checkinteger(L, base + 1));
  const int y = static_cast<int>(luaL_checkinteger(L, base + 2));
  const float color = gfxColorArg(L, base + 3, 15.0f);
  const bool invert = gfxFlagArg(L, base + 3, "invert");
  const uint8_t* bits = host->imageBits(key);
  if (bits == nullptr) {
    host->noteAssetMiss();
    return luaL_error(L, "g:image: asset '%s' not preloaded (preload_assets?)", key);
  }
  host->noteAssetHit();
  const int iw = host->imageWidth(key);
  const int ih = host->imageHeight(key);
  // The device never scales: a w/h in the opts that differs from the XIC size is
  // a diagnosable warning and the image is still drawn at its source size.
  if (lua_istable(L, base + 3)) {
    const char* whKeys[] = {"w", "h", "width", "height", nullptr};
    for (int i = 0; whKeys[i] != nullptr; ++i) {
      lua_getfield(L, base + 3, whKeys[i]);
      if (lua_isnumber(L, -1)) {
        const int asked = static_cast<int>(lua_tointeger(L, -1));
        const int native_ = (whKeys[i][0] == 'w') ? iw : ih;
        if (asked != native_) {
          LOG_ERR("LUAHOST", "g:image '%s': %s=%d differs from native %d; drawn unscaled", key,
                  whKeys[i], asked, native_);
        }
      }
      lua_pop(L, 1);
    }
  }
  if (color < 8.0f && !invert) {
    // Tint "0" (white): the matte layer. Lay down a white fill of the same
    // footprint so subsequent art composites on clean paper.
    host->gfx.addRect(LuaGfx::Kind::FillRect, x, y, iw, ih, 1, false);
    return 0;
  }
  // Normal: ink pixels take the requested color. invert flips polarity, which is
  // how an app draws a white-on-black asset.
  host->gfx.addImage(bits, iw, ih, x, y, gfxInk(color) != invert);
  return 0;
}

int g_size(lua_State* L) {
  ScriptHost* host = hostOf(L);
  // Size of the CURRENT draw target: the layer inside layer:draw_with, the panel
  // otherwise (coordinates must land inside that target).
  int w = 0;
  int h = 0;
  host->gfx.targetSize(w, h);
  lua_createtable(L, 2, 2);
  lua_pushinteger(L, w);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, h);
  lua_setfield(L, -2, "height");
  lua_pushinteger(L, w);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, h);
  lua_rawseti(L, -2, 2);
  return 1;
}

int g_layer(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:layer(layer, x, y) — blit a ctx.layers handle.
  const int id = layerIdFromArg(L, base);
  if (id < 0) return luaL_error(L, "g:layer: expected a layer handle");
  const int x = static_cast<int>(luaL_checkinteger(L, base + 1));
  const int y = static_cast<int>(luaL_checkinteger(L, base + 2));
  host->gfx.addLayerBlit(id, x, y, true);
  return 0;
}

const luaL_Reg gFuncs[] = {
    {"clear", g_clear}, {"text", g_text},   {"rect", g_rect},   {"line", g_line},
    {"circle", g_circle}, {"image", g_image}, {"size", g_size},  {"layer", g_layer},
    {nullptr, nullptr},
};


// --- extended XTApp API (spec: xteink-xtapp-lua-api, api 0.8) ---------------
//
// Namespaces the original firmware exposes besides `g` and ctx's core:
// ctx.sys / ctx.log / ctx.longtask / ctx.display / ctx.system / ctx.i18n /
// ctx.perf / ctx.gc / ctx.input / ctx.lock / ctx.assets / ctx.data / ctx.layers
// and the XTEink.SystemStatusProxy mirrors.
//
// Capabilities our firmware does not have answer honestly (nil + reason, or a
// documented no-op) instead of faking success: real apps probe with
// `if ctx.i18n and ctx.i18n.t then` and must see the truth.

struct LayerHandle {
  int id;
};

constexpr const char* kLayerMt = "xtapp.layer";
constexpr const char* kReadOnlyMt = "xtapp.ro";
constexpr int kMaxLogLine = 220;
constexpr int kMaxDataLine = 1024;

int layerIdFromArg(lua_State* L, int idx) {
  if (lua_type(L, idx) == LUA_TUSERDATA) {
    LayerHandle* h = static_cast<LayerHandle*>(luaL_testudata(L, idx, kLayerMt));
    if (h != nullptr) return h->id;
  }
  if (lua_istable(L, idx)) {  // tolerate a handle table
    lua_getfield(L, idx, "handle");
    const int id = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : -1;
    lua_pop(L, 1);
    return id;
  }
  return -1;
}

void pushReadOnly(lua_State* L, int nargs) {
  // Wraps the top table with a metatable whose __newindex raises: the contract
  // says clock/status tables are read-only ("asignar hour lanza un error").
  if (nargs < 1) return;
  luaL_getmetatable(L, kReadOnlyMt);
  if (lua_istable(L, -1)) lua_setmetatable(L, -2);
  else lua_pop(L, 1);
}

// Shared clock snapshot for ctx.sys:clock()/epoch_sec()/local_sec(). The RTC
// HAL already applies the POSIX TZ rule, so this is the same local wall clock
// the status bar draws.
struct ClockSnapshot {
  bool valid = false;
  int hour = 0, minute = 0, second = 0;
  int year = 1970, month = 1, day = 1, weekday = 0;
  int offsetMinutes = 0;
  double epochSec = 0, localSec = 0;
  const char* source = "none";
  const char* trust = "invalid";
};

bool clockSnapshot(ClockSnapshot& out) {
  struct tm t {};
  if (!halClock.isAvailable() || !halClock.localTime(t)) return false;
  out.hour = t.tm_hour;
  out.minute = t.tm_min;
  out.second = t.tm_sec;
  out.year = t.tm_year + 1900;
  out.month = t.tm_mon + 1;
  out.day = t.tm_mday;
  out.weekday = t.tm_wday;
  struct tm copy = t;
  const time_t epoch = mktime(&copy);
  struct tm utc {};
  gmtime_r(&epoch, &utc);
  out.offsetMinutes =
      (t.tm_yday - utc.tm_yday) * 1440 + (t.tm_hour - utc.tm_hour) * 60 + (t.tm_min - utc.tm_min);
  out.epochSec = static_cast<double>(epoch);
  out.localSec = out.epochSec + out.offsetMinutes * 60.0;
  out.valid = out.year >= 2024;  // an unset RTC reads 1970
  out.source = out.valid ? "hardware_rtc" : "none";
  out.trust = out.valid ? "plausible" : "invalid";
  return out.valid;
}

int sys_millis(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(millis()));
  return 1;
}

int sys_epoch_sec(lua_State* L) {
  ClockSnapshot c;
  if (!clockSnapshot(c)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, c.epochSec);
  return 1;
}

int sys_local_sec(lua_State* L) {
  ClockSnapshot c;
  if (!clockSnapshot(c)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushnumber(L, c.localSec);
  return 1;
}

void pushClockTable(lua_State* L, const ClockSnapshot& c) {
  lua_createtable(L, 0, 16);
  lua_pushboolean(L, c.valid);
  lua_setfield(L, -2, "valid");
  if (c.valid) {
    lua_pushinteger(L, c.hour);
    lua_setfield(L, -2, "hour");
    lua_pushinteger(L, c.minute);
    lua_setfield(L, -2, "minute");
    lua_pushinteger(L, c.second);
    lua_setfield(L, -2, "second");
    lua_pushinteger(L, c.year);
    lua_setfield(L, -2, "year");
    lua_pushinteger(L, c.month);
    lua_setfield(L, -2, "month");
    lua_pushinteger(L, c.day);
    lua_setfield(L, -2, "day");
    lua_pushinteger(L, c.weekday);
    lua_setfield(L, -2, "weekday");
    lua_pushinteger(L, c.offsetMinutes);
    lua_setfield(L, -2, "offset_minutes");
    lua_pushnumber(L, c.epochSec);
    lua_setfield(L, -2, "epoch_sec");
    lua_pushnumber(L, c.localSec);
    lua_setfield(L, -2, "local_sec");
    lua_pushstring(L, c.source);
    lua_setfield(L, -2, "source");
    lua_pushstring(L, c.trust);
    lua_setfield(L, -2, "trust");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "source_stale");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "revision");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "timezone_revision");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "format_revision");
  }
  pushReadOnly(L, 1);
}

int sys_clock(lua_State* L) {
  ClockSnapshot c;
  clockSnapshot(c);
  pushClockTable(L, c);
  return 1;
}

int sys_battery(lua_State* L) {
  const uint16_t pct = powerManager.getBatteryPercentage();
  lua_createtable(L, 0, 3);
  lua_pushstring(L, pct > 0 ? "available" : "unavailable");
  lua_setfield(L, -2, "availability");
  lua_pushinteger(L, pct);
  lua_setfield(L, -2, "percentage");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "millivolts");
  pushReadOnly(L, 1);
  return 1;
}

// --- ctx.log ---------------------------------------------------------------

std::string joinArgs(lua_State* L, int first) {
  std::string out;
  const int n = lua_gettop(L);
  for (int i = first; i <= n; ++i) {
    size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    if (s != nullptr) {
      if (!out.empty()) out += ' ';
      out.append(s, len);
    }
    lua_pop(L, 1);
    if (out.size() > kMaxLogLine) {
      out.resize(kMaxLogLine);
      out += "...";
      break;
    }
  }
  return out;
}

int log_write(lua_State* L, bool warn) {
  ScriptHost* host = hostOf(L);
  const int first = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const std::string line = joinArgs(L, first);
  const std::string tag = "APP:" + (host != nullptr ? host->appName() : std::string("?"));
  if (warn) {
    LOG_ERR("LUAHOST", "[%s] %s", tag.c_str(), line.c_str());
  } else {
    LOG_INF("LUAHOST", "[%s] %s", tag.c_str(), line.c_str());
  }
  return 0;
}

int log_info(lua_State* L) { return log_write(L, false); }
int log_warn(lua_State* L) { return log_write(L, true); }
int log_error(lua_State* L) { return log_write(L, true); }

// --- ctx.longtask ----------------------------------------------------------
//
// The original aborts a callback that overruns its budget unless a long task
// opened a grace window. Our port never aborts callbacks, so start()/feed()
// succeed and just record the state (an app calling them works unchanged);
// stop() means "the user cancelled", which the contract defines as a quit.
int lt_start(lua_State* L) {
  ScriptHost* host = hostOf(L);
  if (host != nullptr) host->longTaskActive = true;
  lua_pushboolean(L, 1);
  return 1;
}

int lt_feed(lua_State* L) {
  ScriptHost* host = hostOf(L);
  if (host == nullptr || !host->longTaskActive) {
    lua_pushnil(L);
    lua_pushliteral(L, "inactive");
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int lt_stop(lua_State* L) {
  ScriptHost* host = hostOf(L);
  if (host != nullptr) {
    host->longTaskActive = false;
    host->requestQuit();
    LOG_INF("LUAHOST", "longtask:stop() -> quit requested");
  }
  return 0;
}

// --- ctx.display -----------------------------------------------------------

int display_full_refresh(lua_State* L) {
  ScriptHost* host = hostOf(L);
  if (host != nullptr) host->requestFullRefresh();
  return 0;
}

int display_defer_auto_full(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const lua_Integer ms = luaL_optinteger(L, base, 0);
  if (host != nullptr && ms > 0) host->deferAutoFullUntil = millis() + static_cast<uint32_t>(ms);
  return 0;
}

// --- ctx.system ------------------------------------------------------------

int system_set_as_lockscreen_app(lua_State* L) {
  LOG_ERR("LUAHOST", "ctx.system:set_as_lockscreen_app() unsupported on this port");
  lua_pushboolean(L, 0);
  return 1;
}

// --- ctx.perf --------------------------------------------------------------

int perf_info(lua_State* L) {
  ScriptHost* host = hostOf(L);
  lua_createtable(L, 0, 7);
  const auto put = [&](const char* k, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
  };
  put("tick_us", host != nullptr ? static_cast<lua_Integer>(host->lastTickUs) : 0);
  put("draw_us", host != nullptr ? static_cast<lua_Integer>(host->lastDrawUs) : 0);
  put("cmd_count", host != nullptr ? static_cast<lua_Integer>(host->lastFrameOps) : 0);
  put("image_count", host != nullptr ? static_cast<lua_Integer>(host->lastFrameImages) : 0);
  put("asset_hit", host != nullptr ? static_cast<lua_Integer>(host->assetHits) : 0);
  put("asset_miss", host != nullptr ? static_cast<lua_Integer>(host->assetMisses) : 0);
  put("draw_frame", host != nullptr ? static_cast<lua_Integer>(host->frameCount) : 0);
  return 1;
}

// --- ctx.gc ----------------------------------------------------------------

int gc_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(lua_gc(L, LUA_GCCOUNT)));
  return 1;
}

int gc_collect(lua_State* L) {
  lua_gc(L, LUA_GCCOLLECT);
  return 0;
}

// --- ctx.input -------------------------------------------------------------

int input_has_key(lua_State* L) {
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* key = luaL_checkstring(L, base);
  const bool ok = key != nullptr && (strcmp(key, "up") == 0 || strcmp(key, "down") == 0 ||
                                     strcmp(key, "left") == 0 || strcmp(key, "right") == 0 ||
                                     strcmp(key, "ok") == 0 || strcmp(key, "back") == 0);
  lua_pushboolean(L, ok);
  return 1;
}

int input_has_bt_keyboard(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

int input_has_bt_gamepad(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

// --- ctx.lock (lockscreen entry only) --------------------------------------

int lock_flush_once(lua_State* L) {
  LOG_ERR("LUAHOST", "ctx.lock:flush_once() unsupported (no script lockscreen on this port)");
  return 0;
}

int lock_set_interval(lua_State* L) { return 0; }

// --- ctx.assets ------------------------------------------------------------

int assets_evict(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* key = luaL_checkstring(L, base);
  lua_pushboolean(L, host != nullptr && host->evictAsset(key));
  return 1;
}

int assets_info(lua_State* L) {
  ScriptHost* host = hostOf(L);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, host != nullptr ? static_cast<lua_Integer>(host->assetBytes()) : 0);
  lua_setfield(L, -2, "used");
  lua_pushinteger(L, 256 * 1024);
  lua_setfield(L, -2, "limit");
  lua_pushinteger(L, host != nullptr ? host->assetCount() : 0);
  lua_setfield(L, -2, "count");
  return 1;
}

int assets_handle(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* key = luaL_checkstring(L, base);
  if (host == nullptr || host->imageBits(key) == nullptr) {
    lua_pushnil(L);
    lua_pushliteral(L, "not_found");
    return 2;
  }
  lua_createtable(L, 0, 4);
  lua_pushstring(L, key);
  lua_setfield(L, -2, "handle");
  lua_pushstring(L, key);
  lua_setfield(L, -2, "asset_key");
  lua_pushinteger(L, host->imageWidth(key));
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, host->imageHeight(key));
  lua_setfield(L, -2, "height");
  return 1;
}


// --- ctx.i18n --------------------------------------------------------------
//
// Texto traducido: lang/<locale>.tsv del propio app (una linea "clave<TAB>valor").
// Sin traduccion se devuelve la clave tal cual, que es el respaldo legible que
// pide el contrato.
int i18n_t(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* key = luaL_checkstring(L, base);
  const char* value = (host != nullptr) ? host->i18nLookup(key) : nullptr;
  // Missing key falls back to the key itself (contract section 8).
  std::string out = value != nullptr ? value : key;
  // In a language file the two characters "\n" mean a line break.
  for (size_t p = out.find("\\n"); p != std::string::npos; p = out.find("\\n", p + 1)) {
    out.replace(p, 2, "\n");
  }
  // {name} -> tostring(params.name); a name the table does not carry stays as
  // written (contract section 8).
  if (lua_istable(L, base + 1)) {
    size_t p = 0;
    while ((p = out.find('{', p)) != std::string::npos) {
      const size_t close = out.find('}', p + 1);
      if (close == std::string::npos) break;
      const std::string name = out.substr(p + 1, close - p - 1);
      lua_getfield(L, base + 1, name.c_str());
      bool replaced = false;
      if (lua_isstring(L, -1) || lua_isnumber(L, -1)) {
        size_t vlen = 0;
        const char* v = lua_tolstring(L, -1, &vlen);
        if (v != nullptr) {
          out.replace(p, close - p + 1, std::string(v, vlen));
          p += vlen;
          replaced = true;
        }
      }
      lua_pop(L, 1);
      if (!replaced) p = close + 1;
    }
  }
  lua_pushlstring(L, out.c_str(), out.size());
  return 1;
}

// --- ctx.data --------------------------------------------------------------
//
// data/<name>.tsv|.txt declarados en manifest.data_files. Solo el nombre del
// fichero, nunca una ruta (mismo contrato que el manifest).
bool dataNameOk(const char* name) {
  if (name == nullptr) return false;
  const size_t n = strlen(name);
  if (n == 0 || n > 64) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = name[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return strstr(name, "..") == nullptr;
}

struct DataReader {
  HalFile file;
  int maxLine = kMaxDataLine;
  bool open = false;
};

int data_read_line(lua_State* L) {
  DataReader* r = static_cast<DataReader*>(luaL_checkudata(L, 1, "xtapp.datareader"));
  if (!r->open || !r->file.isOpen()) {
    lua_pushnil(L);
    lua_pushliteral(L, "read_failed");
    return 2;
  }
  std::string line;
  line.reserve(64);
  while (true) {
    const int c = r->file.read();
    if (c < 0) break;
    if (c == '\n') {
      lua_pushlstring(L, line.c_str(), line.size());
      return 1;
    }
    if (c != '\r') line.push_back(static_cast<char>(c));
    if (static_cast<int>(line.size()) >= r->maxLine) {
      lua_pushnil(L);
      lua_pushliteral(L, "line_too_long");
      return 2;
    }
  }
  if (!line.empty()) {  // last line without a trailing newline
    lua_pushlstring(L, line.c_str(), line.size());
    return 1;
  }
  lua_pushnil(L);
  return 1;
}

int data_seek(lua_State* L) {
  DataReader* r = static_cast<DataReader*>(luaL_checkudata(L, 1, "xtapp.datareader"));
  const lua_Integer pos = luaL_checkinteger(L, 2);
  lua_pushboolean(L, r->open && r->file.seek(static_cast<size_t>(pos)));
  return 1;
}

int data_tell(lua_State* L) {
  DataReader* r = static_cast<DataReader*>(luaL_checkudata(L, 1, "xtapp.datareader"));
  lua_pushinteger(L, r->open ? static_cast<lua_Integer>(r->file.position()) : 0);
  return 1;
}

int data_close(lua_State* L) {
  DataReader* r = static_cast<DataReader*>(luaL_checkudata(L, 1, "xtapp.datareader"));
  if (r->open) {
    r->file.close();
    r->open = false;
  }
  return 0;
}

int data_gc(lua_State* L) { return data_close(L); }

int data_exists(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* name = luaL_checkstring(L, base);
  if (host == nullptr || !dataNameOk(name)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushboolean(L, Storage.exists((host->appPath() + "data/" + name).c_str()) ? 1 : 0);
  return 1;
}

int data_size(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* name = luaL_checkstring(L, base);
  if (host == nullptr || !dataNameOk(name)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_name");
    return 2;
  }
  HalFile f = Storage.open((host->appPath() + "data/" + name).c_str());
  if (!f.isOpen()) {
    lua_pushnil(L);
    lua_pushliteral(L, "not_found");
    return 2;
  }
  const lua_Integer size = static_cast<lua_Integer>(f.fileSize());
  f.close();
  lua_pushinteger(L, size);
  return 1;
}

int data_read_text(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* name = luaL_checkstring(L, base);
  if (host == nullptr || !dataNameOk(name)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_name");
    return 2;
  }
  const String content = Storage.readFile((host->appPath() + "data/" + name).c_str());
  if (content.length() == 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "not_found");
    return 2;
  }
  lua_pushlstring(L, content.c_str(), content.length());
  return 1;
}

int data_open_text(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* name = luaL_checkstring(L, base);
  if (host == nullptr || !dataNameOk(name)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_name");
    return 2;
  }
  DataReader* r = static_cast<DataReader*>(lua_newuserdata(L, sizeof(DataReader)));
  new (r) DataReader();  // trivially destructible, but keep it explicit
  luaL_getmetatable(L, "xtapp.datareader");
  lua_setmetatable(L, -2);
  if (lua_istable(L, base + 1)) {
    lua_getfield(L, base + 1, "max_line_bytes");
    if (lua_isnumber(L, -1)) r->maxLine = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
  }
  if (r->maxLine <= 0) r->maxLine = kMaxDataLine;
  r->file = Storage.open((host->appPath() + "data/" + name).c_str());
  r->open = r->file.isOpen();
  if (!r->open) {
    lua_pop(L, 1);  // drop the userdata
    lua_pushnil(L);
    lua_pushliteral(L, "not_found");
    return 2;
  }
  return 1;
}

// --- ctx.layers ------------------------------------------------------------

int layers_create(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const int w = static_cast<int>(luaL_checkinteger(L, base));
  const int h = static_cast<int>(luaL_checkinteger(L, base + 1));
  const int id = host->gfx.createLayer(w, h);
  if (id < 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "unsupported");
    return 2;
  }
  LayerHandle* handle = static_cast<LayerHandle*>(lua_newuserdata(L, sizeof(LayerHandle)));
  handle->id = id;
  luaL_getmetatable(L, kLayerMt);
  lua_setmetatable(L, -2);
  return 1;
}

int layer_draw_with(lua_State* L) {
  ScriptHost* host = hostOf(L);
  LayerHandle* handle = static_cast<LayerHandle*>(luaL_checkudata(L, 1, kLayerMt));
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (!host->gfx.beginLayer(handle->id)) {
    return 0;  // nested draw_with: the original refuses it the same way
  }
  // The callback receives the same `g` table (the contract's example is
  // layer:draw_with(function(g) ... end)).
  lua_getglobal(L, "g");
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    LOG_ERR("LUAHOST", " layer:draw_with: %s", msg != nullptr ? msg : "(no message)");
    host->setLastError(std::string("layer:draw_with: ") + (msg != nullptr ? msg : "error"));
    lua_pop(L, 1);
  }
  host->gfx.endLayer();
  return 0;
}

int layer_dispose(lua_State* L) {
  ScriptHost* host = hostOf(L);
  LayerHandle* handle = static_cast<LayerHandle*>(luaL_checkudata(L, 1, kLayerMt));
  if (handle->id >= 0 && host != nullptr) {
    host->gfx.disposeLayer(handle->id);
    handle->id = -1;
  }
  return 0;
}

int layer_gc(lua_State* L) {
  LayerHandle* handle = static_cast<LayerHandle*>(luaL_testudata(L, 1, kLayerMt));
  ScriptHost* host = hostOf(L);
  if (handle != nullptr && host != nullptr && handle->id >= 0) {
    host->gfx.disposeLayer(handle->id);
    handle->id = -1;
  }
  return 0;
}

int ro_newindex(lua_State* L) {
  return luaL_error(L, "table is read-only");
}

void registerXtappMetatables(lua_State* L) {
  luaL_newmetatable(L, kLayerMt);
  lua_pushcfunction(L, layer_gc);
  lua_setfield(L, -2, "__gc");
  lua_newtable(L);
  lua_pushcfunction(L, layer_draw_with);
  lua_setfield(L, -2, "draw_with");
  lua_pushcfunction(L, layer_dispose);
  lua_setfield(L, -2, "dispose");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, "xtapp.datareader");
  lua_pushcfunction(L, data_gc);
  lua_setfield(L, -2, "__gc");
  lua_newtable(L);
  lua_pushcfunction(L, data_read_line);
  lua_setfield(L, -2, "read_line");
  lua_pushcfunction(L, data_seek);
  lua_setfield(L, -2, "seek");
  lua_pushcfunction(L, data_tell);
  lua_setfield(L, -2, "tell");
  lua_pushcfunction(L, data_close);
  lua_setfield(L, -2, "close");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);

  luaL_newmetatable(L, kReadOnlyMt);
  lua_pushcfunction(L, ro_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pop(L, 1);
}

// --- namespace tables ------------------------------------------------------

const luaL_Reg sysFuncs[] = {
    {"millis", sys_millis},      {"uptime_ms", sys_millis},   {"epoch_sec", sys_epoch_sec},
    {"local_sec", sys_local_sec}, {"clock", sys_clock},        {"battery", sys_battery},
    {nullptr, nullptr},
};
const luaL_Reg logFuncs[] = {
    {"info", log_info}, {"warn", log_warn}, {"error", log_error}, {nullptr, nullptr},
};
const luaL_Reg longtaskFuncs[] = {
    {"start", lt_start}, {"feed", lt_feed}, {"stop", lt_stop}, {nullptr, nullptr},
};
const luaL_Reg displayFuncs[] = {
    {"full_refresh", display_full_refresh},
    {"defer_auto_full", display_defer_auto_full},
    {nullptr, nullptr},
};
const luaL_Reg systemFuncs[] = {
    {"set_as_lockscreen_app", system_set_as_lockscreen_app}, {nullptr, nullptr},
};
const luaL_Reg i18nFuncs[] = {
    {"t", i18n_t}, {nullptr, nullptr},
};
const luaL_Reg perfFuncs[] = {
    {"info", perf_info}, {nullptr, nullptr},
};
const luaL_Reg gcFuncs[] = {
    {"count", gc_count}, {"collect", gc_collect}, {nullptr, nullptr},
};
const luaL_Reg inputFuncs[] = {
    {"has_key", input_has_key},
    {"has_bt_keyboard", input_has_bt_keyboard},
    {"has_bt_gamepad", input_has_bt_gamepad},
    {nullptr, nullptr},
};
const luaL_Reg lockFuncs[] = {
    {"flush_once", lock_flush_once}, {"set_interval", lock_set_interval}, {nullptr, nullptr},
};
const luaL_Reg assetsFuncs[] = {
    {"evict", assets_evict}, {"info", assets_info}, {"handle", assets_handle}, {nullptr, nullptr},
};
// --- ctx.fonts (2.x contract, section 7) ------------------------------------
//
// A font object is a plain table: { id, name, line_height, ascent, measure }.
// The metrics come from the renderer for the injected id, so an app's own
// centring math lands exactly where the text is drawn.
int font_measure(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;  // font:measure(text)
  const char* s = luaL_checkstring(L, base);
  int id = host != nullptr ? host->defaultFontId : 0;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "id");
    if (lua_isnumber(L, -1)) id = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
  }
  lua_pushinteger(L, host != nullptr ? host->textWidth(id, s) : 0);
  return 1;
}

void pushFontObject(lua_State* L, ScriptHost* host, const char* name, int id) {
  lua_createtable(L, 0, 5);
  lua_pushinteger(L, id);
  lua_setfield(L, -2, "id");
  lua_pushstring(L, name);
  lua_setfield(L, -2, "name");
  lua_pushinteger(L, host->lineHeightOf(id));
  lua_setfield(L, -2, "line_height");
  lua_pushinteger(L, host->ascenderOf(id));
  lua_setfield(L, -2, "ascent");
  lua_pushcfunction(L, font_measure);
  lua_setfield(L, -2, "measure");
}

// --- ctx.package (2.x contract, section 8) ----------------------------------
//
// Package images are the XIC files the manifest preloads. The object a script
// gets is a table carrying the asset key; g:image accepts it (or the 0.8 bare
// key string) and the size comes from the decoded asset.
int pkg_image_release(lua_State* L);

int pkg_image(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
  const char* key = luaL_checkstring(L, base);
  if (host == nullptr || key == nullptr || strchr(key, '/') != nullptr ||
      strstr(key, "..") != nullptr) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  const int w = host->imageWidth(key);
  const int h = host->imageHeight(key);
  if (w <= 0 || h <= 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "not_found");
    return 2;
  }
  lua_createtable(L, 0, 5);
  lua_pushliteral(L, "image");
  lua_setfield(L, -2, "__kind");
  lua_pushstring(L, key);
  lua_setfield(L, -2, "key");
  lua_pushinteger(L, w);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, h);
  lua_setfield(L, -2, "height");
  lua_pushcfunction(L, pkg_image_release);
  lua_setfield(L, -2, "release");
  return 1;
}

int pkg_image_release(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const char* key = nullptr;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "key");
    if (lua_isstring(L, -1)) key = lua_tostring(L, -1);
  }
  // Drops the decoded asset; the object stays readable and can be drawn again
  // (the host re-decodes it), so releasing twice is harmless.
  if (host != nullptr && key != nullptr) host->evictAsset(key);
  lua_pushboolean(L, 1);
  return 1;
}

int pkg_exists(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
  const char* name = luaL_checkstring(L, base);
  lua_pushboolean(L, (host != nullptr && name != nullptr && host->imageWidth(name) > 0) ? 1 : 0);
  return 1;
}

int pkg_cache_stats(lua_State* L) {
  ScriptHost* host = hostOf(L);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, host != nullptr ? host->assetCount() : 0);
  lua_setfield(L, -2, "count");
  lua_pushinteger(L, 320);  // the contract's ceiling, reported as the host limit
  lua_setfield(L, -2, "limit");
  lua_pushinteger(L, host != nullptr ? static_cast<lua_Integer>(host->assetBytes()) : 0);
  lua_setfield(L, -2, "bytes");
  return 1;
}

const luaL_Reg packageFuncs[] = {
    {"image", pkg_image},
    {"exists", pkg_exists},
    {"cache_stats", pkg_cache_stats},
    // The manifest's read-only data files (section 8): the same implementations
    // ctx.data already exposes for 0.8 apps.
    {"size", data_size},
    {"read", data_read_text},
    {"open_text", data_open_text},
    {nullptr, nullptr},
};

// --- ctx.storage / ctx.json (2.x contract, sections 9-10) -------------------
// (appendJsonString lives with the state writer further down.)
void appendJsonString(std::string& out, const char* s, size_t len);

// 2.x apps do NOT persist through ctx.state: they keep session state in module
// locals and write it with ctx.json:encode + ctx.storage:write. The 0.8
// ctx.data namespace stays for 0.8 apps; these are the 2.x names over the same
// private per-app directory.
constexpr size_t STORAGE_MAX_FILE = 262144;      // 256 KB per file
constexpr size_t STORAGE_MAX_FILES = 512;
constexpr size_t STORAGE_MAX_TOTAL = 134217728;  // 128 MB per app
constexpr size_t JSON_MAX_OUTPUT = 262144;
constexpr int JSON_MAX_DEPTH = 32;
constexpr const char* kJsonNullRegistry = "x4_json_null";
constexpr const char* kStoragePrefix = "storage/";

// name: 1..64 bytes of [a-z0-9._-], no leading or trailing '.'. The trailing
// rule is also what keeps "<name>.tmp" from ever colliding with a real file.
static bool storageNameOk(const char* name) {
  if (name == nullptr) return false;
  const size_t n = strlen(name);
  if (n == 0 || n > 64) return false;
  if (name[0] == '.' || name[n - 1] == '.') return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = name[i];
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static std::string storageDirOf(const ScriptHost* host) { return host->appPath() + kStoragePrefix; }

// ctx.storage:read(...) — methods are called with the table as arg 1.
static int storageArgBase(lua_State* L) { return lua_type(L, 1) == LUA_TTABLE ? 2 : 1; }

// Files + total bytes in use, optionally collecting the names.
static size_t storageScan(const ScriptHost* host, std::vector<std::string>* names) {
  size_t total = 0;
  HalFile dir = Storage.open(storageDirOf(host).c_str());
  if (!dir.isOpen() || !dir.isDirectory()) return 0;
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    char nm[80];
    const size_t n = f.getName(nm, sizeof(nm));
    if (names != nullptr && n > 0) names->push_back(std::string(nm, n));
    total += static_cast<size_t>(f.fileSize());
  }
  return total;
}

int storage_read(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const char* name = luaL_checkstring(L, storageArgBase(L));
  if (host == nullptr || !storageNameOk(name)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  const std::string path = storageDirOf(host) + name;
  if (!Storage.exists(path.c_str())) {
    lua_pushnil(L);
    lua_pushliteral(L, "not_found");
    return 2;
  }
  String content = Storage.readFile(path.c_str());
  lua_pushlstring(L, content.c_str(), content.length());
  return 1;
}

int storage_write(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = storageArgBase(L);
  const char* name = luaL_checkstring(L, base);
  size_t len = 0;
  const char* data = luaL_checklstring(L, base + 1, &len);
  if (host == nullptr || !storageNameOk(name)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  if (len > STORAGE_MAX_FILE) {
    lua_pushnil(L);
    lua_pushliteral(L, "limit_exceeded");
    return 2;
  }
  const std::string dir = storageDirOf(host);
  const std::string path = dir + name;
  const bool existed = Storage.exists(path.c_str());
  std::vector<std::string> names;
  size_t files = 0;
  const size_t total = storageScan(host, &names);
  files = names.size();
  size_t prev = 0;
  if (existed) {
    HalFile old = Storage.open(path.c_str());
    if (old.isOpen()) {
      prev = static_cast<size_t>(old.fileSize());
      old.close();
    }
  }
  if (!existed && files + 1 > STORAGE_MAX_FILES) {
    lua_pushnil(L);
    lua_pushliteral(L, "limit_exceeded");
    return 2;
  }
  if (total - prev + len > STORAGE_MAX_TOTAL) {
    lua_pushnil(L);
    lua_pushliteral(L, "limit_exceeded");
    return 2;
  }
  if (!Storage.exists(dir.c_str())) Storage.mkdir(dir.c_str());
  // Atomic: write the temp file, then rename over the target, so a power cut
  // leaves either the old or the new content, never half of either.
  const std::string tmp = path + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite("LUAHOST", tmp, f)) {
    lua_pushnil(L);
    lua_pushliteral(L, "io_error");
    return 2;
  }
  const size_t written = (len > 0) ? f.write(reinterpret_cast<const uint8_t*>(data), len) : 0;
  f.flush();
  f.close();
  if (written != len || !Storage.rename(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    lua_pushnil(L);
    lua_pushliteral(L, "io_error");
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

int storage_remove(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const char* name = luaL_checkstring(L, storageArgBase(L));
  if (host == nullptr || !storageNameOk(name)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  const std::string path = storageDirOf(host) + name;
  if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
    lua_pushnil(L);
    lua_pushliteral(L, "io_error");
    return 2;
  }
  lua_pushboolean(L, 1);  // removing a file that is not there is a success
  return 1;
}

int storage_exists(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const char* name = luaL_checkstring(L, storageArgBase(L));
  if (host == nullptr || !storageNameOk(name)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushboolean(L, Storage.exists((storageDirOf(host) + name).c_str()) ? 1 : 0);
  return 1;
}

int storage_size(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const char* name = luaL_checkstring(L, storageArgBase(L));
  if (host == nullptr || !storageNameOk(name)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  HalFile f = Storage.open((storageDirOf(host) + name).c_str());
  if (!f.isOpen()) {
    lua_pushnil(L);
    lua_pushliteral(L, "not_found");
    return 2;
  }
  const lua_Integer size = static_cast<lua_Integer>(f.fileSize());
  f.close();
  lua_pushinteger(L, size);
  return 1;
}

int storage_list(lua_State* L) {
  ScriptHost* host = hostOf(L);
  std::vector<std::string> names;
  if (host != nullptr) storageScan(host, &names);
  std::sort(names.begin(), names.end());  // byte order, as the contract asks
  lua_createtable(L, static_cast<int>(names.size()), 0);
  int i = 1;
  for (const std::string& n : names) {
    lua_pushlstring(L, n.c_str(), n.size());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

int storage_usage(lua_State* L) {
  ScriptHost* host = hostOf(L);
  std::vector<std::string> names;
  const size_t bytes = host != nullptr ? storageScan(host, &names) : 0;
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, static_cast<lua_Integer>(names.size()));
  lua_setfield(L, -2, "files");
  lua_pushinteger(L, static_cast<lua_Integer>(bytes));
  lua_setfield(L, -2, "bytes");
  lua_pushinteger(L, static_cast<lua_Integer>(STORAGE_MAX_FILES));
  lua_setfield(L, -2, "max_files");
  lua_pushinteger(L, static_cast<lua_Integer>(STORAGE_MAX_TOTAL));
  lua_setfield(L, -2, "max_bytes");
  return 1;
}

// ctx.json.null: one shared table so `v == ctx.json.null` is a plain compare.
static void pushJsonNull(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, kJsonNullRegistry);
  if (lua_istable(L, -1)) return;
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushliteral(L, "null");
  lua_setfield(L, -2, "__json");
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, kJsonNullRegistry);
}

static bool isJsonNull(lua_State* L, int idx) {
  lua_getfield(L, LUA_REGISTRYINDEX, kJsonNullRegistry);
  const bool same = lua_istable(L, -1) && lua_rawequal(L, idx, -1) != 0;
  lua_pop(L, 1);
  return same;
}

static bool jsonEncodeValue(lua_State* L, int idx, std::string& out, int depth) {
  if (depth > JSON_MAX_DEPTH || out.size() > JSON_MAX_OUTPUT) return false;
  const int abs = lua_absindex(L, idx);
  switch (lua_type(L, abs)) {
    case LUA_TNIL:
      out += "null";
      return true;
    case LUA_TBOOLEAN:
      out += lua_toboolean(L, abs) ? "true" : "false";
      return true;
    case LUA_TNUMBER: {
      char buf[40];
      if (lua_isinteger(L, abs)) {
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(lua_tointeger(L, abs)));
      } else {
        const double d = lua_tonumber(L, abs);
        if (!isfinite(d)) return false;  // NaN / inf are invalid_argument
        snprintf(buf, sizeof(buf), "%.9g", d);
      }
      out += buf;
      return true;
    }
    case LUA_TSTRING: {
      size_t len = 0;
      const char* s = lua_tolstring(L, abs, &len);
      appendJsonString(out, s, len);
      return true;
    }
    case LUA_TTABLE:
      break;
    default:
      return false;  // functions, coroutines, userdata, ...
  }
  if (isJsonNull(L, abs)) {
    out += "null";
    return true;
  }
  // Arrays are tables whose keys are exactly 1..n; objects have string keys;
  // anything else is invalid_argument (contract section 10).
  std::vector<lua_Integer> ints;
  std::vector<std::string> keys;
  bool hasStringKey = false;
  lua_pushnil(L);
  while (lua_next(L, abs) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      size_t klen = 0;
      const char* k = lua_tolstring(L, -2, &klen);
      keys.push_back(std::string(k, klen));
      hasStringKey = true;
    } else if (lua_isinteger(L, -2)) {
      ints.push_back(lua_tointeger(L, -2));
    } else {
      lua_pop(L, 2);
      return false;
    }
    lua_pop(L, 1);
  }
  if (!hasStringKey) {
    bool array = true;
    for (size_t i = 0; i < ints.size() && array; ++i) {
      bool found = false;
      for (size_t j = 0; j < ints.size(); ++j) {
        if (ints[j] == static_cast<lua_Integer>(i + 1)) found = true;
      }
      if (!found) array = false;
    }
    if (array) {
      out += '[';
      const int n = static_cast<int>(ints.size());
      for (int i = 1; i <= n; ++i) {
        if (i > 1) out += ',';
        lua_rawgeti(L, abs, i);
        const bool ok = jsonEncodeValue(L, -1, out, depth + 1);
        lua_pop(L, 1);
        if (!ok) return false;
      }
      out += ']';
      return true;
    }
    if (!ints.empty()) return false;  // integer keys that are not 1..n
  }
  if (ints.size() > 0 && hasStringKey) return false;  // mixed keys
  std::sort(keys.begin(), keys.end());
  out += '{';
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i > 0) out += ',';
    appendJsonString(out, keys[i].c_str(), keys[i].size());
    out += ':';
    lua_pushlstring(L, keys[i].c_str(), keys[i].size());
    lua_gettable(L, abs);
    const bool ok = jsonEncodeValue(L, -1, out, depth + 1);
    lua_pop(L, 1);
    if (!ok) return false;
  }
  out += '}';
  return true;
}

int json_encode(lua_State* L) {
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;  // ctx.json:encode(v)
  if (lua_isnoneornil(L, base)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  std::string out;
  out.reserve(256);
  if (!jsonEncodeValue(L, base, out, 0)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  if (out.size() > JSON_MAX_OUTPUT) {
    lua_pushnil(L);
    lua_pushliteral(L, "limit_exceeded");
    return 2;
  }
  lua_pushlstring(L, out.c_str(), out.size());
  return 1;
}

// decode: like restoreStateValue, except null becomes the ctx.json.null object
// and out-of-range nesting is invalid_argument.
void pushJsonValue(lua_State* L, JsonVariantConst v, int depth) {
  if (depth > JSON_MAX_DEPTH) {
    lua_pushnil(L);
    return;
  }
  if (v.is<JsonObjectConst>()) {
    lua_newtable(L);
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      pushJsonValue(L, kv.value(), depth + 1);
      lua_setfield(L, -2, kv.key().c_str());
    }
  } else if (v.is<JsonArrayConst>()) {
    lua_newtable(L);
    int i = 1;
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
      pushJsonValue(L, e, depth + 1);
      lua_rawseti(L, -2, i++);
    }
  } else if (v.is<const char*>()) {
    lua_pushstring(L, v.as<const char*>());
  } else if (v.is<bool>()) {
    lua_pushboolean(L, v.as<bool>());
  } else if (v.is<long long>()) {
    lua_pushinteger(L, static_cast<lua_Integer>(v.as<long long>()));
  } else if (v.is<double>()) {
    lua_pushnumber(L, v.as<double>());
  } else {
    pushJsonNull(L);  // JSON null
  }
}

int json_decode(lua_State* L) {
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
  size_t len = 0;
  const char* s = luaL_checklstring(L, base, &len);
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, s, len);
  if (err) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  pushJsonValue(L, doc.as<JsonVariantConst>(), 0);
  return 1;
}

const luaL_Reg storageFuncs[] = {
    {"read", storage_read},     {"write", storage_write}, {"remove", storage_remove},
    {"exists", storage_exists}, {"size", storage_size},   {"list", storage_list},
    {"usage", storage_usage},   {nullptr, nullptr},
};
const luaL_Reg jsonFuncs[] = {
    {"encode", json_encode},
    {"decode", json_decode},
    {nullptr, nullptr},
};

const luaL_Reg dataFuncs[] = {
    {"exists", data_exists},   {"size", data_size},       {"read_text", data_read_text},
    {"open_text", data_open_text}, {nullptr, nullptr},
};
const luaL_Reg layersFuncs[] = {
    {"create", layers_create}, {nullptr, nullptr},
};


// --- ctx.net: async HTTP ---------------------------------------------------
//
// The request runs on its own task and writes into a HEAP-allocated record; the
// binding only reads it, so the Lua state is never touched from another task.
// Gated on the manifest's permissions: ["net.http"], exactly like the original
// (the doc: "ctx.net is available only after this permission is declared").


// Minimal HTTP/1.1 client over the Arduino NetworkClient. The doc guarantees
// http:// only (net.http), which is exactly what a plain TCP client covers;
// https:// answers "unsupported_scheme" instead of pretending.
bool parseHttpUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
  if (url.rfind("http://", 0) != 0) return false;
  const std::string rest = url.substr(7);
  const size_t slash = rest.find('/');
  const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  path = (slash == std::string::npos) ? "/" : rest.substr(slash);
  host = authority;
  port = 80;
  const size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    port = static_cast<uint16_t>(atoi(authority.substr(colon + 1).c_str()));
  }
  return !host.empty() && port > 0;
}

// Reads one CRLF line (without the CRLF); empty string on timeout/close.
std::string netReadLine(NetworkClient& c, uint32_t timeoutMs) {
  std::string line;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!c.connected() && !c.available()) break;
    const int ch = c.read();
    if (ch < 0) {
      delay(1);
      continue;
    }
    if (ch == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    line.push_back(static_cast<char>(ch));
    if (line.size() > 2048) break;
  }
  return line;
}

void netWorkerTask(void* arg) {
  ScriptHost::NetTask* t = static_cast<ScriptHost::NetTask*>(arg);
  if (!t->cancelled) {
    std::string host, path;
    uint16_t port = 80;
    if (t->url.rfind("https://", 0) == 0) {
      t->err = "unsupported_scheme";  // https is not part of the net.http contract
    } else if (!parseHttpUrl(t->url, host, port, path)) {
      t->err = "invalid_url";
    } else {
      NetworkClient client;
      client.setTimeout(15000);
      if (!client.connect(host.c_str(), port, 10000)) {
        t->err = "connect_failed";
      } else {
        const bool post = !t->reqBody.empty();
        std::string req = std::string(post ? "POST " : "GET ") + path + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Connection: close\r\n";
        req += "Accept: */*\r\n";
        if (post) {
          req += "Content-Length: " + std::to_string(t->reqBody.size()) + "\r\n";
          req += "Content-Type: text/plain\r\n";
        }
        size_t hp = 0;
        while (hp < t->headers.size()) {
          const size_t eol = t->headers.find('\n', hp);
          const std::string line =
              t->headers.substr(hp, (eol == std::string::npos ? t->headers.size() : eol) - hp);
          if (!line.empty()) req += line + "\r\n";
          if (eol == std::string::npos) break;
          hp = eol + 1;
        }
        req += "\r\n";
        if (post) req += t->reqBody;
        client.write(reinterpret_cast<const uint8_t*>(req.data()), req.size());

        const std::string status = netReadLine(client, 15000);
        if (status.rfind("HTTP/", 0) == 0) {
          const size_t sp = status.find(' ');
          t->status = (sp != std::string::npos) ? atoi(status.c_str() + sp + 1) : 0;
        }
        long contentLength = -1;
        bool chunked = false;
        while (true) {
          const std::string line = netReadLine(client, 15000);
          if (line.empty()) break;
          std::string lower = line;
          for (char& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
          if (lower.rfind("content-length:", 0) == 0) {
            contentLength = atol(lower.c_str() + 15);
          } else if (lower.find("transfer-encoding:") == 0 && lower.find("chunked") != std::string::npos) {
            chunked = true;
          }
        }
        std::string body;
        if (chunked) {
          while (body.size() < ScriptHost::MAX_NET_RESPONSE) {
            const std::string sizeLine = netReadLine(client, 15000);
            if (sizeLine.empty()) break;
            const long chunk = strtol(sizeLine.c_str(), nullptr, 16);
            if (chunk <= 0) break;
            const size_t before = body.size();
            const uint32_t start = millis();
            while (body.size() - before < static_cast<size_t>(chunk) && millis() - start < 15000) {
              const int ch = client.read();
              if (ch < 0) {
                delay(1);
                continue;
              }
              body.push_back(static_cast<char>(ch));
            }
            netReadLine(client, 2000);  // trailing CRLF
          }
        } else {
          const uint32_t start = millis();
          while (body.size() < ScriptHost::MAX_NET_RESPONSE && millis() - start < 30000) {
            if (contentLength >= 0 && body.size() >= static_cast<size_t>(contentLength)) break;
            if (client.available() == 0 && !client.connected()) break;
            const int ch = client.read();
            if (ch < 0) {
              if (!client.connected()) break;
              delay(1);
              continue;
            }
            body.push_back(static_cast<char>(ch));
          }
        }
        client.stop();
        t->resp = body;
        t->bytes = body.size();
        t->ok = t->status >= 200 && t->status < 300;
        if (t->status == 0 && t->err.empty()) t->err = "no_response";
      }
    }
  }
  if (t->cancelled && t->err.empty()) t->err = "cancelled";
  t->done = true;
  t->task = nullptr;
  vTaskDelete(nullptr);
}

int net_get(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* url = luaL_checkstring(L, base);
  const int id = host->netStart(url, nullptr, nullptr);
  if (id < 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "unavailable");
    return 2;
  }
  lua_pushinteger(L, id);
  return 1;
}

int net_post(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const char* url = luaL_checkstring(L, base);
  const char* body = luaL_optstring(L, base + 1, "");
  std::string headers;
  if (lua_istable(L, base + 2)) {
    lua_pushnil(L);
    while (lua_next(L, base + 2) != 0) {
      const char* value = lua_tostring(L, -1);
      lua_pop(L, 1);
      const char* name = lua_tostring(L, -1);
      if (name != nullptr && value != nullptr) {
        headers += name;
        headers += ": ";
        headers += value;
        headers += '\n';
      }
    }
  }
  const int id = host->netStart(url, body, headers.empty() ? nullptr : headers.c_str());
  if (id < 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "unavailable");
    return 2;
  }
  lua_pushinteger(L, id);
  return 1;
}

int net_poll(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const int id = static_cast<int>(luaL_checkinteger(L, base));
  if (id < 0 || id >= ScriptHost::MAX_NET_TASKS || host->netTasks[id] == nullptr) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_task");
    return 2;
  }
  ScriptHost::NetTask* t = host->netTasks[id];
  lua_createtable(L, 0, 6);
  lua_pushboolean(L, t->done ? 1 : 0);
  lua_setfield(L, -2, "done");
  lua_pushboolean(L, t->ok ? 1 : 0);
  lua_setfield(L, -2, "ok");
  lua_pushinteger(L, t->status);
  lua_setfield(L, -2, "status");
  lua_pushinteger(L, static_cast<lua_Integer>(t->bytes));
  lua_setfield(L, -2, "bytes");
  if (t->done) {
    lua_pushlstring(L, t->resp.c_str(), t->resp.size());
    lua_setfield(L, -2, "body");
    if (!t->err.empty()) {
      lua_pushstring(L, t->err.c_str());
    } else {
      lua_pushnil(L);
    }
    lua_setfield(L, -2, "err");
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "body");
    lua_pushnil(L);
    lua_setfield(L, -2, "err");
  }
  return 1;
}

int net_cancel(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = (lua_type(L, 1) == LUA_TTABLE) ? 2 : 1;
  const int id = static_cast<int>(luaL_checkinteger(L, base));
  if (id < 0 || id >= ScriptHost::MAX_NET_TASKS || host->netTasks[id] == nullptr) {
    lua_pushboolean(L, 0);
    return 1;
  }
  ScriptHost::NetTask* t = host->netTasks[id];
  if (t->done) {
    delete t;  // worker is gone; safe to free here
    host->netTasks[id] = nullptr;
  } else {
    // In flight: the worker owns the record until it finishes, then frees it.
    t->cancelled = true;
    host->netTasks[id] = nullptr;
  }
  lua_pushboolean(L, 1);
  return 1;
}

const luaL_Reg netFuncs[] = {
    {"get", net_get}, {"post", net_post}, {"poll", net_poll}, {"cancel", net_cancel}, {nullptr, nullptr},
};

// --- ctx bindings ------------------------------------------------------

int ctx_screen(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "x4_screen_w");
  int w = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, "x4_screen_h");
  int h = static_cast<int>(lua_tointeger(L, -1));
  lua_pop(L, 1);
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, w);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, h);
  lua_setfield(L, -2, "height");
  return 1;
}

int ctx_invalidate(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
  // 2.x: the argument is the WAVEFORM — omitted is a local refresh, "full" a
  // full one (which also clears ghosting). Anything else is invalid_argument.
  if (lua_isnoneornil(L, base)) {
    host->requestInvalidate();
    return 0;
  }
  const char* mode = lua_tostring(L, base);
  if (mode != nullptr && strcmp(mode, "full") == 0) {
    host->requestFullRefresh();
    host->requestInvalidate();
    return 0;
  }
  lua_pushnil(L);
  lua_pushliteral(L, "invalid_argument");
  return 2;
}

// ctx:set_tick(ms) (2.x): 33..60000, or 0 to stop. The 0.8 name set_tick_rate
// with its level strings keeps working.
int ctx_set_tick(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
  const lua_Integer ms = luaL_checkinteger(L, base);
  if (ms != 0 && (ms < 33 || ms > 60000)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  host->setTickIntervalMs(static_cast<uint32_t>(ms));
  return 0;
}

// ctx:defer_full_refresh(ms) (2.x, section 5).
int ctx_defer_full_refresh(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
  const lua_Integer ms = luaL_checkinteger(L, base);
  if (ms < 1 || ms > 60000) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  host->deferAutoFullUntil = millis() + static_cast<uint32_t>(ms);
  return 0;
}

int timer_cancel(lua_State* L) {
  ScriptHost* host = hostOf(L);
  int id = -1;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "id");
    if (lua_isnumber(L, -1)) id = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
  }
  if (host != nullptr && id >= 0) host->cancelTimer(id);
  return 0;
}

// ctx:after(ms, fn) (2.x): a one-shot timer, at most 32 pending.
int ctx_after(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
  const lua_Integer ms = luaL_checkinteger(L, base);
  if (ms < 1 || ms > 2147483647LL || !lua_isfunction(L, base + 1)) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid_argument");
    return 2;
  }
  lua_pushvalue(L, base + 1);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  const int id = (host != nullptr) ? host->addTimer(static_cast<uint32_t>(ms), ref) : -1;
  if (id < 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_pushnil(L);
    lua_pushliteral(L, "limit_exceeded");
    return 2;
  }
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, id);
  lua_setfield(L, -2, "id");
  lua_pushcfunction(L, timer_cancel);
  lua_setfield(L, -2, "cancel");
  return 1;
}

int ctx_set_tick_rate(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;  // colon-call aware
  // XTApp rates: ms int, "idle" (tick only when nothing to draw), "off" (stop).
  if (lua_isstring(L, base)) {
    const char* rate = lua_tostring(L, base);
    // Documented fixed levels (approx): realtime 33, high 50, normal 100,
    // low 500, idle 2000 ms. Unknown names fall back to normal, as the original
    // logs them ("set_tick_rate('%s') unknown level, fallback to normal").
    uint32_t ms = 100;
    if (strcmp(rate, "realtime") == 0) {
      ms = 33;
    } else if (strcmp(rate, "high") == 0) {
      ms = 50;
    } else if (strcmp(rate, "normal") == 0) {
      ms = 100;
    } else if (strcmp(rate, "low") == 0) {
      ms = 500;
    } else if (strcmp(rate, "idle") == 0) {
      ms = 2000;
    } else if (strcmp(rate, "off") == 0) {
      ms = 0;
    } else {
      LOG_ERR("LUAHOST", " set_tick_rate('%s') unknown level, fallback to normal", rate);
    }
    host->setTickIntervalMs(ms);
    return 0;
  }
  lua_Integer ms = luaL_checkinteger(L, base);
  if (ms < TICK_MIN_MS) ms = TICK_MIN_MS;
  host->setTickIntervalMs(static_cast<uint32_t>(ms));
  return 0;
}

int ctx_quit(lua_State* L) {
  ScriptHost* host = hostOf(L);
  if (host != nullptr) {
    host->requestQuit();
    LOG_INF("LUAHOST", " ctx:quit() requested");
  }
  return 0;
}

int ctx_request_refresh(lua_State* L) {
  // "solo una intencion; el sistema sigue eligiendo la forma de onda": taken as a
  // repaint request, not as a refresh-mode decision.
  ScriptHost* host = hostOf(L);
  if (host != nullptr) host->requestInvalidate();
  return 0;
}

int ctx_save(lua_State* L) {
  ScriptHost* host = hostOf(L);
  if (host != nullptr) host->saveState();
  lua_pushboolean(L, host != nullptr ? 1 : 0);
  return 1;
}

const luaL_Reg ctxFuncs[] = {
    {"invalidate", ctx_invalidate},
    {"set_tick_rate", ctx_set_tick_rate},
    {"set_tick", ctx_set_tick},
    {"after", ctx_after},
    {"defer_full_refresh", ctx_defer_full_refresh},
    {"quit", ctx_quit},
    {"request_refresh", ctx_request_refresh},
    {"save", ctx_save},
    {nullptr, nullptr},
};

// --- ctx.state <-> state.json --------------------------------------------
//
// ctx.state is a flat table the app mutates freely; ScriptHost serializes the
// scalar subset (string / integer / number / boolean) into
// /apps/<AppName>/state.json with ArduinoJson and restores it on load.
// Nested tables and functions are skipped (documented limitation).

// ctx.state <-> state.json. BOTH directions recurse through nested tables:
// XTApp state is arbitrary JSON-ish data (the vault keeps `passcode` = an array
// of key names and `vault` = an array of {label, secret} objects), so a
// scalars-only pass silently loses them — the app then starts with
// has_pass = true and an EMPTY passcode, which "unlocks" on an empty combo and
// never restores a saved one.
constexpr int STATE_MAX_DEPTH = 6;
constexpr int STATE_MAX_NODES = 600;

void restoreStateValue(lua_State* L, JsonVariantConst v, int depth) {
  if (depth > STATE_MAX_DEPTH) {
    lua_pushnil(L);
    return;
  }
  if (v.is<JsonObjectConst>()) {
    lua_newtable(L);
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      restoreStateValue(L, kv.value(), depth + 1);
      lua_setfield(L, -2, kv.key().c_str());
    }
  } else if (v.is<JsonArrayConst>()) {
    lua_newtable(L);
    int i = 1;
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
      restoreStateValue(L, e, depth + 1);
      lua_rawseti(L, -2, i++);
    }
  } else if (v.is<const char*>()) {
    lua_pushstring(L, v.as<const char*>());
  } else if (v.is<bool>()) {
    lua_pushboolean(L, v.as<bool>());
  } else if (v.is<long long>()) {
    lua_pushinteger(L, static_cast<lua_Integer>(v.as<long long>()));
  } else if (v.is<double>()) {
    lua_pushnumber(L, v.as<double>());
  } else {
    lua_pushnil(L);
  }
}

// Fills the state table at the top of the stack from a parsed state.json root.
void restoreStateFromJson(lua_State* L, JsonVariantConst doc) {
  if (!doc.is<JsonObjectConst>()) return;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    restoreStateValue(L, kv.value(), 0);
    lua_setfield(L, -2, kv.key().c_str());
  }
}

void appendJsonString(std::string& out, const char* s, size_t len) {
  out += '"';
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          out += esc;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void appendJsonValue(lua_State* L, int idx, std::string& out, int depth, int* nodes) {
  if (*nodes > STATE_MAX_NODES || depth > STATE_MAX_DEPTH ||
      out.size() > MAX_STATE_SERIALIZED) {
    out += "null";
    return;
  }
  const int abs = lua_absindex(L, idx);
  switch (lua_type(L, abs)) {
    case LUA_TSTRING: {
      size_t len = 0;
      const char* s = lua_tolstring(L, abs, &len);
      appendJsonString(out, s, len);
      break;
    }
    case LUA_TNUMBER: {
      char buf[40];
      if (lua_isinteger(L, abs)) {
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(lua_tointeger(L, abs)));
      } else {
        snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(lua_tonumber(L, abs)));
      }
      out += buf;
      break;
    }
    case LUA_TBOOLEAN:
      out += lua_toboolean(L, abs) ? "true" : "false";
      break;
    case LUA_TTABLE: {
      ++(*nodes);
      const lua_Integer n = luaL_len(L, abs);
      bool isArray = n > 0;
      for (lua_Integer i = 1; i <= n && isArray; ++i) {
        if (lua_rawgeti(L, abs, i) == LUA_TNIL) isArray = false;  // hole: an object
        lua_pop(L, 1);
      }
      if (isArray) {
        out += '[';
        for (lua_Integer i = 1; i <= n; ++i) {
          if (i > 1) out += ',';
          lua_rawgeti(L, abs, i);
          appendJsonValue(L, -1, out, depth + 1, nodes);
          lua_pop(L, 1);
        }
        out += ']';
      } else {
        out += '{';
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, abs) != 0) {  // key at -2, value at -1
          if (!first) out += ',';
          first = false;
          lua_pushvalue(L, -2);  // copy the key: lua_tolstring may convert it
          size_t klen = 0;
          const char* k = lua_tolstring(L, -1, &klen);
          if (k == nullptr) {
            k = "";
            klen = 0;
          }
          appendJsonString(out, k, klen);
          lua_pop(L, 1);
          out += ':';
          appendJsonValue(L, -1, out, depth + 1, nodes);
          lua_pop(L, 1);  // pop value, keep key for lua_next
        }
        out += '}';
      }
      break;
    }
    default:
      out += "null";  // functions / userdata / threads
      break;
  }
}

// --- require: confined to the app root --------------------------------------

// Candidates tried for require(name), most specific first. Keeps working for
// both Studio layouts (domain/vault_app.lua) and hand-made flat bundles
// (vault_app.lua next to index.lua).
std::vector<std::string> requireCandidates(ScriptHost* host, const std::string& name) {
  std::string slashed = name;
  for (auto& c : slashed) {
    if (c == '.') c = '/';
  }
  std::string base = slashed;
  const size_t slash = base.find_last_of('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  const std::string root = host->appPath();
  std::vector<std::string> v;
  v.push_back(root + slashed + ".lua");
  if (base != slashed) v.push_back(root + base + ".lua");
  if (slashed != name) v.push_back(root + name + ".lua");
  v.push_back(root + slashed + "/init.lua");
  v.push_back(root + "project-files/" + slashed + ".lua");
  v.push_back(std::string("/apps/") + slashed + ".lua");
  return v;
}

// "index.lua domain main.lua ..." — the real contents of the app root, so a
// wrong SD layout is visible on the device screen instead of guessed at.
std::string appListing(const std::string& dir, size_t maxLen) {
  std::string out;
  const std::vector<String> files = Storage.listFiles(dir.c_str(), 40);
  for (const String& f : files) {
    std::string s(f.c_str());
    const size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.empty()) continue;
    out += s;
    out += ' ';
    if (out.size() >= maxLen) {
      out += "...";
      break;
    }
  }
  if (out.empty()) out = "(empty)";
  return out;
}

// Loader contract (loadlib.c: searcher_Lua/findloader): a searcher returns the
// COMPILED CHUNK as a function; require() then calls it with (name, loaderData)
// and takes its return value as the module. Returning the chunk from a wrapper
// callback instead made the module a function (index.lua: "attempt to index a
// function value (upvalue 'App')").
int searcherFromApp(lua_State* L) {
  // package.searchers entry: (chunk, loaderData) when found, (message) when not.
  ScriptHost* host = hostOf(L);
  const std::string name = luaL_checkstring(L, 1);
  if (name.empty() || name.front() == '/' || name.find("..") != std::string::npos) {
    lua_pushfstring(L, "module '%s' rejected: bad name", name.c_str());
    return 1;
  }
  const std::vector<std::string> cands = requireCandidates(host, name);
  std::string unreadable;
  for (const std::string& c : cands) {
    if (!Storage.exists(c.c_str())) continue;
    std::string src;
    if (!Storage.readFileToString("LuaHost", c, MAX_REQUIRE_BYTES, src)) {
      unreadable += " " + c;
      continue;
    }
    if (luaL_loadbufferx(L, src.data(), src.size(), ("@" + c).c_str(), nullptr) != LUA_OK) {
      return lua_error(L);  // syntax error inside the module: file:line is in the message
    }
    lua_pushstring(L, c.c_str());  // loaderData: 2nd arg the chunk receives
    return 2;                      // (chunk, loaderData)
  }
  // Not found: report every path tried plus what the app root really holds.
  std::string msg = "require('" + name + "') tried:";
  for (const std::string& c : cands) {
    msg += "\n  " + c;
  }
  if (!unreadable.empty()) msg += "\nunreadable:" + unreadable;
  msg += "\n" + host->appPath() + " -> " + appListing(host->appPath(), 260);
  lua_pushstring(L, msg.c_str());
  return 1;  // single string = searcher error message for this path
}

}  // namespace

// --- XTApp assets: preload_assets (manifest.json) + XIC decode --------------

const uint8_t* ScriptHost::imageBits(const char* key) {
  for (auto& a : assets) {
    if (a.key == key) return a.bits;
  }
  return nullptr;
}

int ScriptHost::imageWidth(const char* key) {
  for (auto& a : assets) {
    if (a.key == key) return a.w;
  }
  return 0;
}

int ScriptHost::imageHeight(const char* key) {
  for (auto& a : assets) {
    if (a.key == key) return a.h;
  }
  return 0;
}

bool ScriptHost::preloadAssets() {
  assets.clear();
  std::string manifestPath = appPathV + "manifest.json";
  std::string manifest;
  if (!Storage.readFileToString("LuaHost", manifestPath, 16 * 1024, manifest)) {
    return false;  // Apps without assets are legal.
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifest);
  if (err) {
    LOG_ERR("LUAHOST", " manifest.json: %s", err.c_str());
    return false;
  }
  // Alongside the asset list, the manifest declares the app's permissions
  // (net.http is the only one today) and its read-only data files. ctx.data
  // validates names against that list, and ctx.net exists only with net.http.
  permissions.clear();
  dataFiles.clear();
  // api: "2.1" -> the 2.x event contract (see ScriptHost::api2x).
  const char* apiVer = doc["api"].as<const char*>();
  api2x = (apiVer != nullptr) && apiVer[0] == '2';
  for (JsonVariantConst v : doc["permissions"].as<JsonArrayConst>()) {
    if (v.is<const char*>()) permissions.push_back(v.as<const char*>());
  }
  for (JsonVariantConst v : doc["data_files"].as<JsonArrayConst>()) {
    const char* name = v["name"].as<const char*>();
    if (name != nullptr) dataFiles.push_back(name);
  }
  const char* listKeys[] = {"preload_assets"};
  for (const char* listKey : listKeys) {
    JsonArrayConst list = doc[listKey].as<JsonArrayConst>();
    for (JsonVariantConst v : list) {
      std::string key = v.as<std::string>();
      // XTApp Studio writes assets under assets/<key>.xic (flat), and older
      // layouts used assets/xic/<key>.xic — try both. Each preloaded key also
      // pulls its "<key>_matte" companion when present: Studio exports the
      // matte as a separate XIC but never lists it in preload_assets (apps
      // draw g:image("<key>_matte", x, y, {color = 0}) to lay the white
      // under-layer first), so loading only the listed keys leaves the matte
      // missing at draw time.
      for (const std::string& k : {key, key + "_matte"}) {
        std::string paths[2] = {appPathV + "assets/" + k + ".xic",
                                appPathV + "assets/xic/" + k + ".xic"};
        for (const std::string& path : paths) {
          std::string raw;
          if (!Storage.readFileToString("LuaHost", path, 256 * 1024, raw)) continue;
          if (raw.size() < 17 || raw.compare(0, 3, "XIC") != 0) break;
          const uint8_t* hdr = reinterpret_cast<const uint8_t*>(raw.data());
          int w = hdr[4] | (hdr[5] << 8);
          int h = hdr[6] | (hdr[7] << 8);
          size_t need = static_cast<size_t>((w + 7) / 8) * h;
          if (w <= 0 || h <= 0 || raw.size() < 16 + need) break;
          // XIC layout: "XIC", u16 width, u16 height, flags — Studio writes a
          // 24-BYTE header. Reading such a file with a 16-byte header leaves 8
          // "trailing" bytes and shifts every payload row by 64 bits, so each
          // row starts with the previous row's tail: the art shows a wrapped
          // strip of its right edge along the left (the "looping background").
          // Derive the payload start from the file size instead of assuming.
          size_t headerBytes = raw.size() - need;
          if (headerBytes < 16 || headerBytes > 64) headerBytes = 16;
          uint8_t* bits = new uint8_t[need];
          memcpy(bits, raw.data() + headerBytes, need);
          assets.push_back(LoadedAsset{k, w, h, bits});
          LOG_INF("LUAHOST", " asset %s: %dx%d (%u B)", k.c_str(), w, h,
                  static_cast<unsigned>(need));
          break;
        }
      }
    }
  }
  return true;
}


// ---------------------------------------------------------------------------
// ScriptHost
// ---------------------------------------------------------------------------

ScriptHost::ScriptHost(GfxRenderer& renderer, HostCb cb) : renderer(renderer), cb(cb), gfx(renderer) {}


ScriptHost::~ScriptHost() {
  onUnload();
  // Release preloaded XIC asset buffers.
  for (auto& a : assets) {
    delete[] a.bits;
  }
  assets.clear();
}

bool ScriptHost::doFile(const char* relFile) {
  std::string path = appPathV + relFile;
  ReadCtx ctx;
  if (!Storage.openFileForRead("LuaHost", path, ctx.file)) return false;
  if (ctx.file.size() > MAX_SCRIPT_BYTES) {
    loadErr = "script too large: " + path;
    return false;
  }
  lua_settop(L, 0);
  if (lua_load(L, storageReader, &ctx, ("@" + path).c_str(), nullptr) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    loadErr = msg ? msg : "load failed";
    return false;
  }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    loadErr = msg ? msg : "exec failed";
    return false;
  }
  return true;
}


// --- extended-API members --------------------------------------------------

size_t ScriptHost::assetBytes() const {
  size_t total = 0;
  for (const LoadedAsset& a : assets) {
    total += static_cast<size_t>((a.w + 7) / 8) * static_cast<size_t>(a.h);
  }
  return total;
}

bool ScriptHost::evictAsset(const char* key) {
  if (key == nullptr) return false;
  for (size_t i = 0; i < assets.size(); ++i) {
    if (assets[i].key == key) {
      delete[] assets[i].bits;
      assets.erase(assets.begin() + static_cast<long>(i));
      LOG_INF("LUAHOST", " asset '%s' evicted", key);
      return true;
    }
  }
  return false;
}

// ctx.i18n:t(key) — lang/<locale>.tsv of the app, loaded once. Missing files or
// keys fall back to English and then to the key itself (the "readable fallback"
// the contract asks for).
const char* ScriptHost::i18nLookup(const char* key) {
  if (key == nullptr) return nullptr;
  if (i18nTable.empty() && i18nLoadedFor.empty()) {
    // zh-TW falls back to zh before en (contract section 8).
    std::string codes[3] = {localeCode, std::string() , std::string("en")};
    if (localeCode == "zh-TW") codes[1] = "zh";
    for (const std::string& code : codes) {
      if (code.empty() || i18nTable.size() > 0) continue;
      const std::string path = appPathV + "lang/" + code + ".tsv";
      if (!Storage.exists(path.c_str())) continue;
      const String content = Storage.readFile(path.c_str());
      size_t pos = 0;
      int entries = 0;
      while (pos < content.length() && entries < 400) {
        const size_t eol = content.indexOf('\n', pos);
        const size_t end = (eol == static_cast<size_t>(-1)) ? content.length() : eol;
        const size_t tab = content.indexOf('\t', pos);
        if (tab != static_cast<size_t>(-1) && tab < end) {
          std::string k = std::string(content.c_str() + pos, tab - pos);
          std::string v = std::string(content.c_str() + tab + 1, end - tab - 1);
          while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
          if (!k.empty() && k.size() <= 60 && v.size() <= 96) {
            i18nTable[k] = v;
            ++entries;
          }
        }
        if (eol == static_cast<size_t>(-1)) break;
        pos = eol + 1;
      }
      if (entries > 0) {
        i18nLoadedFor = code;
        LOG_INF("LUAHOST", " i18n: %d entries from lang/%s.tsv", entries, code.c_str());
      }
    }
  }
  const auto it = i18nTable.find(key);
  return (it == i18nTable.end()) ? nullptr : it->second.c_str();
}

// ctx:save() / on_unload: flatten ctx.state into state.json.
void ScriptHost::saveState() {
  if (L == nullptr) return;
  lua_getglobal(L, "ctx");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, -1, "state");
  if (lua_istable(L, -1)) {
    std::string out;
    int nodes = 0;
    appendJsonValue(L, -1, out, 0, &nodes);
    if (out.size() <= MAX_STATE_SERIALIZED) {
      Storage.writeFile((appPathV + "state.json").c_str(), String(out.c_str()));
    } else {
      LOG_ERR("LUAHOST", "state too large (%u B), not saved", static_cast<unsigned>(out.size()));
    }
  }
  lua_pop(L, 1);
  lua_pop(L, 1);
}

bool ScriptHost::hasPermission(const char* perm) const {
  if (perm == nullptr) return false;
  for (const std::string& p : permissions) {
    if (p == perm) return true;
  }
  return false;
}

int ScriptHost::netStart(const char* url, const char* body, const char* headers) {
  if (url == nullptr || url[0] == '\0') return -1;
  int slot = -1;
  for (int i = 0; i < MAX_NET_TASKS; ++i) {
    if (netTasks[i] == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    // Reclaim the oldest finished task rather than refusing a new request.
    for (int i = 0; i < MAX_NET_TASKS; ++i) {
      if (netTasks[i] != nullptr && netTasks[i]->done) {
        delete netTasks[i];
        netTasks[i] = nullptr;
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) return -1;
  NetTask* t = new (std::nothrow) NetTask();
  if (t == nullptr) return -1;
  t->url = url;
  if (body != nullptr) {
    const size_t n = strlen(body);
    t->reqBody.assign(body, n < MAX_NET_REQUEST ? n : MAX_NET_REQUEST);
  }
  if (headers != nullptr) t->headers = headers;
  t->started = true;
  netTasks[slot] = t;
  if (xTaskCreatePinnedToCore(netWorkerTask, "xtnet", 8192, t, 1, nullptr, tskNO_AFFINITY) != pdPASS) {
    netTasks[slot] = nullptr;
    delete t;
    return -1;
  }
  LOG_INF("LUAHOST", " net task %d: %s", slot, url);
  return slot;
}

bool ScriptHost::load(const std::string& name) {
  app = name;
  appPathV = std::string("/apps/") + name + "/";
  L = lua_newstate(luaAlloc, nullptr, 0);
  if (L == nullptr) {
    loadErr = "lua_newstate OOM";
    return false;
  }
  lua_atpanic(L, panicHandler);

  // Host pointer in the registry for bindings.
  lua_pushlightuserdata(L, this);
  lua_setfield(L, LUA_REGISTRYINDEX, "x4_host");

  // Standard libraries (no OS/IO on device).
  luaL_requiref(L, "base", luaopen_base, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "table", luaopen_table, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "string", luaopen_string, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "math", luaopen_math, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "utf8", luaopen_utf8, 1);
  lua_pop(L, 1);

  // Screen size (portrait logical coords).
  lua_pushinteger(L, renderer.getScreenWidth());
  lua_setfield(L, LUA_REGISTRYINDEX, "x4_screen_w");
  lua_pushinteger(L, renderer.getScreenHeight());
  lua_setfield(L, LUA_REGISTRYINDEX, "x4_screen_h");

  // g table.
  luaL_newlib(L, gFuncs);
  lua_setglobal(L, "g");

  registerXtappMetatables(L);


  // require confined to the app root (replace the default searchers).
  // Sandbox parity (and safety): the contract's sandbox has no io/os/dofile/
  // loadfile/load, so an app cannot read arbitrary SD files or compile code at
  // runtime. `print` stays as a debug aid (apps written for the device use
  // ctx.log; keeping print costs nothing and helps when bench-debugging), and
  // `package` stays because require() is what resolves our flat app bundles —
  // its searchers are confined to the app root.
  lua_pushnil(L);
  lua_setglobal(L, "dofile");
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_pushnil(L);
  lua_setglobal(L, "load");
  lua_pushnil(L);
  lua_setglobal(L, "io");
  lua_pushnil(L);
  lua_setglobal(L, "os");

  luaL_requiref(L, "package", luaopen_package, 1);
  lua_pop(L, 1);
  lua_getglobal(L, "package");
  if (lua_istable(L, -1)) {
    lua_newtable(L);                 // searchers vector
    lua_pushcfunction(L, searcherFromApp);
    lua_rawseti(L, -2, 1);           // [1] = app-root searcher
    lua_setfield(L, -2, "searchers");
  }
  lua_pop(L, 1);

  // ctx table (with state populated from state.json, then published).
  luaL_newlib(L, ctxFuncs);
  // XTApp contract: ctx.screen is a TABLE {width, height}.
  lua_getfield(L, LUA_REGISTRYINDEX, "x4_screen_w");
  lua_Integer sw = lua_tointeger(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, LUA_REGISTRYINDEX, "x4_screen_h");
  lua_Integer sh = lua_tointeger(L, -1);
  lua_pop(L, 1);
  lua_createtable(L, 0, 9);
  lua_pushinteger(L, sw);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, sh);
  lua_setfield(L, -2, "height");
  // Portrait-only firmware here, and both boards are 480x800 portrait.
  lua_pushstring(L, "portrait");
  lua_setfield(L, -2, "orientation");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "is_landscape");
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "is_portrait");
  lua_pushinteger(L, sw < sh ? sw : sh);
  lua_setfield(L, -2, "short_side");
  lua_pushinteger(L, sw < sh ? sh : sw);
  lua_setfield(L, -2, "long_side");
  // 2.x (section 3): occlusion is the band the shell covers over the app (zero
  // on both X4 profiles — the app frame is the whole panel) and gray_levels is
  // what an app's fast-refresh frame actually shows (2: 0..7 paper, 8..15 ink).
  lua_createtable(L, 0, 4);
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "top");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "right");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "bottom");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "left");
  lua_setfield(L, -2, "occlusion");
  lua_pushinteger(L, 2);
  lua_setfield(L, -2, "gray_levels");
  lua_setfield(L, -2, "screen");
  // ctx.state
  lua_newtable(L);
  std::string statePath = appPathV + "state.json";
  if (Storage.exists(statePath.c_str())) {
    String content = Storage.readFile(statePath.c_str());
    if (content.length() > 0) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, content);
      if (!err) restoreStateFromJson(L, doc.as<JsonVariantConst>());
    }
  }
  lua_setfield(L, -2, "state");

  // --- 2.x additions (contract api-2.1.3) ----------------------------------
  // ctx.api (section 19): this host implements the 2.1 contract.
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, 2);
  lua_setfield(L, -2, "major");
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "minor");
  lua_setfield(L, -2, "api");
  // ctx.capabilities (section 17): only what this firmware really has. Section
  // 17 lets a host report any entry false, so the ones whose namespaces are not
  // implemented yet (pattern, transforms, runtime TTF, ctx.ui, reader, dialog,
  // frontlight, image decode) are false and their namespaces stay absent — an
  // app that checks first degrades exactly as the contract prescribes.
  lua_createtable(L, 0, 8);
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "pattern");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "image_transform");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "ttf");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "ui");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "reader");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "dialog");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "frontlight");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "image_decode");
  lua_setfield(L, -2, "capabilities");
  // ctx.storage: private per-app files (section 9) + ctx.json (section 10).
  luaL_newlib(L, storageFuncs);
  lua_setfield(L, -2, "storage");
  luaL_newlib(L, jsonFuncs);
  pushJsonNull(L);
  lua_setfield(L, -2, "null");
  lua_setfield(L, -2, "json");
  // ctx.fonts (section 7): the three system faces, each with its real metrics.
  lua_newtable(L);
  if (fontIdSmall != 0) {
    pushFontObject(L, this, "small", fontIdSmall);
    lua_setfield(L, -2, "small");
  }
  if (fontIdMedium != 0) {
    pushFontObject(L, this, "medium", fontIdMedium);
    lua_setfield(L, -2, "medium");
  }
  if (fontIdLarge != 0) {
    pushFontObject(L, this, "large", fontIdLarge);
    lua_setfield(L, -2, "large");
  }
  lua_setfield(L, -2, "fonts");
  // ctx.package (section 8): the preloaded XIC images + the manifest's data files.
  luaL_newlib(L, packageFuncs);
  lua_setfield(L, -2, "package");

  // --- the rest of the 0.8 surface, as namespaces on ctx -------------------
  luaL_newlib(L, sysFuncs);
  lua_setfield(L, -2, "sys");
  luaL_newlib(L, logFuncs);
  lua_setfield(L, -2, "log");
  luaL_newlib(L, longtaskFuncs);
  lua_setfield(L, -2, "longtask");
  luaL_newlib(L, displayFuncs);
  lua_setfield(L, -2, "display");
  luaL_newlib(L, systemFuncs);
  lua_setfield(L, -2, "system");
  luaL_newlib(L, perfFuncs);
  lua_setfield(L, -2, "perf");
  luaL_newlib(L, gcFuncs);
  lua_setfield(L, -2, "gc");
  luaL_newlib(L, assetsFuncs);
  lua_setfield(L, -2, "assets");
  luaL_newlib(L, dataFuncs);
  lua_setfield(L, -2, "data");
  luaL_newlib(L, layersFuncs);
  lua_setfield(L, -2, "layers");
  // ctx.i18n: locale (UI language) + t(key) against the app's lang/<code>.tsv.
  lua_newtable(L);
  lua_pushstring(L, localeCode.c_str());
  lua_setfield(L, -2, "locale");
  luaL_newlib(L, i18nFuncs);
  lua_setfield(L, -2, "t");
  lua_setfield(L, -2, "i18n");
  // ctx.input: caps + capability probes.
  luaL_newlib(L, inputFuncs);
  lua_newtable(L);
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "touch");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "bluetooth");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "keyboard");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "gamepad");
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "keys");
  lua_setfield(L, -2, "caps");
  lua_setfield(L, -2, "input");
  // ctx.lock: documented fields exist; the lockscreen is not script-rendered on
  // this port, so flush_once() says so instead of pretending.
  luaL_newlib(L, lockFuncs);
  lua_pushstring(L, "none");
  lua_setfield(L, -2, "reason");
  lua_pushnil(L);
  lua_setfield(L, -2, "base_page");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "elapsed_sec");
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "interval_sec");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "clear_background");
  lua_setfield(L, -2, "lock");
  lua_setglobal(L, "ctx");

  // XTEink.SystemStatusProxy mirrors, as the firmware exposes them.
  lua_newtable(L);
  luaL_newlib(L, sysFuncs);
  lua_setfield(L, -2, "SystemStatusProxy");
  lua_setglobal(L, "XTEink");

  // Global convenience alias: scripts can call invalidate() directly.
  lua_getglobal(L, "ctx");
  lua_getfield(L, -1, "invalidate");
  lua_remove(L, -2);
  lua_setglobal(L, "invalidate");

  if (!doFile("main.lua") && !doFile("index.lua")) {
    if (loadErr.empty()) loadErr = "no main.lua or index.lua in " + appPathV;
    lua_close(L);
    L = nullptr;
    return false;
  }
  preloadAssets();
  // ctx.net exists only with the manifest's "net.http" permission, exactly as the
  // doc states, so it is published once the manifest has been parsed.
  if (hasPermission("net.http")) {
    lua_getglobal(L, "ctx");
    if (lua_istable(L, -1)) {
      luaL_newlib(L, netFuncs);
      lua_setfield(L, -2, "net");
    }
    lua_pop(L, 1);
    LOG_INF("LUAHOST", " ctx.net published (net.http declared)");
  }
  callHandler("on_load");
  LOG_INF("LUAHOST", " %s loaded (%s)", app.c_str(), loadErr.empty() ? "ok" : loadErr.c_str());
  return true;
}

// XTApp API contract: every lifecycle handler receives ctx as its first
// argument (on_load(ctx), on_enter(ctx), on_input(ctx, ev), on_draw(ctx, g)).
// Handlers that ignore the argument are unaffected: extra args are dropped.
bool ScriptHost::callHandler(const char* name) {
  if (L == nullptr) return false;
  lua_getglobal(L, name);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return false;
  }
  lua_getglobal(L, "ctx");
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    lastErrorV = msg ? msg : "(no message)";
    LOG_ERR("LUAHOST", " %s: %s", name, lastErrorV.c_str());
    requestInvalidate();
    lua_pop(L, 1);
    return false;
  }
  return true;
}

bool ScriptHost::hasHandler(const char* name) const {
  if (L == nullptr) return false;
  lua_getglobal(const_cast<lua_State*>(L), name);
  bool isFn = lua_isfunction(L, -1);
  lua_pop(L, 1);
  return isFn;
}

void ScriptHost::onEnter() {
  tickIntervalMs = 0;
  lastTickMs = millis();
  gfx.reset();
  clearInvalidate();
  callHandler("on_enter");
}

void ScriptHost::onLeave() { callHandler("on_leave"); }

// --- ctx:after timers (2.x contract, section 2) -----------------------------
int ScriptHost::addTimer(uint32_t delayMs, int funcRef) {
  for (int i = 0; i < MAX_TIMERS; ++i) {
    if (timers[i].ref < 0) {
      timers[i].dueMs = millis() + delayMs;
      timers[i].ref = funcRef;
      return i;
    }
  }
  return -1;
}

void ScriptHost::cancelTimer(int id) {
  if (id < 0 || id >= MAX_TIMERS || timers[id].ref < 0) return;
  if (L != nullptr) luaL_unref(L, LUA_REGISTRYINDEX, timers[id].ref);
  timers[id].ref = -1;
}

bool ScriptHost::pumpTimers(uint32_t nowMillis) {
  if (L == nullptr) return false;
  bool ran = false;
  for (int i = 0; i < MAX_TIMERS; ++i) {
    if (timers[i].ref < 0) continue;
    if (static_cast<int32_t>(nowMillis - timers[i].dueMs) < 0) continue;
    const int ref = timers[i].ref;
    // One-shot: the slot is freed BEFORE the call, so a callback that arms
    // another timer cannot collide with the one it is running inside.
    timers[i].ref = -1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_isfunction(L, -1)) {
      lua_getglobal(L, "ctx");
      if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        LOG_ERR("LUAHOST", " after(): %s", msg ? msg : "(no message)");
        lastErrorV = msg ? msg : "after() error";
        lua_pop(L, 1);
      }
      ran = true;
    } else {
      lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
  }
  return ran;
}

// --- text metrics for the 2.x text options ---------------------------------
int ScriptHost::textWidth(int fontId, const char* s) const {
  if (fontId == 0 || s == nullptr) return 0;
  return renderer.getTextWidth(fontId, s);
}

int ScriptHost::lineHeightOf(int fontId) const {
  return fontId == 0 ? 0 : renderer.getLineHeight(fontId);
}

int ScriptHost::ascenderOf(int fontId) const {
  return fontId == 0 ? 0 : renderer.getFontAscenderSize(fontId);
}

void ScriptHost::onUnload() {
  // Release HTTP tasks: a finished one is ours to delete, a running one is owned
  // by its worker (marking it cancelled makes the worker free it on completion)
  // so nothing keeps a pointer into this host.
  for (int i = 0; i < MAX_NET_TASKS; ++i) {
    if (netTasks[i] == nullptr) continue;
    if (netTasks[i]->done || !netTasks[i]->started) {
      delete netTasks[i];  // finished, or never picked up by a worker
    } else {
      netTasks[i]->cancelled = true;  // the worker frees it when it returns
    }
    netTasks[i] = nullptr;
  }
  if (L == nullptr) return;
  callHandler("on_unload");
  callHandler("on_leave");
  saveState();  // ctx.state -> state.json on the way out
  lua_close(L);
  L = nullptr;
}

bool ScriptHost::onInput(const LuaInputEvent& ev) {
  if (L == nullptr) return false;
  // Convenience alias: scripts may handle taps via on_touch(x, y) instead of
  // the raw on_input(ctx, ev) table form.
  if (ev.isTouch) {
    lua_getglobal(L, "on_touch");
    if (lua_isfunction(L, -1)) {
      lua_getglobal(L, "ctx");
      lua_pushinteger(L, ev.x);
      lua_pushinteger(L, ev.y);
      if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        LOG_ERR("LUAHOST", " on_touch: %s", msg ? msg : "(no message)");
        lua_pop(L, 1);
      }
      return true;
    }
    lua_pop(L, 1);
  }
  lua_getglobal(L, "on_input");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return false;
  }
  lua_getglobal(L, "ctx");
  lua_newtable(L);
  if (ev.isTouch) {
    // Section 19: a 2.x package sees ev.type == "pointer", a 0.x/1.x package the
    // name it was written against ("touch"). Everything else in the event is the
    // same table for both.
    lua_pushstring(L, api2x ? "pointer" : "touch");
    lua_setfield(L, -2, "type");
    const char* gesture = "tap";
    switch (ev.gesture) {
      case LuaGesture::DoubleTap: gesture = "double_tap"; break;
      case LuaGesture::Long: gesture = "long"; break;
      case LuaGesture::SwipeLeft: gesture = "swipe_left"; break;
      case LuaGesture::SwipeRight: gesture = "swipe_right"; break;
      case LuaGesture::SwipeUp: gesture = "swipe_up"; break;
      case LuaGesture::SwipeDown: gesture = "swipe_down"; break;
      case LuaGesture::Tap: break;
    }
    lua_pushstring(L, gesture);
    lua_setfield(L, -2, "gesture");
    lua_pushinteger(L, ev.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, ev.y);
    lua_setfield(L, -2, "y");
    // A hold fires while the finger is down; a tap/swipe is release-driven.
    lua_pushstring(L, ev.phaseUp ? "up" : "hold");
    lua_setfield(L, -2, "phase");
  } else {
    lua_pushliteral(L, "key");
    lua_setfield(L, -2, "type");
    // 2.x: phase down/up, one pair per press. `state` is the 0.8 field name and
    // stays populated so an old app reading it keeps working.
    lua_pushstring(L, ev.phaseUp ? "up" : "down");
    lua_setfield(L, -2, "phase");
    lua_pushliteral(L, "down");
    lua_setfield(L, -2, "state");
    // XTApp key model: a single ev.key string, not per-key booleans.
    const char* keyName = ev.keyBack  ? "back"
                          : ev.keyOk  ? "ok"
                          : ev.keyLeft ? "left"
                          : ev.keyRight ? "right"
                          : ev.keyUp  ? "up"
                          : "down";
    lua_pushstring(L, keyName);
    lua_setfield(L, -2, "key");
  }
  // ev.time_ms: monotonic delivery time (the contract exposes it for both kinds).
  lua_pushinteger(L, static_cast<lua_Integer>(ev.timeMs != 0 ? ev.timeMs : millis()));
  lua_setfield(L, -2, "time_ms");
  if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    LOG_ERR("LUAHOST", " on_input: %s", msg ? msg : "(no message)");
    lastErrorV = msg ? msg : "on_input error";
    lua_pop(L, 1);
    return false;
  }
  // The script's return value is the consumed flag (true = handled), which the
  // activity uses to decide whether Back should pop the screen.
  const bool consumed = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return consumed;
}


void ScriptHost::onDraw() {
  if (L == nullptr) return;
  gfx.reset();  // drain the previous frame's ops before queueing anew
  const uint32_t drawStartUs = micros();
  // on_draw receives the g table as its single argument (function on_draw(g)).
  lua_getglobal(L, "on_draw");
  if (lua_isfunction(L, -1)) {
    lua_getglobal(L, "ctx");
    lua_getglobal(L, "g");
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      lastErrorV = msg ? msg : "(no message)";
      LOG_ERR("LUAHOST", " on_draw: %s", lastErrorV.c_str());
      lua_pop(L, 1);
    }
  } else {
    lua_pop(L, 1);
  }
  // Metrics for ctx.perf:info(): capture the queue BEFORE apply(), which resets
  // it (reading operationCount() afterwards always shows 0).
  lastFrameOps = static_cast<uint32_t>(gfx.operationCount());
  lastFrameImages = static_cast<uint32_t>(gfx.imageCount());
  ++frameCount;
  // Flushing the queued frame must happen under RenderLock; the activity
  // calls onDraw from render(), so the lock is held here -> apply + flush.
  gfx.apply();
  lastDrawUs = micros() - drawStartUs;
}

bool ScriptHost::onTick(uint32_t nowMillis) {
  if (L == nullptr || tickIntervalMs == 0) return false;
  uint32_t elapsed = nowMillis - lastTickMs;
  if (elapsed < tickIntervalMs) return false;
  lastTickMs = nowMillis;
  // dt is the real elapsed time in ms, not the requested interval.
  lua_getglobal(L, "on_tick");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return false;
  }
  lua_pushinteger(L, elapsed);
  const uint32_t tickStartUs = micros();
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    LOG_ERR("LUAHOST", " on_tick: %s", msg ? msg : "(no message)");
    lua_pop(L, 1);
  }
  lastTickUs = micros() - tickStartUs;
  return true;
}
