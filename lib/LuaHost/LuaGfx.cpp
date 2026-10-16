#include "LuaCApi.h"
#include "LuaGfx.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdlib>
#include <cstring>

void LuaGfx::reset() {
  opCount = 0;
  stringPoolUsed = 0;
  dropped = 0;
  imagesQueued = 0;
}

LuaGfx::Op* LuaGfx::nextOp() {
  if (opCount >= MAX_OPS) return nullptr;  // caller counts the drop
  const size_t used = static_cast<size_t>(opCount);
  if (used >= ops.size()) {
    size_t want = ops.empty() ? 128 : ops.size() * 2;
    if (want > static_cast<size_t>(MAX_OPS)) want = MAX_OPS;
    if (used >= want) return nullptr;
    ops.resize(want);  // Op is trivially constructible; capacity is retained
  }
  return &ops[used];
}

bool LuaGfx::addClear(bool black) {
  Op* slot = nextOp();
  if (slot == nullptr) {
    ++dropped;
    return false;
  }
  Op& op = *slot;
  op.kind = static_cast<uint8_t>(Kind::ClearScreen);
  op.ink = black;
  op.text = nullptr;
  ++opCount;
  return true;
}

bool LuaGfx::addText(int fontId, int x, int y, const char* s, bool black) {
  if (!s) {
    ++dropped;
    return false;
  }
  Op* slot = nextOp();
  if (slot == nullptr) {
    ++dropped;
    return false;
  }
  size_t len = strlen(s);
  // Always keep room for the NUL: every pooled string stays NUL-terminated, so
  // apply() can hand op.text straight to the renderer (no full-pool scratch
  // buffer on the render task's stack).
  const size_t avail = STRING_POOL_BYTES - stringPoolUsed;
  if (len + 1 > avail) {
    if (avail <= 1) {
      ++dropped;  // Per-frame text pool exhausted.
      return false;
    }
    len = avail - 1;
    // Do not split a UTF-8 sequence; trim back to a codepoint boundary.
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) len--;
  }
  char* textSlot = stringPool + stringPoolUsed;
  memcpy(textSlot, s, len);
  textSlot[len] = '\0';
  stringPoolUsed += len + 1;

  Op& op = *slot;
  op.kind = static_cast<uint8_t>(Kind::Text);
  op.ink = black;
  op.a = static_cast<int16_t>(x);
  op.b = static_cast<int16_t>(y);
  op.c = static_cast<int16_t>(fontId);
  op.text = textSlot;
  ++opCount;
  return true;
}

bool LuaGfx::addImage(const uint8_t* bitmap, int w, int h, int x, int y, bool ink) {
  Op* slot = nextOp();
  if (slot == nullptr) {
    ++dropped;
    return false;
  }
  Op& op = *slot;
  op.kind = static_cast<uint8_t>(Kind::Image);
  op.a = static_cast<int16_t>(x);
  op.b = static_cast<int16_t>(y);
  op.c = static_cast<int16_t>(w);
  op.d = static_cast<int16_t>(h);
  op.ink = ink;
  op.bitmap = bitmap;
  ++imagesQueued;
  ++opCount;
  return true;
}

bool LuaGfx::addRect(Kind kind, int x, int y, int w, int h, int lineWidth, bool black) {
  Op* slot = nextOp();
  if (slot == nullptr) {
    ++dropped;
    return false;
  }
  Op& op = *slot;
  op.kind = static_cast<uint8_t>(kind);
  op.ink = black;
  op.a = static_cast<int16_t>(x);
  op.b = static_cast<int16_t>(y);
  op.c = static_cast<int16_t>(w);
  op.d = static_cast<int16_t>(h);
  op.e = static_cast<int16_t>(lineWidth);
  op.text = nullptr;
  ++opCount;
  return true;
}

bool LuaGfx::addLine(int x1, int y1, int x2, int y2, bool black) {
  Op* slot = nextOp();
  if (slot == nullptr) {
    ++dropped;
    return false;
  }
  Op& op = *slot;
  op.kind = static_cast<uint8_t>(Kind::DrawLine);
  op.ink = black;
  op.a = static_cast<int16_t>(x1);
  op.b = static_cast<int16_t>(y1);
  op.c = static_cast<int16_t>(x2);
  op.d = static_cast<int16_t>(y2);
  op.e = 0;
  op.text = nullptr;
  ++opCount;
  return true;
}

bool LuaGfx::addCircle(Kind kind, int cx, int cy, int radius, bool black) {
  Op* slot = nextOp();
  if (slot == nullptr) {
    ++dropped;
    return false;
  }
  Op& op = *slot;
  op.kind = static_cast<uint8_t>(kind);
  op.ink = black;
  op.a = static_cast<int16_t>(cx);
  op.b = static_cast<int16_t>(cy);
  op.c = static_cast<int16_t>(radius);
  op.text = nullptr;
  ++opCount;
  return true;
}

// --- offscreen layers -----------------------------------------------------

int LuaGfx::liveLayers() const {
  int n = 0;
  for (int i = 0; i < MAX_LAYERS; ++i) {
    if (layers[i].bits != nullptr) ++n;
  }
  return n;
}

int LuaGfx::createLayer(int w, int h) {
  if (w <= 0 || h <= 0 || w > MAX_LAYER_SIDE || h > MAX_LAYER_SIDE) {
    LOG_ERR("LUAHOST", "layer create(%d,%d) rejected (limit %d)", w, h, MAX_LAYER_SIDE);
    return -1;
  }
  const int stride = (w + 7) / 8;
  const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
  if (layerBytesUsed + bytes > MAX_LAYER_BYTES) {
    LOG_ERR("LUAHOST", "layer create(%d,%d): over budget (%u B live)", w, h,
            static_cast<unsigned>(layerBytesUsed));
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < MAX_LAYERS; ++i) {
    if (layers[i].bits == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    LOG_ERR("LUAHOST", "layer create: no free slot (%d)", MAX_LAYERS);
    return -1;
  }
  uint8_t* bits = static_cast<uint8_t*>(malloc(bytes));
  if (bits == nullptr) {
    LOG_ERR("LUAHOST", "layer create: OOM %u B", static_cast<unsigned>(bytes));
    return -1;
  }
  memset(bits, 0, bytes);  // no ink
  layers[slot] = Layer{bits, w, h, stride};
  layerBytesUsed += bytes;
  return slot;
}

bool LuaGfx::disposeLayer(int id) {
  if (!layerValid(id)) return false;
  if (targetLayer == id) endLayer();
  layerBytesUsed -= static_cast<size_t>(layers[id].stride) * static_cast<size_t>(layers[id].h);
  free(layers[id].bits);
  layers[id] = Layer{};
  return true;
}

bool LuaGfx::beginLayer(int id) {
  if (!layerValid(id)) return false;
  if (targetLayer >= 0) {
    // The original refuses nesting the same way ("layer:draw_with cannot be
    // nested"): the inner draw_with is skipped, not merged.
    LOG_ERR("LUAHOST", "layer:draw_with cannot be nested");
    return false;
  }
  targetLayer = id;
  layerOpMark = opCount;
  layerPoolMark = stringPoolUsed;
  return true;
}

void LuaGfx::endLayer() {
  if (targetLayer < 0) return;
  // Rasterize while the layer is STILL the target (clearing it first would send
  // these ops to the panel: the layer would stay empty and its content would
  // paint on screen).
  applyRange(layerOpMark, opCount);
  opCount = layerOpMark;  // ...and keep the layer's ops out of the frame
  stringPoolUsed = layerPoolMark;
  targetLayer = -1;
}

bool LuaGfx::addLayerBlit(int id, int x, int y, bool black) {
  if (!layerValid(id)) return false;
  Op* slot = nextOp();
  if (slot == nullptr) {
    ++dropped;
    return false;
  }
  Op& op = *slot;
  op.kind = static_cast<uint8_t>(Kind::LayerBlit);
  op.ink = black;
  op.a = static_cast<int16_t>(id);
  op.b = static_cast<int16_t>(x);
  op.c = static_cast<int16_t>(y);
  op.d = 0;
  op.e = 0;
  op.text = nullptr;
  op.bitmap = nullptr;
  ++opCount;
  return true;
}

void LuaGfx::targetSize(int& w, int& h) const {
  if (targetLayer >= 0 && layerValid(targetLayer)) {
    w = layers[targetLayer].w;
    h = layers[targetLayer].h;
    return;
  }
  w = renderer.getScreenWidth();
  h = renderer.getScreenHeight();
}

void LuaGfx::putPixel(int x, int y, bool ink) {
  if (targetLayer < 0) {
    renderer.drawPixel(x, y, ink);
    return;
  }
  Layer& l = layers[targetLayer];
  if (x < 0 || y < 0 || x >= l.w || y >= l.h) return;  // layer-local clip
  uint8_t& byte = l.bits[static_cast<size_t>(y) * l.stride + (x >> 3)];
  const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
  if (ink) {
    byte |= mask;
  } else {
    byte &= static_cast<uint8_t>(~mask);
  }
}

void LuaGfx::putSpan(int x, int y, int w, int h, bool ink) {
  if (targetLayer < 0) {
    renderer.fillRect(x, y, w, h, ink);
    return;
  }
  for (int dy = 0; dy < h; ++dy) {
    for (int dx = 0; dx < w; ++dx) putPixel(x + dx, y + dy, ink);
  }
}

void LuaGfx::apply() {
  lastOps = opCount;
  lastDropped = dropped;  // dropped counts the frame that just queued
  lastImages = imagesQueued;
  imagesQueued = 0;
  applyRange(0, opCount);
  reset();
}

void LuaGfx::applyRange(int from, int to) {
  for (int i = from; i < to; ++i) {
    const Op& op = ops[i];
    const Kind kind = static_cast<Kind>(op.kind);
    switch (kind) {
      case Kind::ClearScreen:
        if (targetLayer >= 0) {
          putSpan(0, 0, layers[targetLayer].w, layers[targetLayer].h, op.ink);
        } else {
          renderer.clearScreen();
        }
        break;
      case Kind::Text:
        // Pooled strings are always NUL-terminated (addText reserves the byte),
        // so they go straight to the renderer.
        if (targetLayer >= 0) {
          // Same limitation the original firmware has: layers hold geometry and
          // images, not glyph rasters.
          LOG_ERR("LUAHOST", "g:text inside layer:draw_with is not supported, skipped");
          ++dropped;
        } else {
          renderer.drawText(op.c, op.a, op.b, op.text, op.ink);
        }
        break;
      case Kind::DrawRect:
        if (targetLayer < 0) {
          renderer.drawRect(op.a, op.b, op.c, op.d, op.e > 0 ? op.e : 1, op.ink);
        } else {
          putSpan(op.a, op.b, op.c, 1, op.ink);
          putSpan(op.a, op.b + op.d - 1, op.c, 1, op.ink);
          putSpan(op.a, op.b, 1, op.d, op.ink);
          putSpan(op.a + op.c - 1, op.b, 1, op.d, op.ink);
        }
        break;
      case Kind::FillRect:
        putSpan(op.a, op.b, op.c, op.d, op.ink);
        break;
      case Kind::DrawLine:
        if (targetLayer < 0) {
          renderer.drawLine(op.a, op.b, op.c, op.d, op.ink);
        } else {
          int x0 = op.a, y0 = op.b;
          const int x1 = op.c, y1 = op.d;
          const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
          const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
          int err = dx + dy;
          while (true) {
            putPixel(x0, y0, op.ink);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
          }
        }
        break;
      case Kind::DrawCircle:
      case Kind::FillCircle: {
        // GfxRenderer has no circle primitive. The FILL is an exact scan (for
        // every row |dy| <= r the span is |dx| <= isqrt(r^2 - dy^2)): a
        // Bresenham 8-octant disc comes out ~1px fat along the diagonals, which
        // shows up as the corners of a rounded frame/button protruding past
        // their straight 3px band — visible on capsules, invisible at small
        // radii.
        const int r = op.c > 0 ? op.c : 1;
        if (kind == Kind::FillCircle) {
          for (int dy = -r; dy <= r; ++dy) {
            const int inside = r * r - dy * dy;
            int dx = r;
            while (dx > 0 && dx * dx > inside) --dx;
            putSpan(op.a - dx, op.b + dy, 2 * dx + 1, 1, op.ink);
          }
        } else {
          // 1px outline: midpoint circle over the 8 octants.
          int x = r, y = 0, err = 1 - r;
          while (x >= y) {
            putPixel(op.a + x, op.b + y, op.ink);
            putPixel(op.a - x, op.b + y, op.ink);
            putPixel(op.a + x, op.b - y, op.ink);
            putPixel(op.a - x, op.b - y, op.ink);
            putPixel(op.a + y, op.b + x, op.ink);
            putPixel(op.a - y, op.b + x, op.ink);
            putPixel(op.a + y, op.b - x, op.ink);
            putPixel(op.a - y, op.b - x, op.ink);
            ++y;
            if (err < 0) {
              err += 2 * y + 1;
            } else {
              --x;
              err += 2 * (y - x) + 1;
            }
          }
        }
        break;
      }
      case Kind::Image: {
        // XIC payload: row-major 1bpp, MSB-first, bit=1 ink. Blitted through
        // drawPixel() in LOGICAL coordinates on purpose: GfxRenderer::drawImage
        // writes PANEL-space bits (its Portrait branch also shifts the origin
        // up by `height`, sending a y=0 image off-screen) and never rotates the
        // bits ("TODO: Rotate bits"), while XTApp g:image takes a logical
        // top-left like every other g: op. Reader-style per-pixel blit keeps
        // the orientation correct.
        if (op.bitmap != nullptr && op.c > 0 && op.d > 0) {
          const int stride = (op.c + 7) / 8;
          for (int row = 0; row < op.d; ++row) {
            const uint8_t* src = op.bitmap + row * stride;
            const int ly = op.b + row;
            for (int col = 0; col < op.c; ++col) {
              if ((src[col >> 3] >> (7 - (col & 7))) & 1) {
                putPixel(op.a + col, ly, op.ink);
              }
            }
          }
        }
        break;
      }
      case Kind::LayerBlit: {
        if (!layerValid(op.a)) break;
        const Layer& l = layers[op.a];
        for (int row = 0; row < l.h; ++row) {
          const uint8_t* src = l.bits + static_cast<size_t>(row) * l.stride;
          for (int col = 0; col < l.w; ++col) {
            if ((src[col >> 3] >> (7 - (col & 7))) & 1) {
              putPixel(op.b + col, op.c + row, op.ink);
            }
          }
        }
        break;
      }
    }
  }
}
