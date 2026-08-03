// render_backend.h
// The seam a declarative UI layer (Qt/QML, or a headless browser) drops into
// later.
//
// The renderer's main loop knows only this. It hands over a raw RGB888 buffer
// and a timestamp; what happens inside is entirely the backend's business. A
// future QML backend implements the same three calls and nothing above it
// changes -- not the shared-memory transport, not the driver, not the config
// plumbing.
//
// The contract on renderFrame():
//   * `rgb` is exactly width*height*3 bytes.
//   * The backend must write EVERY pixel. It is handed a slot that may contain
//     an older frame, and partial writes would show as ghosting.
//   * It may block, allocate, and take as long as it likes. It is on the other
//     side of the shared-memory boundary from the driver, which is the entire
//     point of the split.

#ifndef LED_RENDERER_RENDER_BACKEND_H
#define LED_RENDERER_RENDER_BACKEND_H

#include <cstdint>
#include <memory>
#include <string>

#include "layout_config.h"

namespace led {

struct FrameData;   // whatever the data layer has most recently produced

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool Init(std::shared_ptr<const LayoutConfig> layout,
                      int width, int height, std::string *err) = 0;

    // Adopt a new layout at a frame boundary. Returning false means the
    // backend could not use it (e.g. a font failed to load); the caller keeps
    // the previous layout rather than dropping to a blank screen.
    virtual bool Reload(std::shared_ptr<const LayoutConfig> layout, std::string *err) = 0;

    // Hand the backend fresh data. Cheap to call with unchanged data: the
    // backend decides whether anything needs rebuilding.
    virtual void SetData(const FrameData &data) = 0;

    // Draw one complete frame.
    virtual void RenderFrame(uint8_t *rgb, int64_t t_ns) = 0;
};

}  // namespace led

#endif  // LED_RENDERER_RENDER_BACKEND_H
