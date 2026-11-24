// (2026-11-21, base PS5; the idea is HeyListen TICK-052's gone_ring.h, written again here) Which texture each bump of the
// textures' destroy generation was for, so that what was resolved to a texture at one generation can tell at a later one
// whether that texture went in between (stale) or only others did (it stands). Every texture destroyed (an eviction, a render
// target let go, a texture made again at another size) bumped one generation, and every recording thread's view cache
// (texture_view_of's handle hits) and the descriptor reuse went stale with it: the next look at every texture handle took the
// slow path (the slot read, the texture map, the view map). On the PS5 the texture budget evicts in most frames while the camera
// moves, and render targets are made and let go ~0.6 times a frame (klog_61: 4,438 in a session), so that was nearly every
// frame.
//
// One writer at a time (destroy_later and its callers run on the replay thread, or under ParLock in a parallel replay); any
// number of readers, without a lock. A bump writes its entry before it publishes the generation; a reader that finds an entry
// of another generation (written over by a later bump, or not yet published) answers "stale", as the plain compare did. A bump
// for no texture (nullptr) is for all of them. A reader whose object is null (nothing was resolved) is always stale.
// Kept pure (no Vulkan): it can be compiled alone.
#pragma once
#include <atomic>
#include <cstdint>

namespace mkwvk::gone_ring {

template <typename T, uint64_t N = 1024, uint64_t Window = 256>
class Ring {
    static_assert(N && (N & (N - 1)) == 0, "a power of two");
    static_assert(Window < N, "the window must lie within the ring");
    struct Entry {
        std::atomic<uint64_t> gen{0};
        std::atomic<const void*> obj{nullptr};   // (null: every object)
    };
    Entry ring_[N];
    std::atomic<uint64_t> gen_;

public:
    explicit Ring(uint64_t first = 1) : gen_(first) {}
    uint64_t generation() const { return gen_.load(std::memory_order_acquire); }
    /// A bump for obj going (null: for all); obj may be any tag, a texture or a sentinel. Returns the new generation
    uint64_t bump(const void* obj) {
        const uint64_t g = gen_.load(std::memory_order_relaxed) + 1;
        Entry& e = ring_[g & (N - 1)];
        e.gen.store(0, std::memory_order_relaxed);
        e.obj.store(obj, std::memory_order_relaxed);
        e.gen.store(g, std::memory_order_release);
        gen_.store(g, std::memory_order_release);
        return g;
    }
    /// Whether obj, resolved at generation `from`, still stands at `to`: no bump in (from, to] was for it or for all. A null obj
    /// never stands across a bump, nor anything across a gap wider than the window
    bool kept(const T* obj, uint64_t from, uint64_t to) const {
        if (from == to) return true;
        if (!obj || to < from || to - from > Window) return false;
        for (uint64_t g = from + 1; g <= to; g++) {
            const Entry& e = ring_[g & (N - 1)];
            if (e.gen.load(std::memory_order_acquire) != g) return false;
            const void* x = e.obj.load(std::memory_order_relaxed);
            if (!x || x == (const void*)obj) return false;
            if (e.gen.load(std::memory_order_acquire) != g) return false;   // (written over while it was read)
        }
        return true;
    }
};

} // namespace mkwvk::gone_ring
