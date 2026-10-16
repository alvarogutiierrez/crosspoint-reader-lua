#include "LuaCApi.h"
#include "ScriptHost.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "HalStorage.h"

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
constexpr uint32_t TICK_MIN_MS = 50;

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

// XTApp color model: gray level 0..15 (0 = white, 15 = black). Our 1bpp panel
// renders any nonzero level as ink.
static bool gfxInkOfColor(lua_State* L, int idx) {
  return !lua_isnumber(L, idx) || lua_tointeger(L, idx) != 0;
}

int g_clear(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  bool white = lua_isnumber(L, base) && lua_tointeger(L, base) == 0;
  host->gfx.addClear(!white);
  return 0;
}

int g_text(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:text(x, y, str[, {color = level}])
  int x = static_cast<int>(luaL_checkinteger(L, base));
  int y = static_cast<int>(luaL_checkinteger(L, base + 1));
  const char* s = luaL_checkstring(L, base + 2);
  lua_Integer color = 15;  // default: black
  if (lua_istable(L, base + 3)) {
    lua_getfield(L, base + 3, "color");
    if (lua_isnumber(L, -1)) color = lua_tointeger(L, -1);
    lua_pop(L, 1);
  } else if (lua_isnumber(L, base + 3)) {
    color = lua_tointeger(L, base + 3);
  }
  // Text queueing needs a font that exists in the renderer's font map; without
  // one drawText() logs "Font N not found" and draws nothing.
  if (host->defaultFontId == 0) {
    LOG_ERR("LUAHOST", "g:text called with no default font set; text dropped");
    return 0;
  }
  host->gfx.addText(host->defaultFontId, x, y, s, color != 0);
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
  const char* mode = "stroke";
  lua_Integer color = 15;
  if (lua_isstring(L, base + 4)) {
    mode = lua_tostring(L, base + 4);
  } else if (lua_isnumber(L, base + 4)) {
    color = lua_tointeger(L, base + 4);
  }
  if (lua_isnumber(L, base + 5)) color = lua_tointeger(L, base + 5);
  bool filled = strcmp(mode, "fill") == 0;
  host->gfx.addRect(filled ? LuaGfx::Kind::FillRect : LuaGfx::Kind::DrawRect, x, y, w, h, 1,
                    color != 0);
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
  lua_Integer color = 15;
  if (lua_isnumber(L, base + 4)) color = lua_tointeger(L, base + 4);
  host->gfx.addLine(x1, y1, x2, y2, color != 0);
  return 0;
}

int g_circle(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:circle(cx, cy, r[, "fill"|"stroke"][, color])
  int cx = static_cast<int>(luaL_checkinteger(L, base));
  int cy = static_cast<int>(luaL_checkinteger(L, base + 1));
  int r = static_cast<int>(luaL_checkinteger(L, base + 2));
  const char* mode = "stroke";
  lua_Integer color = 15;
  if (lua_isstring(L, base + 3)) {
    mode = lua_tostring(L, base + 3);
  } else if (lua_isnumber(L, base + 3)) {
    color = lua_tointeger(L, base + 3);
  }
  if (lua_isnumber(L, base + 4)) color = lua_tointeger(L, base + 4);
  bool filled = strcmp(mode, "fill") == 0;
  host->gfx.addCircle(filled ? LuaGfx::Kind::FillCircle : LuaGfx::Kind::DrawCircle, cx, cy, r,
                      color != 0);
  return 0;
}

int g_image(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = gfxArgBase(L);
  // XTApp signature: g:image(assetKey, x, y[, {color = level}]). Assets come
  // from preload_assets (manifest.json) — XIC files preloaded at app load.
  const char* key = luaL_checkstring(L, base);
  int x = static_cast<int>(luaL_checkinteger(L, base + 1));
  int y = static_cast<int>(luaL_checkinteger(L, base + 2));
  lua_Integer color = 15;
  if (lua_istable(L, base + 3)) {
    lua_getfield(L, base + 3, "color");
    if (lua_isnumber(L, -1)) color = lua_tointeger(L, -1);
    lua_pop(L, 1);
  } else if (lua_isnumber(L, base + 3)) {
    color = lua_tointeger(L, base + 3);
  }
  const uint8_t* bits = host->imageBits(key);
  if (bits == nullptr) {
    return luaL_error(L, "g:image: asset '%s' not preloaded (preload_assets?)", key);
  }
  if (color == 0) {
    // Tint "0" (white): the matte layer. Lay down a white fill of the same
    // footprint so subsequent art composites on clean paper.
    host->gfx.addRect(LuaGfx::Kind::FillRect, x, y, host->imageWidth(key), host->imageHeight(key), 1, false);
    return 0;
  }
  host->gfx.addImage(bits, host->imageWidth(key), host->imageHeight(key), x, y);
  return 0;
}

const luaL_Reg gFuncs[] = {
    {"clear", g_clear}, {"text", g_text}, {"rect", g_rect},
    {"line", g_line},   {"circle", g_circle}, {"image", g_image},
    {nullptr, nullptr},
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
  hostOf(L)->requestInvalidate();
  return 0;
}

int ctx_set_tick_rate(lua_State* L) {
  ScriptHost* host = hostOf(L);
  const int base = lua_type(L, 1) == LUA_TTABLE ? 2 : 1;  // colon-call aware
  // XTApp rates: ms int, "idle" (tick only when nothing to draw), "off" (stop).
  if (lua_isstring(L, base)) {
    const char* rate = lua_tostring(L, base);
    if (strcmp(rate, "idle") == 0) host->setTickIntervalMs(TICK_MIN_MS);
    if (strcmp(rate, "off") == 0) host->setTickIntervalMs(0);
    return 0;
  }
  lua_Integer ms = luaL_checkinteger(L, base);
  if (ms < TICK_MIN_MS) ms = TICK_MIN_MS;
  host->setTickIntervalMs(static_cast<uint32_t>(ms));
  return 0;
}

const luaL_Reg ctxFuncs[] = {
    {"invalidate", ctx_invalidate},
    {"set_tick_rate", ctx_set_tick_rate},
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


  // require confined to the app root (replace the default searchers).
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
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, sw);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, sh);
  lua_setfield(L, -2, "height");
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
  lua_setglobal(L, "ctx");

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

void ScriptHost::onUnload() {
  if (L == nullptr) return;
  callHandler("on_unload");
  callHandler("on_leave");
  // Flatten ctx.state into state.json on the way out.
  lua_getglobal(L, "ctx");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "state");
    if (lua_istable(L, -1)) {
      {
        std::string out;
        int nodes = 0;
        appendJsonValue(L, -1, out, 0, &nodes);
        if (out.size() <= MAX_STATE_SERIALIZED) {
          std::string statePath = appPathV + "state.json";
          Storage.writeFile(statePath.c_str(), String(out.c_str()));
        } else {
          LOG_ERR("LUAHOST", "state too large (%u B), not saved", static_cast<unsigned>(out.size()));
        }
      }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  lua_close(L);
  L = nullptr;
}

void ScriptHost::onInput(const LuaInputEvent& ev) {
  if (L == nullptr) return;
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
      return;
    }
    lua_pop(L, 1);
  }
  lua_getglobal(L, "on_input");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_getglobal(L, "ctx");
  lua_newtable(L);
  if (ev.isTouch) {
    lua_pushliteral(L, "touch");
    lua_setfield(L, -2, "type");
    lua_pushliteral(L, "tap");
    lua_setfield(L, -2, "gesture");
    lua_pushinteger(L, ev.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, ev.y);
    lua_setfield(L, -2, "y");
  } else {
    lua_pushliteral(L, "key");
    lua_setfield(L, -2, "type");
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
  if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    LOG_ERR("LUAHOST", " on_input: %s", msg ? msg : "(no message)");
    lua_pop(L, 1);
    return;
  }
  // Returning true from on_input marks the event consumed; fallthrough returns
  // nil/false so activities can ignore the result for now.
  lua_pop(L, 1);
}


void ScriptHost::onDraw() {
  if (L == nullptr) return;
  gfx.reset();  // drain the previous frame's ops before queueing anew
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
  // Flushing the queued frame must happen under RenderLock; the activity
  // calls onDraw from render(), so the lock is held here -> apply + flush.
  gfx.apply();
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
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    LOG_ERR("LUAHOST", " on_tick: %s", msg ? msg : "(no message)");
    lua_pop(L, 1);
  }
  return true;
}
