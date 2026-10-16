#include "LuaAppActivity.h"

// Loop task only: events in, Lua callbacks out. All drawing happens in
// render() under the render task's RenderLock.

bool LuaAppActivity::dispatchInput() {
  if (host == nullptr || !host->loaded()) return false;

  int tx = 0;
  int ty = 0;
  LuaInputEvent ev;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    ev.isTouch = true;
    ev.x = tx;
    ev.y = ty;
    host->onInput(ev);
    return true;
  }
  ev.isTouch = false;
  ev.keyBack = mappedInput.wasPressed(MappedInputManager::Button::Back);
  ev.keyOk = mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  ev.keyLeft = mappedInput.wasPressed(MappedInputManager::Button::Left);
  ev.keyRight = mappedInput.wasPressed(MappedInputManager::Button::Right);
  ev.keyUp = mappedInput.wasPressed(MappedInputManager::Button::Up);
  ev.keyDown = mappedInput.wasPressed(MappedInputManager::Button::Down);
  if (ev.keyBack || ev.keyOk || ev.keyLeft || ev.keyRight || ev.keyUp || ev.keyDown) {
    host->onInput(ev);
    return true;
  }
  return false;
}

void LuaAppActivity::runTick(const uint32_t nowMs) {
  if (host == nullptr) return;
  if (host->onTick(nowMs)) {
    lastTickMs = nowMs;
    if (host->wantsInvalidate()) requestUpdate();
  }
}

void LuaAppActivity::loop() {
  if (loadFailed) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      Activity::finish();
    }
    return;
  }
  if (host == nullptr || !host->loaded()) return;

  // Back exists the app from anywhere: forward it as on_input too, then pop.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    LuaInputEvent ev;
    ev.keyBack = true;
    host->onInput(ev);
    manager.popActivity();
    return;
  }

  const uint32_t now = millis();
  if (dispatchInput()) lastInputMs = now;
  runTick(now);

  // One e-ink frame costs a whole waveform (hundreds of ms), so a burst of key
  // presses must collapse into ONE refresh instead of queueing one per press:
  // hold the frame until input has been quiet for a moment. Adds ~50ms to a
  // lone press — invisible next to the panel's own refresh time.
  if (host->wantsInvalidate() && (now - lastInputMs) >= kInputSettleMs) {
    host->clearInvalidate();
    requestUpdate();
  }
}
