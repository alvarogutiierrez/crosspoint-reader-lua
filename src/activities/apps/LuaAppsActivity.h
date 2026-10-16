#pragma once

#include <Logging.h>

#include <vector>

#include "../Activity.h"
#include "../ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Lua apps browser: lists the app directories under /apps/ and launches one in
// LuaAppActivity. Direct scan only — deliberately does NOT touch
// PluginLocations or the plugin store.
class LuaAppsActivity : public Activity {
 public:
  LuaAppsActivity(ActivityManager& manager, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LuaApps", renderer, mappedInput), manager(manager) {}

  void onEnter() override {
    apps = scanApps();
    selectorIndex = 0;
    requestUpdate();
  }

  void loop() override {
    if (apps.empty()) {
      // Only Back (or Home, handled globally) leaves the empty listing.
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        Activity::finish();
        return;
      }
    } else {
      navigateButtons();
      dispatchTouch();
    }
  }

  void render(RenderLock&&) override {
    const auto& metrics = UITheme::getInstance().getMetrics();

    renderer.clearScreen();
    // Standard header band geometry (same as Settings/Library): y starts at
    // topPadding so the status strip isn't clipped against the bezel.
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                   tr(STR_APPS));

    if (apps.empty()) {
      UITheme::drawCenteredText(renderer, UITheme::getContentArea(renderer), UI_12_FONT_ID,
                                static_cast<int>(metrics.headerHeight) + 40, tr(STR_NO_LUA_APPS));
      renderer.displayBuffer();
      return;
    }

    const Rect content = UITheme::getContentArea(renderer);
    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const Rect listRect{content.x, listTop, content.width,
                        content.height - (listTop - content.y) - metrics.buttonHintsHeight};
    GUI.drawButtonMenu(renderer, listRect, static_cast<int>(apps.size()), selectorIndex,
                       [this](int index) { return apps[index].name; }, [](int) { return UIIcon::None; });
    renderer.displayBuffer();
  }

 protected:
  struct LuaApp {
    std::string name;
  };

  // List directories under /apps/ containing main.lua or index.lua.
  static std::vector<LuaApp> scanApps() {
    std::vector<LuaApp> result;
    if (!Storage.exists("/apps")) return result;
    HalFile dir = Storage.open("/apps");
    if (!dir.isOpen() || !dir.isDirectory()) return result;
    dir.rewindDirectory();
    while (HalFile entry = dir.openNextFile()) {
      if (!entry.isDirectory()) continue;
      char nameBuf[128];
      if (entry.getName(nameBuf, sizeof(nameBuf)) == 0) continue;
      const std::string base = std::string("/apps/") + nameBuf;
      if (Storage.exists((base + "/main.lua").c_str()) || Storage.exists((base + "/index.lua").c_str())) {
        result.push_back({nameBuf});
      }
    }
    return result;
  }

 private:
  // Row geometry shared by render() and the touch hit-test; drawButtonMenu
  // walks rows with the theme's menuRowHeight + menuSpacing stride.
  int listTop() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  }

  void dispatchTouch() {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect content = UITheme::getContentArea(renderer);
    int row = -1;
    const auto touch = mappedInput.rowTouch(row, listTop(), metrics.menuRowHeight + metrics.menuSpacing,
                                            static_cast<int>(apps.size()), 0, INT32_MAX, metrics.menuRowHeight);
    if (touch == MappedInputManager::RowTouch::None) return;
    if (touch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != row) {
        selectorIndex = row;
        requestUpdate();
      }
      return;
    }
    // Tap release on a row: select + activate.
    selectorIndex = row;
    manager.goToApp(apps[selectorIndex].name);
  }

  void navigateButtons() {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (selectorIndex > 0) {
        selectorIndex--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (selectorIndex < static_cast<int>(apps.size()) - 1) {
        selectorIndex++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      manager.goToApp(apps[selectorIndex].name);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      Activity::finish();
    }
  }

  ActivityManager& manager;
  std::vector<LuaApp> apps;
  int selectorIndex = 0;
};
