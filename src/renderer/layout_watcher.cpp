#include "layout_watcher.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#if defined(__linux__)
#include <sys/inotify.h>
#endif

namespace led {

namespace {

#if defined(__linux__)
std::string DirOf(const std::string &p) {
    const size_t s = p.rfind('/');
    return (s == std::string::npos) ? std::string(".") : p.substr(0, s);
}
std::string BaseOf(const std::string &p) {
    const size_t s = p.rfind('/');
    return (s == std::string::npos) ? p : p.substr(s + 1);
}
#endif

}  // namespace

LayoutWatcher::~LayoutWatcher() { Stop(); }

bool LayoutWatcher::Start(const std::string &path, std::string *err) {
    path_ = path;

    LayoutConfig initial;
    if (!LoadLayoutConfig(path_, &initial, err)) return false;
    current_ = std::make_shared<const LayoutConfig>(std::move(initial));

    stop_.store(false);
    thread_ = std::thread([this] { WatchLoop(); });
    return true;
}

void LayoutWatcher::Stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (inotify_fd_ >= 0) { close(inotify_fd_); inotify_fd_ = -1; }
}

std::shared_ptr<const LayoutConfig> LayoutWatcher::Current() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return current_;
}

std::shared_ptr<const LayoutConfig> LayoutWatcher::TakeIfChanged() {
    if (!changed_.exchange(false, std::memory_order_acq_rel)) return nullptr;
    std::lock_guard<std::mutex> lk(mtx_);
    return current_;
}

void LayoutWatcher::TryReload() {
    LayoutConfig next;
    std::string err;
    if (!LoadLayoutConfig(path_, &next, &err)) {
        // Keep running the layout we have. This is the whole point: a
        // half-saved file must not take the display down.
        rejects_.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "renderer: layout reload REJECTED, keeping previous: %s\n",
                     err.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        // Editors often touch a file without changing it, and rebuilding the
        // layout resets every scroll position -- visible as the board jumping.
        if (current_ && current_->fingerprint == next.fingerprint) return;
        current_ = std::make_shared<const LayoutConfig>(std::move(next));
    }
    changed_.store(true, std::memory_order_release);
    reloads_.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "renderer: layout reloaded from %s\n", path_.c_str());
}

#if defined(__linux__)

void LayoutWatcher::WatchLoop() {
    const std::string dir  = DirOf(path_);
    const std::string base = BaseOf(path_);

    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0) {
        std::fprintf(stderr, "renderer: inotify_init1 failed (%s); "
                     "falling back to mtime polling\n", std::strerror(errno));
    } else if (inotify_add_watch(inotify_fd_, dir.c_str(),
                                 IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
        std::fprintf(stderr, "renderer: inotify_add_watch(%s) failed (%s); "
                     "falling back to mtime polling\n", dir.c_str(), std::strerror(errno));
        close(inotify_fd_);
        inotify_fd_ = -1;
    }

    // mtime is the fallback and also a safety net: if inotify events are ever
    // missed, the poll still catches the change within a second.
    struct stat st;
    time_t last_mtime = (stat(path_.c_str(), &st) == 0) ? st.st_mtime : 0;

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (!stop_.load(std::memory_order_relaxed)) {
        bool hit = false;

        if (inotify_fd_ >= 0) {
            const ssize_t n = read(inotify_fd_, buf, sizeof(buf));
            for (ssize_t off = 0; off < n;) {
                const auto *ev = reinterpret_cast<const struct inotify_event *>(buf + off);
                if (ev->len > 0 && base == ev->name) hit = true;
                off += sizeof(struct inotify_event) + ev->len;
            }
        }

        if (!hit && stat(path_.c_str(), &st) == 0 && st.st_mtime != last_mtime) {
            last_mtime = st.st_mtime;
            hit = true;
        }

        if (hit) {
            // Editors write in stages; a short settle avoids reading a file
            // that is still half-written (which would just be rejected, but
            // logging a spurious error every save is noise).
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            if (stat(path_.c_str(), &st) == 0) last_mtime = st.st_mtime;
            TryReload();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

#else  // portable fallback: poll mtime (macOS dev builds)

void LayoutWatcher::WatchLoop() {
    struct stat st;
    time_t last_mtime = (stat(path_.c_str(), &st) == 0) ? st.st_mtime : 0;
    while (!stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (stat(path_.c_str(), &st) == 0 && st.st_mtime != last_mtime) {
            last_mtime = st.st_mtime;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            TryReload();
        }
    }
}

#endif

}  // namespace led
