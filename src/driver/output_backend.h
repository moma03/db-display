// output_backend.h
// The seam a future Art-Net / Colorlight receiver-card backend drops into.
//
// The driver's hot loop knows only this interface. It never sees a FrameCanvas,
// a GPIO pin or a socket -- it hands over an RGB888 frame plus a dither phase
// and blocks until the backend has taken it. Everything panel-specific,
// including gamma and quantisation, lives behind here.
//
// The blocking contract is what paces the whole driver: PresentAndWait() must
// not return until the output is ready for the next frame. For the GPIO backend
// that is SwapOnVSync(); for an Art-Net backend it would be a send plus a sleep
// to the target frame interval. The hot loop has no timing logic of its own,
// which is why swapping the backend cannot change the loop.

#ifndef LED_DRIVER_OUTPUT_BACKEND_H
#define LED_DRIVER_OUTPUT_BACKEND_H

#include <cstdint>
#include <string>

namespace led {

struct DriverConfig;

class IOutputBackend {
public:
    virtual ~IOutputBackend() = default;

    // Bring the output up. Called once, before real-time priority is dropped
    // onto the hot loop. All allocation belongs here.
    virtual bool Init(const DriverConfig &cfg, std::string *err) = 0;

    // Geometry the backend expects. The driver validates the shm header
    // against these and refuses to start on a mismatch.
    virtual int Width()  const = 0;
    virtual int Height() const = 0;

    // The effective PWM depth after any command-line override. The driver
    // builds its gamma LUT and dither from this, not from the config file --
    // the hub launches us with --led-pwm-bits, and a quantiser targeting the
    // wrong depth would put every colour on the wrong level.
    virtual int PwmBits() const = 0;

    // Take one frame and block until the hardware is ready for the next.
    // Hot path: no allocation, no logging, no syscalls beyond what the
    // hardware handshake itself needs.
    virtual void PresentAndWait(const uint8_t *rgb888, uint32_t dither_phase) = 0;

    // Called when there is no new frame and dithering is off, i.e. the output
    // already shows the right thing. Must pace the loop without re-sending
    // pixels, so an idle driver costs approximately nothing.
    virtual void WaitIdle() = 0;

    virtual void Shutdown() = 0;
};

}  // namespace led

#endif  // LED_DRIVER_OUTPUT_BACKEND_H
