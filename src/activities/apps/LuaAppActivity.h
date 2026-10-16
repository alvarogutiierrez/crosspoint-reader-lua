#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include <Logging.h>

#include <memory>
#include <string>

#include "../Activity.h"
#include "../ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include <Memory.h>
#include <ScriptHost.h>

// Runs a single Lua app from /apps/<appName>/ without Ronald's plugin
// machinery. Owns the lifecycle: ScriptHost load/enter on open, key/touch
// events forwarded to on_input, invalidation-driven on_draw frames under
// RenderLock, ctx:set_tick_rate pacing for on_tick, and on_leave/on_unload
// (state.json flush) on Back.
class LuaAppActivity : public Activity {
 public:
  LuaAppActivity(ActivityManager& manager, GfxRenderer& renderer, MappedInputManager& mappedInput, std::string appName)
      : Activity("LuaApp", renderer, mappedInput), manager(manager), appName(std::move(appName)) {
    host = makeUniqueNoThrow<ScriptHost>(renderer, ScriptHost::HostCb{
        // ScriptHost runs on the main loop task; an invalidate here must not
        // race the render task. requestUpdate() is atomic and safe.
        .invalidate = [](void* user) { static_cast<Activity*>(user)->requestUpdate(); },
        .user = static_cast<void*>(this)});
    // LuaGfx queues text with a font id the renderer resolves through its font
    // map: without a real id the texts are dropped silently. Apps lay out
    // against a 12px-advance monospace metric, so APPMONO_10 (12.5px advance)
    // is their font — UI_12 is proportional and ~2x taller, which made every
    // label oversized and threw the apps' own centring off.
    if (host != nullptr) host->setDefaultFont(APPMONO_10_FONT_ID);
  }

  void onEnter() override {
    if (host == nullptr) {
      LOG_ERR("LUAAPP", "OOM: script host");
      loadFailed = true;
      requestUpdate();
      return;
    }
    if (!host->load(appName)) {
      loadErr = host->loadError();
      loadFailed = true;
      LOG_ERR("LUAAPP", "load failed: %s", loadErr.c_str());
    } else {
      host->onEnter();  // on_enter runs before the first on_draw frame
      lastTickMs = millis();
    }
    requestUpdate();
  }

  void onExit() override {
    // on_leave before unload so the script can save transient UI state; the
    // destructor then flushes state.json and closes the lua_State.
    if (host != nullptr && host->loaded()) host->onLeave();
  }

  void loop() override;

  void render(RenderLock&&) override {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int top = static_cast<int>(metrics.topPadding);
    // Capture the script's frame before anything else can reset the count.
    if (loadFailed) {
      drawWrapped(UI_12_FONT_ID, 8, top + 16, 6, renderer.getScreenWidth() - 16, loadErr);
    } else if (host != nullptr && host->loaded()) {
      host->onDraw();
    }
    // DEBUG strip (remove when the Lua app pipeline is confirmed healthy):
    // logical resolution + queued ops + the app dir actually being used.
    char dbg[96];
    snprintf(dbg, sizeof(dbg), "DBG %dx%d ops=%d %s", renderer.getScreenWidth(), renderer.getScreenHeight(),
             host != nullptr ? host->gfx.operationCount() : -1, appName.c_str());
    renderer.drawText(UI_10_FONT_ID, 8, top, dbg, true);
    if (host != nullptr) {
      const std::string& luaErr = host->lastError();
      if (!luaErr.empty()) {
        drawWrapped(UI_10_FONT_ID, 8, top + 12, 14, renderer.getScreenWidth() - 16, luaErr);
      }
    }
    renderer.displayBuffer();
  }

 protected:
  // Char-width-budgeted wrapping: on device there is no serial console, so the
  // only way to read a long Lua error is to print it across several lines.
  void drawWrapped(int fontId, int x, int top, int maxLines, int maxWidth, const std::string& text) {
    if (text.empty()) return;
    const int lineH = renderer.getLineHeight(fontId);
    int y = top;
    size_t i = 0;
    for (int n = 0; n < maxLines && i < text.size(); ++n) {
      size_t end = i;
      size_t lastSpace = std::string::npos;
      while (end < text.size() && text[end] != '\n') {
        if (text[end] == ' ') lastSpace = end;
        const std::string probe = text.substr(i, end - i + 1);
        if (end > i && renderer.getTextWidth(fontId, probe.c_str()) > maxWidth) break;
        ++end;
      }
      if (end < text.size() && text[end] != '\n' && lastSpace != std::string::npos && lastSpace > i) {
        end = lastSpace;  // wrap on the last whole word
      }
      const std::string line = text.substr(i, end - i);
      if (!line.empty()) renderer.drawText(fontId, x, y, line.c_str(), true);
      y += lineH;
      i = (end < text.size() && (text[end] == '\n' || text[end] == ' ')) ? end + 1 : end;
      if (i >= text.size()) break;
    }
  }

  bool dispatchInput();
  void runTick(uint32_t nowMs);

  // Input-coalescing window: keystrokes closer than this fold into one frame.
  static constexpr uint32_t kInputSettleMs = 50;

  ActivityManager& manager;
  std::unique_ptr<ScriptHost> host;
  std::string appName;
  std::string loadErr;
  bool loadFailed = false;
  uint32_t lastTickMs = 0;
  uint32_t lastInputMs = 0;
};
