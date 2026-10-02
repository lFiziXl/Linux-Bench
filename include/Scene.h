#pragma once

// Embree 4 public interface. A plain extern "C" umbrella header, so pulling
// it in here keeps the Scene API self-contained for main.cpp.
#include <rtcore.h>

#include <cstddef>

#include "Vec3.h"

// A primary ray: an origin point plus a normalized direction.
struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// A ray-traced scene backed by Intel Embree 4.
//
// The constructor builds a heavy procedural geometry — a dense 3D grid of
// intersecting spheres tessellated into one shared triangle-mesh buffer
// (~10M triangles) — uploads it to Embree (rtcSetNewGeometryBuffer) and
// commits the BVH.
//
// Thread-safety contract:
//   * After the constructor returns the scene is read-only.
//   * Embree guarantees that ray queries (rtcIntersect1, rtcOccluded1)
//     on a committed scene are thread-safe, so traceRay() may be called
//     concurrently from any number of worker threads.
//   * traceRay() keeps all ray/hit state in caller-local stack variables,
//     so no state is shared between threads at all.
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

    // Traces one primary ray against the scene.
    // Returns the Lambertian-shaded color of the nearest hit — hard-shadow
    // tested with a second ray toward the light (ambient-only when
    // occluded) — or the background gradient for a miss. Thread-safe
    // (see class comment).
    [[nodiscard]] Vec3 traceRay(const Ray& ray) const;

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
