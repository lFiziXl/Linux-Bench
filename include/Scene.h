#pragma once

// Embree 4 public interface. A plain extern "C" umbrella header, so pulling
// it in here keeps the Scene API self-contained for main.cpp.
#include <rtcore.h>

#include <cstddef>
#include <cstdint>

#include "Vec3.h"

// ---------------------------------------------------------------------------
// XorShift32: a tiny, self-contained, lock-free PRNG.
//
// std::rand() is global, slow, and not thread-safe. This struct holds its
// own state, is cheap (3 XORs + 3 shifts per draw), and can feed millions
// of area-light / anti-aliasing samples per frame. Give every worker (or
// pixel) its own instance — no mutexes, no atomics, no shared state.
// ---------------------------------------------------------------------------
struct XorShift32 {
    std::uint32_t state = 0x9E3779B9u; // non-zero default: XorShift degenerates at state == 0

    constexpr XorShift32() noexcept = default;

    explicit constexpr XorShift32(std::uint32_t seed) noexcept
        : state(seed != 0u ? seed : 0x9E3779B9u) {}

    [[nodiscard]] constexpr std::uint32_t nextUInt() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // Uniform float in [0.0f, 1.0f) — top 24 bits scaled by 1/2^24.
    [[nodiscard]] constexpr float nextFloat() noexcept {
        return static_cast<float>(nextUInt() >> 8) / 16777216.0f;
    }
};

// A primary ray: an origin point plus a normalized direction.
struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// A ray-traced scene backed by Intel Embree 4.
//
// The constructor builds a heavy procedural geometry — a dense 3D grid of
// non-intersecting spheres plus a large planar floor disk, both
// tessellated into triangle-mesh buffers (~10.5M triangles; the floor is
// geomID 0, the sphere field geomID 1) — uploads them to Embree
// (rtcSetNewGeometryBuffer) and commits the BVH.
//
// Thread-safety contract:
//   * After the constructor returns the scene is read-only.
//   * Embree guarantees that ray queries (rtcIntersect1, rtcOccluded1)
//     on a committed scene are thread-safe, so traceRay() may be called
//     concurrently from any number of worker threads.
//   * traceRay() keeps all ray/hit state in caller-local stack variables
//     and takes its XorShift32 by reference from the caller, so no state is
//     shared between threads at all — concurrent callers must each pass
//     their own rng instance.
//
// Error reporting: the constructor throws std::runtime_error on Embree
// failure — it never returns a half-initialized scene.
class Scene {
public:
    // Builds the procedural geometry and commits the BVH.
    // Throws std::runtime_error if Embree setup or the BVH build fails.
    Scene();

    // Releases the Embree scene and device.
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Path-traces one primary ray: up to 4 bounces through matte
    // (cosine-weighted hemisphere), metal (reflection + random fuzz), and
    // glass (Schlick fresnel / refraction) materials, lit only by the
    // analytic dusk sky and sun. Returns the accumulated radiance.
    // Thread-safe given a caller-local rng (see class comment).
    [[nodiscard]] Vec3 traceRay(const Ray& ray, XorShift32& rng) const;

    // Scene statistics (reported by main at startup).
    [[nodiscard]] std::size_t sphereCount()   const noexcept { return m_sphereCount; }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return m_triangleCount; }

private:
    // Embree handles (opaque typedefs from rtcore.h).
    RTCDevice m_device = nullptr;
    RTCScene  m_scene  = nullptr;

    std::size_t m_sphereCount   = 0;
    std::size_t m_triangleCount = 0;
};
