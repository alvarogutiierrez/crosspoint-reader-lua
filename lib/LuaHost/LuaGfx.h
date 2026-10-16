#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class GfxRenderer;

// Frame-scoped draw queue for Lua `g` bindings. Lua callbacks run on the main
// loop task while GfxRenderer draws under RenderLock, so binding code never
// touches the renderer directly: ops are queued here and applied by the
// activity inside render(). Budgets keep a runaway script bounded: MAX_OPS
// draws per frame and a fixed per-frame text pool (first-come-first-served; an
// exhausted pool drops further text ops).
class LuaGfx {
 public:
  // Per-frame op budget. Real apps go big: Sudoku's 9x9 board (81 hatched
  // cells + borders + digits) queues ~2678 ops in one frame, so a 1024 cap
  // silently truncated it. The queue itself grows on demand (see nextOp) so a
  // light app pays nothing for the headroom.
  static constexpr int MAX_OPS = 3072;
  static constexpr size_t STRING_POOL_BYTES = 8192;

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
    LayerBlit,
  };

  // Offscreen layers (XTApp `ctx.layers`): a 1bpp bitmap the g: ops rasterize
  // into (bit = 1 ink) which on_draw can then blit any number of times with
  // g:layer(handle, x, y) — how apps cache static geometry (Sudoku's hatch
  // tiles). Cardinality and size are capped because live layers hold RAM for
  // the whole app session. Like the original firmware, layers cannot nest and
  // g:text inside layer:draw_with is NOT supported ("g:text inside
  // layer:draw_with is not supported, skipped" is its own log line): text ops
  // queued into a layer are dropped with a LOG_ERR rather than silently lost.
  static constexpr int MAX_LAYERS = 8;
  static constexpr int MAX_LAYER_SIDE = 200;
  static constexpr size_t MAX_LAYER_BYTES = 20 * 1024;

  // Returns a layer id, or -1 when unsupported (bad size / no slot / out of
  // budget). disposeLayer frees it; a blit of a disposed id is a no-op.
  int createLayer(int w, int h);
  bool disposeLayer(int id);
  bool layerValid(int id) const { return id >= 0 && id < MAX_LAYERS && layers[id].bits != nullptr; }
  // Ops queued between beginLayer/endLayer rasterize into the layer instead of
  // the screen; endLayer drains exactly those ops and restores the screen.
  bool beginLayer(int id);
  void endLayer();
  size_t layerBytes() const { return layerBytesUsed; }
  // Size of the current draw target (the open layer, else the panel): g:size().
  void targetSize(int& w, int& h) const;

  void reset();

  // Ops applied in the last frame (captured in apply() before the reset; the
  // live opCount is always 0 by the time render() can observe it).
  int operationCount() const { return lastOps; }
  // Draw calls dropped because MAX_OPS / the text pool ran out during the last
  // frame. An app that draws ~550 ops (a 43-key keyboard built out of rounded
  // frames) silently lost its last rows at MAX_OPS = 512; overflow must be
  // visible, never silent.
  int droppedOps() const { return lastDropped; }
  // Images queued in the last applied frame (ctx.perf:info().image_count).
  int imageCount() const { return lastImages; }
  int liveLayers() const;
  bool addClear(bool black);
  bool addText(int fontId, int x, int y, const char* s, bool black);
  // lineWidth/stroke only grows INWARD (2.x contract, section 6) and radius
  // rounds all four corners; both default to the 0.8 look (1px, square).
  bool addRect(Kind kind, int x, int y, int w, int h, int lineWidth, bool black, int radius = 0);
  bool addLine(int x1, int y1, int x2, int y2, bool black, int lineWidth = 1);
  bool addCircle(Kind kind, int cx, int cy, int radius, bool black, int lineWidth = 1);
  // Queue a preloaded 1bpp bitmap (bit1 = ink, row-major MSB-first) at x,y.
  // `ink` is the colour the set bits take: false draws the bitmap as paper
  // (an app flipping polarity with {invert = true}).
  bool addImage(const uint8_t* bitmap, int w, int h, int x, int y, bool ink);
  bool addLayerBlit(int id, int x, int y, bool black);

  // Apply every queued op to the bound renderer; then reset. Call under
  // RenderLock from the render path only.
  void apply();

 private:
  // Packed on purpose: 5 int16 of geometry + 2 pointers + kind/ink = 20 bytes,
  // so the full 3072-op queue is ~61 KB instead of ~130 KB.
  struct Op {
    const char* text = nullptr;       // Text only; points into stringPool
    const uint8_t* bitmap = nullptr;  // Image only: XIC payload (header stripped)
    // Geometry: coords/sizes/radii/ids, all bounded by the screen (int16 is
    // enough for 480x800 and for layer ids). NOT for font ids: those are 32-bit
    // hashes from fontIds.h and truncating one makes drawText find no font and
    // drop the text silently — hence the dedicated field.
    // For rects `e` packs the 2.x pair radius<<8 | line_width (both 0..255):
    // the struct must NOT grow, because Sudoku fills the 3072-op queue and
    // every extra byte per op is RAM the app cannot have back.
    int16_t a = 0, b = 0, c = 0, d = 0, e = 0;
    int32_t fontId = 0;  // Text only
    uint8_t kind = 0;
    bool ink = true;
  };
  // Guard the queue's memory: 3072 ops are a real app's frame (Sudoku), so the
  // per-op size is a budget, not a detail. If a new field is needed, pack it
  // into an existing one (radius<<8 | line_width) or the app loses RAM it was
  // counting on.
  static_assert(sizeof(Op) <= 32, "LuaGfx::Op must stay within 32 bytes");

  // Grows the queue on demand (capped at MAX_OPS); nullptr = queue full.
  Op* nextOp();

  // Rasterization sinks: with targetLayer == -1 ops go to the renderer (the
  // panel); otherwise they are written into that layer's bitmap, in LOGICAL
  // coordinates, clipped to its bounds.
  void putPixel(int x, int y, bool ink);
  void putSpan(int x, int y, int w, int h, bool ink);
  void applyRange(int from, int to);

  struct Layer {
    uint8_t* bits = nullptr;  // stride*h, 1bpp MSB-first, bit=1 ink
    int w = 0;
    int h = 0;
    int stride = 0;
  };

  GfxRenderer& renderer;
  std::vector<Op> ops;
  char stringPool[STRING_POOL_BYTES];
  size_t stringPoolUsed = 0;
  int opCount = 0;
  int lastOps = 0;
  int dropped = 0;
  int lastDropped = 0;
  int lastImages = 0;
  int imagesQueued = 0;
  Layer layers[MAX_LAYERS];
  size_t layerBytesUsed = 0;
  int targetLayer = -1;
  int layerOpMark = 0;         // first op belonging to the open layer
  size_t layerPoolMark = 0;    // stringPoolUsed when the layer opened
};
