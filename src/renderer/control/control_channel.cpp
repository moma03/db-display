// control_channel.cpp -- see control_channel.h.

#include "control_channel.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace led {

namespace {

constexpr const char *kEnvVar = "LED_PANEL_CONTROL_FD";
constexpr size_t kMaxLineBytes = 64 * 1024;   // same limit as the hub

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;   // Linux: no SIGPIPE if the hub is gone
#else
constexpr int kSendFlags = 0;              // macOS: SO_NOSIGPIPE, set in Open()
#endif

}  // namespace

ControlChannel::~ControlChannel() { Stop(); }

bool ControlChannel::Open(int fd) {
    if (fd_ >= 0) return true;
    if (fd < 0) {
        const char *env = std::getenv(kEnvVar);
        if (!env || !*env) return false;
        char *end = nullptr;
        const long v = std::strtol(env, &end, 10);
        if (*end != '\0' || v < 0 || v > 65535) {
            std::fprintf(stderr, "control: %s=%s is not a descriptor\n", kEnvVar, env);
            return false;
        }
        fd = static_cast<int>(v);
    }
    if (fcntl(fd, F_GETFD) < 0) {
        std::fprintf(stderr, "control: descriptor %d is not open: %s\n", fd, std::strerror(errno));
        return false;
    }
    // Ours alone: whatever this program starts must not hold the channel open.
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    fd_ = fd;
    return true;
}

void ControlChannel::Start(Handlers handlers) {
    if (fd_ < 0 || reader_.joinable()) return;
    handlers_ = std::move(handlers);
    stop_ = false;
    reader_ = std::thread([this] { ReadLoop(); });
}

bool ControlChannel::SendState(const nlohmann::json &values) {
    if (fd_ < 0) return false;
    const std::string line =
        nlohmann::json{{"type", "state"}, {"values", values}}.dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lk(send_mtx_);
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t n = send(fd_, line.data() + off, line.size() - off, kSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "control: could not report state: %s\n", std::strerror(errno));
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

void ControlChannel::Stop() {
    stop_ = true;
    if (reader_.joinable()) reader_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void ControlChannel::ReadLoop() {
    std::string buf;
    char chunk[4096];
    while (!stop_) {
        // poll() with a timeout rather than shutdown() to wake up: the socket
        // is shared with whatever else inherited it (the driver, under
        // run_pair.sh), and shutting it down would end it for all of them.
        pollfd p{fd_, POLLIN, 0};
        const int r = poll(&p, 1, 250);
        if (r == 0) continue;
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) {
            std::fprintf(stderr, "control: the hub closed the channel\n");
            break;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            const nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
            if (msg.is_discarded() || !msg.is_object()) {
                std::fprintf(stderr, "control: ignoring a malformed line\n");
                continue;
            }
            Dispatch(msg);
        }
        if (buf.size() > kMaxLineBytes) {
            std::fprintf(stderr, "control: dropping a line longer than %zu bytes\n", kMaxLineBytes);
            buf.clear();
        }
    }
}

void ControlChannel::Dispatch(const nlohmann::json &m) {
    const auto type = m.find("type");
    if (type == m.end() || !type->is_string()) return;
    try {
        if (*type == "settings") {
            const auto v = m.find("values");
            if (v != m.end() && v->is_object() && handlers_.on_settings) handlers_.on_settings(*v);
        } else if (*type == "set") {
            const auto k = m.find("key");
            const auto v = m.find("value");
            if (k != m.end() && k->is_string() && v != m.end() && handlers_.on_set)
                handlers_.on_set(k->get<std::string>(), *v);
        } else if (*type == "press") {
            const auto k = m.find("key");
            if (k != m.end() && k->is_string() && handlers_.on_press)
                handlers_.on_press(k->get<std::string>());
        }
    } catch (const std::exception &e) {
        // A handler bug must not end the channel.
        std::fprintf(stderr, "control: error handling a '%s' message: %s\n",
                     type->get<std::string>().c_str(), e.what());
    }
}

}  // namespace led
