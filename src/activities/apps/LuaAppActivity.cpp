#include "LuaAppActivity.h"

#include <CrossPointSettings.h>
#include <HalDisplay.h>
#include <I18nKeys.h>

// Loop task only: events in, Lua callbacks out. All drawing happens in
// render() under the render task's RenderLock.



bool LuaAppActivity::dispatchInput() {
  if (host == nullptr || !host->loaded()) return false;

  const uint32_t now = millis();
  int tx = 0;
  int ty = 0;
  LuaInputEvent ev;
  ev.timeMs = now;

  // Touch classifier, in the SDK's own order: a long press is a one-shot event
  // fired while the finger is down (consuming it suppresses the rest of the
  // contact), swipes are direction events, and a release inside the screen is a
  // tap. Apps branch on all of them (gesture == "long" toggles notes, etc.).
  if (mappedInput.wasScreenLongPress(tx, ty)) {
    ev.isTouch = true;
    ev.gesture = LuaGesture::Long;
    ev.x = tx;
    ev.y = ty;
    lastTouchX = tx;
    lastTouchY = ty;
    host->onInput(ev);
    return true;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::None) {
    ev.isTouch = true;
    ev.phaseUp = true;  // a swipe is release-driven: phase "up"
    // The release point of a swipe is not exposed by the input manager, so the
    // last known contact position rides along: apps keyed on the direction only
    // (the usual case) are unaffected.
    ev.x = lastTouchX;
    ev.y = lastTouchY;
    switch (swipe) {
      case MappedInputManager::SwipeDir::Left: ev.gesture = LuaGesture::SwipeLeft; break;
      case MappedInputManager::SwipeDir::Right: ev.gesture = LuaGesture::SwipeRight; break;
      case MappedInputManager::SwipeDir::Up: ev.gesture = LuaGesture::SwipeUp; break;
      case MappedInputManager::SwipeDir::Down: ev.gesture = LuaGesture::SwipeDown; break;
      default: break;
    }
    host->onInput(ev);
    return true;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    ev.isTouch = true;
    ev.gesture = LuaGesture::Tap;
    ev.phaseUp = true;  // a tap is release-driven: phase "up"
    ev.x = tx;
    ev.y = ty;
    lastTouchX = tx;
    lastTouchY = ty;
    host->onInput(ev);
    return true;
  }
  ev.isTouch = false;
  // 2.x (section 4): every key press is a down/up PAIR with no auto-repeat. Back
  // is handled in loop() (its pair is delivered on release, and only the up
  // decides the exit), so it is deliberately not read here.
  ev.phaseUp = false;
  ev.keyOk = mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  ev.keyLeft = mappedInput.wasPressed(MappedInputManager::Button::Left);
  ev.keyRight = mappedInput.wasPressed(MappedInputManager::Button::Right);
  ev.keyUp = mappedInput.wasPressed(MappedInputManager::Button::Up);
  ev.keyDown = mappedInput.wasPressed(MappedInputManager::Button::Down);
  if (ev.keyOk || ev.keyLeft || ev.keyRight || ev.keyUp || ev.keyDown) {
    host->onInput(ev);
    return true;
  }
  // The release of the same key is its own event (phase "up").
  ev = LuaInputEvent{};
  ev.timeMs = now;
  ev.phaseUp = true;
  ev.keyOk = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  ev.keyLeft = mappedInput.wasReleased(MappedInputManager::Button::Left);
  ev.keyRight = mappedInput.wasReleased(MappedInputManager::Button::Right);
  ev.keyUp = mappedInput.wasReleased(MappedInputManager::Button::Up);
  ev.keyDown = mappedInput.wasReleased(MappedInputManager::Button::Down);
  if (ev.keyOk || ev.keyLeft || ev.keyRight || ev.keyUp || ev.keyDown) {
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

  const uint32_t now = millis();

  // ctx:quit() / ctx.longtask:stop(): the app asked to leave.
  if (host->quitRequested()) {
    host->clearQuitRequest();
    manager.popActivity();
    return;
  }

  // Back, per the contract (section 4): a SHORT press delivers the down and up
  // pair when it is released and only the `up` return decides the exit; a LONG
  // press (>= 1000 ms) is not delivered at all — it exits at once and the app
  // cannot stop it. Delivering down on press would look equivalent for a
  // well-behaved app, but the pair-at-release form is what the contract asks
  // for, and it is what keeps one press from acting on two screens.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, 1000)) {
    backLongPress = true;
    manager.popActivity();
    return;
  }
  if (backLongPress && !mappedInput.isPressed(MappedInputManager::Button::Back)) {
    backLongPress = false;  // the release that follows a long press is swallowed
  }
  if (!backLongPress && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    LuaInputEvent down;
    down.keyBack = true;
    down.timeMs = now;
    LuaInputEvent up = down;
    up.phaseUp = true;
    host->onInput(down);  // the down return value does not count
    if (!host->onInput(up)) {
      manager.popActivity();
      return;
    }
    lastInputMs = now;
  }
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
