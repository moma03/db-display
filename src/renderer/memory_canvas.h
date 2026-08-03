// memory_canvas.h
// An rgb_matrix::Canvas backed by a plain RGB888 buffer.
//
// This is what lets the existing drawing code move across unchanged. ViewPort,
// ScrollingTextBox, DrawText and every draw* helper take a Canvas*, so pointing
// them at one of these writes a frame into shared memory instead of at a panel.
// The renderer therefore links graphics.cc and bdf-font.cc (which contain no
// GPIO code) and never opens /dev/mem, never needs root, and builds and runs on
// a laptop.
//
// Deliberately NOT gamma-corrected and NOT quantised: this holds linear 8-bit
// sRGB exactly as the drawing code produced it. All colour handling belongs to
// the driver. See src/common/gamma.h.

#ifndef LED_RENDERER_MEMORY_CANVAS_H
#define LED_RENDERER_MEMORY_CANVAS_H

#include <canvas.h>

#include <cstdint>
#include <cstring>

namespace led {

class MemoryCanvas : public rgb_matrix::Canvas {
public:
    MemoryCanvas(uint8_t *buf, int w, int h) : buf_(buf), w_(w), h_(h) {}

    void Retarget(uint8_t *buf) { buf_ = buf; }

    int width()  const override { return w_; }
    int height() const override { return h_; }

    void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
        if (static_cast<unsigned>(x) >= static_cast<unsigned>(w_) ||
            static_cast<unsigned>(y) >= static_cast<unsigned>(h_)) return;
        uint8_t *p = buf_ + (static_cast<size_t>(y) * w_ + x) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }

    void Clear() override { std::memset(buf_, 0, static_cast<size_t>(w_) * h_ * 3); }

    void Fill(uint8_t r, uint8_t g, uint8_t b) override {
        if (r == g && g == b) {  // common case: a grey, including black
            std::memset(buf_, r, static_cast<size_t>(w_) * h_ * 3);
            return;
        }
        uint8_t *p = buf_;
        const size_t n = static_cast<size_t>(w_) * h_;
        for (size_t i = 0; i < n; ++i) { *p++ = r; *p++ = g; *p++ = b; }
    }

private:
    uint8_t *buf_;
    int w_, h_;
};

}  // namespace led

#endif  // LED_RENDERER_MEMORY_CANVAS_H
