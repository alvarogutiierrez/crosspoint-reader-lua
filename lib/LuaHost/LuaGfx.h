#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Frame-scoped draw queue for Lua `g` bindings. Lua callbacks run on the main
// loop task while GfxRenderer draws under RenderLock, so binding code never
// touches the renderer directly: ops are queued here and applied by the
// activity inside render(). Budgets keep a runaway script bounded: MAX_OPS
// draws per frame and a fixed per-frame text pool (first-come-first-served; an
// exhausted pool drops further text ops).
class LuaGfx {
 public:
  static constexpr int MAX_OPS = 1024;
  static constexpr size_t STRING_POOL_BYTES = 4096;

  explicit LuaGfx(GfxRenderer& renderer) : renderer(renderer) {}

  enum class Kind : uint8_t {
    ClearScreen,
    Text,
    DrawRect,
    FillRect,
    DrawLine,
    DrawCircle,
    FillCircle,
    Image,
  };

  void reset();

  // Ops applied in the last frame (captured in apply() before the reset; the
  // live opCount is always 0 by the time render() can observe it).
  int operationCount() const { return lastOps; }
  // Draw calls dropped because MAX_OPS / the text pool ran out during the last
  // frame. An app that draws ~550 ops (a 43-key keyboard built out of rounded
  // frames) silently lost its last rows at MAX_OPS = 512; overflow must be
  // visible, never silent.
  int droppedOps() const { return dropped; }
  bool addClear(bool black);
  bool addText(int fontId, int x, int y, const char* s, bool black);
  bool addRect(Kind kind, int x, int y, int w, int h, int lineWidth, bool black);
  bool addLine(int x1, int y1, int x2, int y2, bool black);
  bool addCircle(Kind kind, int cx, int cy, int radius, bool black);
  // Queue a preloaded 1bpp bitmap (bit1 = ink, row-major MSB-first) at x,y.
  bool addImage(const uint8_t* bitmap, int w, int h, int x, int y);

  // Apply every queued op to the bound renderer; then reset. Call under
  // RenderLock from the render path only.
  void apply();

 private:
  struct Op {
    Kind kind;
    bool ink;
    int a, b, c, d, e;  // geometry; meaning per kind
    const char* text;   // Text only; points into stringPool
    size_t textLen;
    // Image only: 1bpp framebuffer payload (XIC minus header).
    const uint8_t* bitmap = nullptr;
  };

  GfxRenderer& renderer;
  Op ops[MAX_OPS];
  char stringPool[STRING_POOL_BYTES];
  size_t stringPoolUsed = 0;
  int opCount = 0;
  int lastOps = 0;
  int dropped = 0;
};
