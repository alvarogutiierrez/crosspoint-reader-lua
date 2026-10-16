#pragma once

#include <Arduino.h>

#include <map>
#include <string>
#include <vector>

#include "LuaGfx.h"

class GfxRenderer;
struct lua_State;

// Input event handed to on_input(ev). ev.type is 'key' (with key/back/ok/...
// booleans and state='down') or 'touch' (with x/y and a gesture name).
enum class LuaGesture : uint8_t {
  Tap,
  DoubleTap,
  Long,
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
};

struct LuaInputEvent {
  bool isTouch = false;
  int x = 0;
  int y = 0;
  // ev.time_ms: monotonic ms when the event was delivered (XTApp exposes it).
  uint32_t timeMs = 0;
  LuaGesture gesture = LuaGesture::Tap;
  // Key flags: exactly one is true for key events.
  bool keyBack = false;
  bool keyOk = false;
  bool keyLeft = false;
  bool keyRight = false;
  bool keyUp = false;
  bool keyDown = false;
};

// Embeds one Lua app: owns lua_State, loads main.lua, calls lifecycle
// callbacks, exposes ctx (screen/state/invalidate/tick rate) and the shared
// `g` drawing table. Reboot-safe: everything is heap-local, nothing leaks.
class ScriptHost {
 public:
  struct HostCb {
    // Request an activity-level redraw (invalidates the current frame).
    void (*invalidate)(void* user) = nullptr;
    void* user = nullptr;
  };

  ScriptHost(GfxRenderer& renderer, HostCb cb);
  ~ScriptHost();

  ScriptHost(const ScriptHost&) = delete;
  ScriptHost& operator=(const ScriptHost&) = delete;

  // Load and run /apps/<appName>/main.lua (or index.lua when main is absent).
  // Runs on_load afterwards. Returns false when no script could be read; the
  // caller then shows an error page without touching Lua again.
  bool load(const std::string& appName);
  bool loaded() const { return L != nullptr; }
  const std::string& lastError() const { return lastErrorV; }
  const std::string& appName() const { return app; }
  const std::string& appPath() const { return appPathV; }
  const std::string& loadError() const { return loadErr; }

  // Lifecycle: on_enter/on_input/on_draw/on_tick/on_leave/on_unload.
  void onEnter();
  void onLeave();
  void onUnload();
  // Forwards one event to on_input; returns true when the script consumed it
  // (the contract's return value), so Back can pop the activity only when the
  // app did not handle it itself (ctx:quit() is the app's own way out).
  bool onInput(const LuaInputEvent& ev);
  // on_draw drains g ops into the renderer (not under RenderLock: the caller
  // wraps it).
  void onDraw();
  // Runs on_tick when due (rate set by ctx:set_tick_rate, default 0 = never).
  // Returns true when on_tick actually ran.
  bool onTick(uint32_t nowMillis);

  // ctx:quit() — the activity polls this and pops itself.
  void requestQuit() { quitFlag = true; }
  bool quitRequested() const { return quitFlag; }
  void clearQuitRequest() { quitFlag = false; }
  // ctx.display:full_refresh() — promote the next panel refresh to a full one.
  void requestFullRefresh() { fullRefreshFlag = true; }
  bool wantsFullRefresh() const { return fullRefreshFlag; }
  void clearFullRefresh() { fullRefreshFlag = false; }
  // ctx.display:defer_auto_full(ms) — deadline (millis) before an automatic full
  // refresh may run again; the activity compares it, 0 = not deferred.
  uint32_t deferAutoFullUntil = 0;
  // ctx:save() (and on_unload) — flatten ctx.state into state.json now.
  void saveState();
  void setLastError(const std::string& e) { lastErrorV = e; }
  // ctx.longtask:start()/feed()/stop() state (our port never aborts callbacks).
  bool longTaskActive = false;
  // ctx.assets:evict(key) — drop a preloaded asset.
  bool evictAsset(const char* key);
  // Set by the activity: UI language code for ctx.i18n (the lib cannot reach
  // CrossPointSettings, same reason the font id is injected).
  void setLocaleCode(const char* code) { localeCode = code != nullptr ? code : "en"; }

  // ctx.perf:info() + ctx.assets:info() counters (real numbers, not Studio stubs).
  uint32_t frameCount = 0;
  uint32_t lastFrameOps = 0;
  uint32_t lastFrameImages = 0;
  uint32_t assetHits = 0;
  uint32_t assetMisses = 0;
  uint32_t lastTickUs = 0;
  uint32_t lastDrawUs = 0;
  void noteAssetHit() { ++assetHits; }
  void noteAssetMiss() { ++assetMisses; }
  size_t assetBytes() const;
  int assetCount() const { return static_cast<int>(assets.size()); }
  // i18n: locale code of the UI language + the app's own lang/<code>.tsv cache.
  std::string localeCode;
  const char* i18nLookup(const char* key);

  bool wantsInvalidate() const { return invalidateFlag; }
  void clearInvalidate() { invalidateFlag = false; }
  void requestInvalidate() { invalidateFlag = true; }

  // Font used by g:text / g:print. The lib cannot see the app's fontIds.h, so
  // the activity injects the UI font id at construction (0 = none: text is
  // skipped, which is what silently hid every label before).
  void setDefaultFont(int id) { defaultFontId = id; }
  int defaultFontId = 0;

  // ctx:set_tick_rate(ms) callable target.
  void setTickIntervalMs(uint32_t ms) { tickIntervalMs = ms; }
  uint32_t tickMs() const { return tickIntervalMs; }

  // Anonymous `g` / ctx binding callbacks in ScriptHost.cpp manipulate this
  // queue directly.
  LuaGfx gfx;

  // XTApp asset registry: XIC files preloaded from the manifest's
  // preload_assets at load(). Public so the g:image binding can look up + draw.
  struct LoadedAsset {
    std::string key;
    int w = 0;
    int h = 0;
    uint8_t* bits = nullptr;  // 1bpp payload, row-major MSB-first, bit1 = ink
  };
  const uint8_t* imageBits(const char* key);
  int imageWidth(const char* key);
  int imageHeight(const char* key);

 private:
  bool preloadAssets();  // parses manifest.json preload_assets
  bool doFile(const char* relFile);
  bool callHandler(const char* name);  // passes ctx as arg 1
  bool hasHandler(const char* name) const;

  GfxRenderer& renderer;
  HostCb cb;
  lua_State* L = nullptr;
  std::string app;      // app dir name
  std::string appPathV;  // /apps/<name>/ absolute
  std::string loadErr;
  uint32_t tickIntervalMs = 0;
  uint32_t lastTickMs = 0;
  bool invalidateFlag = false;
  std::string lastErrorV;  // last Lua runtime error, for on-screen surfaces
  bool quitFlag = false;
  bool fullRefreshFlag = false;
  std::vector<LoadedAsset> assets;
  // App i18n: lang/<locale>.tsv parsed once on first ctx.i18n:t().
  std::map<std::string, std::string> i18nTable;
  std::string i18nLoadedFor;
  // manifest.json extras: permissions (ctx.net gating) + declared data_files.
  std::vector<std::string> permissions;
  std::vector<std::string> dataFiles;


 private:
  friend class LuaBindings;
};
