#include "frame_shm.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <new>

namespace led {

uint64_t NowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

static std::string Errno(const std::string &what) {
    return what + ": " + std::strerror(errno);
}

FrameShm::~FrameShm() { Close(); }

void FrameShm::Close() {
    if (base_) { munmap(base_, size_); base_ = nullptr; }
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (owner_ && !path_.empty()) { shm_unlink(path_.c_str()); owner_ = false; }
    size_ = 0;
}

bool FrameShm::Create(const std::string &path, uint32_t w, uint32_t h,
                      uint32_t bpp, uint32_t slots, mode_t mode,
                      std::string *err) {
    Close();
    if (w == 0 || h == 0 || bpp == 0) { *err = "zero frame geometry"; return false; }
    if (slots < kMinSlots || slots > kMaxSlots) {
        *err = "slot_count out of range (" + std::to_string(kMinSlots) + ".." +
               std::to_string(kMaxSlots) + ")";
        return false;
    }

    const size_t total = SegmentBytes(w, h, bpp, slots);

    // Unlink any stale segment first: a leftover from a crashed run may have a
    // different geometry, and ftruncate on an existing mapping would not
    // shrink it for processes that still have it mapped.
    shm_unlink(path.c_str());

    fd_ = shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, mode);
    if (fd_ < 0) { *err = Errno("shm_open(create)"); return false; }

    // shm_open honours umask, which on Linux would silently drop the
    // group/other read bit that the privilege-dropped driver needs (the matrix
    // library drops to `daemon` once the GPIO is up). Best-effort: macOS
    // rejects fchmod on a shm fd with EINVAL, and there the mode from shm_open
    // already stands, so a failure here is not worth aborting a working
    // segment over.
    (void)fchmod(fd_, mode);

    if (ftruncate(fd_, static_cast<off_t>(total)) != 0) {
        *err = Errno("ftruncate"); Close(); return false;
    }

    base_ = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) { base_ = nullptr; *err = Errno("mmap"); Close(); return false; }
    size_  = total;
    path_  = path;
    owner_ = true;

    std::memset(base_, 0, total);

    // Placement-new so the atomics are properly constructed in shared memory.
    auto *hdr = new (base_) FrameShmHeader{};
    hdr->width           = w;
    hdr->height          = h;
    hdr->bytes_per_pixel = bpp;
    hdr->slot_count      = slots;
    hdr->slot_stride     = AlignUp(FrameBytes(w, h, bpp), kSlotAlign);
    hdr->header_bytes    = sizeof(FrameShmHeader);
    hdr->sequence.store(0, std::memory_order_relaxed);
    hdr->ready_slot.store(0, std::memory_order_relaxed);
    hdr->reading_slot.store(kMaxSlots, std::memory_order_relaxed);  // "none"
    hdr->version         = kFrameShmVersion;
    // Magic last, with a release fence: a driver that opens the segment
    // concurrently either sees no magic at all or sees a fully built header.
    std::atomic_thread_fence(std::memory_order_release);
    hdr->magic = kFrameShmMagic;

    return true;
}

bool FrameShm::Open(const std::string &path, std::string *err) {
    Close();
    fd_ = shm_open(path.c_str(), O_RDWR, 0);
    if (fd_ < 0) { *err = Errno("shm_open(" + path + ")"); return false; }

    struct stat st;
    if (fstat(fd_, &st) != 0) { *err = Errno("fstat"); Close(); return false; }
    if (static_cast<size_t>(st.st_size) < sizeof(FrameShmHeader)) {
        *err = "segment too small to contain a header";
        Close();
        return false;
    }

    base_ = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) { base_ = nullptr; *err = Errno("mmap"); Close(); return false; }
    size_ = static_cast<size_t>(st.st_size);
    path_ = path;

    const FrameShmHeader *hdr = header();
    if (hdr->magic != kFrameShmMagic) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad magic 0x%08X (expected 0x%08X)",
                      hdr->magic, kFrameShmMagic);
        *err = buf;
        Close();
        return false;
    }
    if (hdr->version != kFrameShmVersion) {
        *err = "format version " + std::to_string(hdr->version) +
               ", this binary speaks " + std::to_string(kFrameShmVersion);
        Close();
        return false;
    }
    if (hdr->slot_count < kMinSlots || hdr->slot_count > kMaxSlots) {
        *err = "header slot_count " + std::to_string(hdr->slot_count) + " out of range";
        Close();
        return false;
    }
    const size_t need = hdr->header_bytes +
                        static_cast<size_t>(hdr->slot_stride) * hdr->slot_count;
    if (size_ < need) {
        *err = "segment is " + std::to_string(size_) + " bytes, header describes " +
               std::to_string(need);
        Close();
        return false;
    }
    if (hdr->slot_stride < FrameBytes(hdr->width, hdr->height, hdr->bytes_per_pixel)) {
        *err = "slot_stride smaller than one frame";
        Close();
        return false;
    }
    return true;
}

bool FrameShm::Validate(uint32_t want_w, uint32_t want_h, uint32_t want_bpp,
                        std::string *err) const {
    const FrameShmHeader *hdr = header();
    if (hdr->width != want_w || hdr->height != want_h) {
        *err = "frame is " + std::to_string(hdr->width) + "x" +
               std::to_string(hdr->height) + ", output expects " +
               std::to_string(want_w) + "x" + std::to_string(want_h);
        return false;
    }
    if (hdr->bytes_per_pixel != want_bpp) {
        *err = "bytes_per_pixel is " + std::to_string(hdr->bytes_per_pixel) +
               ", expected " + std::to_string(want_bpp);
        return false;
    }
    return true;
}

uint32_t FrameShm::AcquireWriteSlot() const {
    const FrameShmHeader *hdr = header();
    const uint32_t n       = hdr->slot_count;
    const uint32_t ready   = hdr->ready_slot.load(std::memory_order_relaxed);
    const uint32_t reading = hdr->reading_slot.load(std::memory_order_relaxed);
    for (uint32_t i = 1; i <= n; ++i) {
        const uint32_t cand = (ready + i) % n;
        if (cand != ready && cand != reading) return cand;
    }
    return (ready + 1) % n;  // n < 3 and driver mid-read: unreachable at n>=3
}

void FrameShm::Publish(uint32_t slot_index, uint64_t render_ns) {
    FrameShmHeader *hdr = header();
    hdr->render_ns.store(render_ns, std::memory_order_relaxed);
    hdr->ready_slot.store(slot_index, std::memory_order_relaxed);
    hdr->frames_published.fetch_add(1, std::memory_order_relaxed);
    // The release here is the publication point: every pixel byte written above
    // happens-before any driver acquire-load that observes this value.
    hdr->sequence.fetch_add(1, std::memory_order_release);
}

bool FrameShm::TryAcquireFrame(uint64_t *last_seq, uint32_t *out_slot) const {
    const FrameShmHeader *hdr = header();
    const uint64_t seq = hdr->sequence.load(std::memory_order_acquire);
    if (seq == *last_seq) return false;
    *last_seq  = seq;
    *out_slot  = hdr->ready_slot.load(std::memory_order_relaxed) % hdr->slot_count;
    return true;
}

void FrameShm::MarkReading(uint32_t slot_index) {
    header()->reading_slot.store(slot_index, std::memory_order_relaxed);
}

}  // namespace led
