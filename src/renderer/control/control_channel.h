// COPY of led-panel-runtime/src/app/control_channel.h -- db-display is not built on the
// runtime yet (it carries its own copies of the driver and shm code too).
// Change it there, then copy it here; it goes away with that migration.
//
// control_channel.h
// The program's end of the LED_Panel_Controller hub's control channel: how a
// panel program gets the settings the hub exposes to Home Assistant for it,
// and reports status back -- without any MQTT code or broker credentials.
// Protocol: LED_Panel_Controller/docs/program-controls.md.
//
//   led::ControlChannel ch;
//   if (ch.Open()) {                        // false: not started by the hub
//       led::ControlChannel::Handlers h;
//       h.on_settings = [&](const nlohmann::json &all) { ... };
//       h.on_set      = [&](const std::string &key, const nlohmann::json &v) { ... };
//       h.on_press    = [&](const std::string &key) { ... };
//       ch.Start(std::move(h));
//   }
//   ch.SendState({{"status", "ok"}, {"station_name", "Barmbek"}});   // no-op if closed
//
// Handlers run on the channel's own reader thread. The descriptor arrives
// inherited (LED_PANEL_CONTROL_FD); a program also hands it to anything it
// starts unless it is marked close-on-exec, which Open() does.

#ifndef LED_APP_CONTROL_CHANNEL_H
#define LED_APP_CONTROL_CHANNEL_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <json.hpp>

namespace led {

class ControlChannel {
public:
    struct Handlers {
        std::function<void(const nlohmann::json &values)> on_settings;
        std::function<void(const std::string &key, const nlohmann::json &value)> on_set;
        std::function<void(const std::string &key)> on_press;
    };

    ControlChannel() = default;
    ~ControlChannel();
    ControlChannel(const ControlChannel &) = delete;
    ControlChannel &operator=(const ControlChannel &) = delete;

    // Takes the descriptor named in $LED_PANEL_CONTROL_FD, or `fd` if >= 0.
    // False when there is none (started by hand) -- the rest are then no-ops.
    bool Open(int fd = -1);
    bool active() const { return fd_ >= 0; }

    // Starts the reader thread. Call once, after Open().
    void Start(Handlers handlers);

    // {"type": "state", "values": values}. False if the hub is gone.
    bool SendState(const nlohmann::json &values);

    // Stops the reader and closes the descriptor. Called by the destructor.
    void Stop();

private:
    void ReadLoop();
    void Dispatch(const nlohmann::json &message);

    int fd_ = -1;
    Handlers handlers_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::mutex send_mtx_;
};

}  // namespace led

#endif  // LED_APP_CONTROL_CHANNEL_H
