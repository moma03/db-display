// frame_shm.h
// Shared-memory frame transport between the renderer and the driver.
//
// Layout of the segment:
//
//     [ FrameShmHeader | slot 0 | slot 1 | slot 2 ]
//        256 bytes       w*h*3    w*h*3    w*h*3     (each slot 64-byte aligned)
//
// Synchronisation is a seqlock with a published slot index. The renderer
// writes a complete frame into a slot the driver is not reading, then does a
// single release-store of the incremented sequence number. The driver does an
// acquire-load of the sequence and only then reads `ready_slot`, which pairs
// with the release and guarantees every pixel write is visible.
//
// Why three slots and not two: the driver quantises straight out of the mapped
// slot without copying it, so a slot stays live for the whole of a driver pass.
// With two slots the renderer's next frame necessarily targets the slot the
// driver is mid-pass on, and you get a tear. Three slots plus `reading_slot`
// (which the renderer skips) makes that impossible rather than merely unlikely.
// `slot_count` lives in the header so this is not baked into either binary.
//
// No process ever blocks on another: the renderer never waits for the driver,
// and the driver re-presents its last good frame when nothing new has arrived.

#ifndef LED_COMMON_FRAME_SHM_H
#define LED_COMMON_FRAME_SHM_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace led {

inline constexpr uint32_t kFrameShmMagic   = 0x4C454421u;  // "LED!"
inline constexpr uint32_t kFrameShmVersion = 1u;
inline constexpr uint32_t kMinSlots        = 2u;
inline constexpr uint32_t kMaxSlots        = 8u;

// Fields are grouped onto separate cache lines by writer, so the driver's
// `reading_slot` store never invalidates the line the renderer publishes from.
struct alignas(64) FrameShmHeader {
    // --- immutable after creation ---
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    uint32_t slot_count;
    uint64_t slot_stride;    // bytes from one slot to the next
    uint64_t header_bytes;   // offset of slot 0
    uint8_t  _pad_static[24];

    // --- written by the renderer ---
    std::atomic<uint64_t> sequence;    // release-store publishes a frame
    std::atomic<uint32_t> ready_slot;  // slot published with `sequence`
    uint8_t  _pad_pub[52];

    // --- written by the driver ---
    std::atomic<uint32_t> reading_slot;  // relaxed; renderer avoids this slot
    uint8_t  _pad_read[60];

    // --- diagnostics only, never load-bearing ---
    std::atomic<uint64_t> render_ns;         // wall time of last renderFrame()
    std::atomic<uint64_t> frames_published;
    std::atomic<uint64_t> frames_presented;
    uint8_t  _pad_diag[40];
};

static_assert(sizeof(FrameShmHeader) == 256, "header layout changed; bump kFrameShmVersion");
static_assert(alignof(FrameShmHeader) == 64, "header must be cache-line aligned");
static_assert(offsetof(FrameShmHeader, sequence) == 64, "sequence must own a cache line");
static_assert(offsetof(FrameShmHeader, reading_slot) == 128, "reading_slot must own a cache line");

// Both processes must agree byte-for-byte, and the atomics must be usable
// across address spaces -- a lock-based std::atomic would keep its lock in
// process-local memory and silently fail to synchronise.
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "64-bit atomics must be lock-free for cross-process use");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "32-bit atomics must be lock-free for cross-process use");

inline constexpr size_t kSlotAlign = 64;

inline size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

inline size_t FrameBytes(uint32_t w, uint32_t h, uint32_t bpp) {
    return static_cast<size_t>(w) * h * bpp;
}

inline size_t SegmentBytes(uint32_t w, uint32_t h, uint32_t bpp, uint32_t slots) {
    return sizeof(FrameShmHeader) + AlignUp(FrameBytes(w, h, bpp), kSlotAlign) * slots;
}

// ---------------------------------------------------------------------------
// FrameShm -- owns the mapping. Construct via Create() or Open().
// ---------------------------------------------------------------------------
class FrameShm {
public:
    FrameShm() = default;
    ~FrameShm();
    FrameShm(const FrameShm &) = delete;
    FrameShm &operator=(const FrameShm &) = delete;

    // Create (or re-create) the segment. Used by the renderer and by shmgen.
    // `mode` is the file permission for the segment; the driver may be running
    // as a different user after rpi-rgb-led-matrix drops privileges, so this
    // needs to be readable by that user (0644 by default).
    bool Create(const std::string &path, uint32_t w, uint32_t h,
                uint32_t bpp, uint32_t slots, mode_t mode, std::string *err);

    // Open an existing segment read/write and validate the header. Fails loudly
    // on magic/version/dimension mismatch rather than rendering garbage.
    bool Open(const std::string &path, std::string *err);

    // Validate against expected geometry. Call from the driver at startup.
    bool Validate(uint32_t want_w, uint32_t want_h, uint32_t want_bpp,
                  std::string *err) const;

    void Close();

    bool valid() const { return base_ != nullptr; }
    FrameShmHeader *header() { return reinterpret_cast<FrameShmHeader *>(base_); }
    const FrameShmHeader *header() const {
        return reinterpret_cast<const FrameShmHeader *>(base_);
    }

    uint8_t *slot(uint32_t i) {
        return static_cast<uint8_t *>(base_) + header()->header_bytes +
               static_cast<size_t>(i) * header()->slot_stride;
    }
    const uint8_t *slot(uint32_t i) const {
        return static_cast<const uint8_t *>(base_) + header()->header_bytes +
               static_cast<size_t>(i) * header()->slot_stride;
    }

    // ---- renderer side ----------------------------------------------------
    // Pick a slot to draw into: neither the published one nor the one the
    // driver says it is reading.
    uint32_t AcquireWriteSlot() const;

    // Publish a completed slot. Release-store; everything written to the slot
    // before this call is visible to any driver that acquire-loads `sequence`.
    void Publish(uint32_t slot_index, uint64_t render_ns);

    // ---- driver side ------------------------------------------------------
    // Acquire-load the sequence. Returns false when nothing new has been
    // published since `last_seq`, in which case *out_slot is left alone.
    bool TryAcquireFrame(uint64_t *last_seq, uint32_t *out_slot) const;

    // Tell the renderer which slot is live. Relaxed: it is a hint that only
    // ever makes the renderer more conservative, never less.
    void MarkReading(uint32_t slot_index);

    size_t frame_bytes() const {
        return FrameBytes(header()->width, header()->height,
                          header()->bytes_per_pixel);
    }

private:
    void  *base_ = nullptr;
    size_t size_ = 0;
    int    fd_   = -1;
    std::string path_;
    bool   owner_ = false;   // unlink on close
};

// Monotonic nanoseconds, used for the diagnostics fields.
uint64_t NowNs();

}  // namespace led

#endif  // LED_COMMON_FRAME_SHM_H
