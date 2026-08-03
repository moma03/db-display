// layout_watcher.h
// Watches layout.json and hands the renderer a new LayoutConfig when it
// changes, with no restart and no recompile.
//
// Two things this gets right that a naive implementation does not:
//
//   * It watches the DIRECTORY, not the file. Editors (vim, and anything doing
//     an atomic save) write a temp file and rename it over the target, which
//     replaces the inode. An inotify watch on the file itself survives exactly
//     one save and then silently watches a deleted inode forever.
//
//   * A reload that fails to parse or validate is DISCARDED. The renderer
//     keeps drawing the previous layout and the error goes to the log. Saving a
//     half-finished edit must never blank the panel.
//
// Runs on its own low-priority thread. The renderer picks up the result at a
// frame boundary, so a reload can never tear a frame.

#ifndef LED_RENDERER_LAYOUT_WATCHER_H
#define LED_RENDERER_LAYOUT_WATCHER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "layout_config.h"

namespace led {

class LayoutWatcher {
public:
    ~LayoutWatcher();

    // Loads the layout once, synchronously. Fails if the initial load is bad --
    // there is nothing to fall back to at startup.
    bool Start(const std::string &path, std::string *err);

    void Stop();

    // Returns a new layout if one has been loaded since the last call,
    // otherwise nullptr. Called once per frame from the render thread.
    std::shared_ptr<const LayoutConfig> TakeIfChanged();

    // The layout in force right now.
    std::shared_ptr<const LayoutConfig> Current() const;

    uint64_t reload_count() const { return reloads_.load(std::memory_order_relaxed); }
    uint64_t reject_count() const { return rejects_.load(std::memory_order_relaxed); }

private:
    void WatchLoop();
    void TryReload();

    std::string path_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> changed_{false};
    std::atomic<uint64_t> reloads_{0}, rejects_{0};

    mutable std::mutex mtx_;
    std::shared_ptr<const LayoutConfig> current_;
    int inotify_fd_ = -1;
};

}  // namespace led

#endif  // LED_RENDERER_LAYOUT_WATCHER_H
