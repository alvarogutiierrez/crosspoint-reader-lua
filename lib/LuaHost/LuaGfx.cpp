#include "LuaCApi.h"
#include "LuaGfx.h"

#include <GfxRenderer.h>

#include <cstring>

void LuaGfx::reset() {
  opCount = 0;
  stringPoolUsed = 0;
  dropped = 0;
}

bool LuaGfx::addClear(bool black) {
  if (opCount >= MAX_OPS) {
    ++dropped;
    return false;
  }
  Op& op = ops[opCount++];
  op.kind = Kind::ClearScreen;
  op.ink = black;
  op.text = nullptr;
  op.textLen = 0;
  return true;
}

bool LuaGfx::addText(int fontId, int x, int y, const char* s, bool black) {
  if (opCount >= MAX_OPS || !s) {
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
  char* slot = stringPool + stringPoolUsed;
  memcpy(slot, s, len);
  slot[len] = '\0';
  stringPoolUsed += len + 1;

  Op& op = ops[opCount++];
  op.kind = Kind::Text;
  op.ink = black;
  op.a = x;
  op.b = y;
  op.c = fontId;
  op.text = slot;
  op.textLen = len;
  return true;
}

bool LuaGfx::addImage(const uint8_t* bitmap, int w, int h, int x, int y) {
  Op op;
  op.kind = Kind::Image;
  op.a = x;
  op.b = y;
  op.c = w;
  op.d = h;
  op.bitmap = bitmap;
  lastOps++;
  if (opCount < MAX_OPS) {
    ops[opCount++] = op;
    return true;
  }
  ++dropped;
  return false;
}

bool LuaGfx::addRect(Kind kind, int x, int y, int w, int h, int lineWidth, bool black) {
  if (opCount >= MAX_OPS) {
    ++dropped;
    return false;
  }
  Op& op = ops[opCount++];
  op.kind = kind;
  op.ink = black;
  op.a = x;
  op.b = y;
  op.c = w;
  op.d = h;
  op.e = lineWidth;
  op.text = nullptr;
  op.textLen = 0;
  return true;
}

bool LuaGfx::addLine(int x1, int y1, int x2, int y2, bool black) {
  if (opCount >= MAX_OPS) {
    ++dropped;
    return false;
  }
  Op& op = ops[opCount++];
  op.kind = Kind::DrawLine;
  op.ink = black;
  op.a = x1;
  op.b = y1;
  op.c = x2;
  op.d = y2;
  op.e = 0;
  op.text = nullptr;
  op.textLen = 0;
  return true;
}

bool LuaGfx::addCircle(Kind kind, int cx, int cy, int radius, bool black) {
  if (opCount >= MAX_OPS) {
    ++dropped;
    return false;
  }
  Op& op = ops[opCount++];
  op.kind = kind;
  op.ink = black;
  op.a = cx;
  op.b = cy;
  op.c = radius;
  op.text = nullptr;
  op.textLen = 0;
  return true;
}

void LuaGfx::apply() {
  lastOps = opCount;
  for (int i = 0; i < opCount; ++i) {
    const Op& op = ops[i];
    switch (op.kind) {
      case Kind::ClearScreen:
        renderer.clearScreen();
        break;
      case Kind::Text:
        // Pooled strings are always NUL-terminated (addText reserves the byte),
        // so they go straight to the renderer.
        renderer.drawText(op.c, op.a, op.b, op.text, op.ink);
        break;
      case Kind::DrawRect:
        renderer.drawRect(op.a, op.b, op.c, op.d, op.e > 0 ? op.e : 1, op.ink);
        break;
      case Kind::FillRect:
        renderer.fillRect(op.a, op.b, op.c, op.d, op.ink);
        break;
      case Kind::DrawLine:
        renderer.drawLine(op.a, op.b, op.c, op.d, op.ink);
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
        if (op.kind == Kind::FillCircle) {
          for (int dy = -r; dy <= r; ++dy) {
            const int inside = r * r - dy * dy;
            int dx = r;
            while (dx > 0 && dx * dx > inside) --dx;
            renderer.fillRect(op.a - dx, op.b + dy, 2 * dx + 1, 1, op.ink);
          }
        } else {
          // 1px outline: midpoint circle over the 8 octants.
          int x = r, y = 0, err = 1 - r;
          while (x >= y) {
            renderer.drawPixel(op.a + x, op.b + y, op.ink);
            renderer.drawPixel(op.a - x, op.b + y, op.ink);
            renderer.drawPixel(op.a + x, op.b - y, op.ink);
            renderer.drawPixel(op.a - x, op.b - y, op.ink);
            renderer.drawPixel(op.a + y, op.b + x, op.ink);
            renderer.drawPixel(op.a - y, op.b + x, op.ink);
            renderer.drawPixel(op.a + y, op.b - x, op.ink);
            renderer.drawPixel(op.a - y, op.b - x, op.ink);
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
                renderer.drawPixel(op.a + col, ly, op.ink);
              }
            }
          }
        }
        break;
      }
    }
  }
  reset();
}
