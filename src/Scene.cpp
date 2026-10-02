#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Procedural scene parameters.
//
// A kGridSize^3 grid of slightly overlapping spheres, each tessellated into
// a lat/long triangle mesh. All spheres are merged into a single
// triangle-mesh geometry so Embree builds one BVH over ~10M triangles — the
// real, heavy workload behind traceRay().
// ---------------------------------------------------------------------------
constexpr int   kGridSize    = 28;  // spheres per axis (28^3 = 21 952 spheres)
constexpr float kSpacing     = 0.30f; // center-to-center distance; 2r > spacing, so neighbors intersect
constexpr float kBaseRadius  = 0.17f; // base sphere radius (with ±~20% jitter)
constexpr int   kLonSegments = 20;  // longitude segments per sphere
constexpr int   kLatSegments = 14;  // latitude bands per sphere
constexpr Vec3  kFieldCenter{0.0, 0.0, -6.0}; // in front of the camera at the origin

// Lambertian light direction. Normalized once; initialization of a static
// local is thread-safe in C++11 and later.
const Vec3 kLightDir = Vec3(0.40, 0.70, 0.50).normalize();

// Vertical background gradient: warm at the bottom, cool at the top.
// Kept visually identical to the old analytic backend so misses read the
// same way.
[[nodiscard]] Vec3 backgroundColor(const Vec3& dir) {
    const double t = dir.y * 0.5 + 0.5; // 0 = looking down, 1 = looking up
    const Vec3 down(1.0, 0.85, 0.70);
    const Vec3 up(0.15, 0.35, 0.65);
    return down * (1.0 - t) + up * t;
}

// Deterministic 32-bit mixer (splitmix-style): stable per-sphere colors and
// jitter without any per-frame randomness or shared RNG state.
[[nodiscard]] uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

[[nodiscard]] double hash01(uint32_t x) {
    return static_cast<double>(hash32(x)) / 4294967296.0;
}

// Maps a hue in [0,1) to a soft, saturated color (HSL with s = l = 0.5,
// lifted slightly toward white so Lambertian shading has headroom).
[[nodiscard]] Vec3 hueToColor(double hue) {
    const double hp = hue * 6.0;
    const double c = 1.0;
    const double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));

    double r = 0.0, g = 0.0, b = 0.0;
    if      (hp < 1.0) { r = c; g = x; }
    else if (hp < 2.0) { r = x; g = c; }
    else if (hp < 3.0) { g = c; b = x; }
    else if (hp < 4.0) { g = x; b = c; }
    else if (hp < 5.0) { r = x; b = c; }
    else               { r = c; b = x; }

    const double lift = 0.25;
    return Vec3(r * (1.0 - lift) + lift,
                g * (1.0 - lift) + lift,
                b * (1.0 - lift) + lift);
}

// Appends one sphere — two polar fans plus (kLatSegments - 3) bands of
// quads — to the shared vertex/index buffers. Winding is consistent, and
// the exact orientation is irrelevant: Embree hits both sides of a triangle
// and traceRay() orients the computed normal against the ray.
void appendSphere(const Vec3& center,
                  double radius,
                  std::vector<float>& positions,
                  std::vector<uint32_t>& indices) {
    const int lon = kLonSegments;
    const int lat = kLatSegments;
    const int rings = lat - 2; // middle rings, strictly between the poles

    const uint32_t base   = static_cast<uint32_t>(positions.size() / 3);
    const uint32_t north  = base;
    const uint32_t south  = base + 1;
    const uint32_t ring0  = base + 2;

    // Poles.
    positions.push_back(static_cast<float>(center.x));
    positions.push_back(static_cast<float>(center.y + radius));
    positions.push_back(static_cast<float>(center.z));
    positions.push_back(static_cast<float>(center.x));
    positions.push_back(static_cast<float>(center.y - radius));
    positions.push_back(static_cast<float>(center.z));

    // Middle rings (phi strictly between 0 and PI).
    for (int i = 1; i <= lat - 2; ++i) {
        const double phi  = M_PI * static_cast<double>(i) / static_cast<double>(lat);
        const double sinP = std::sin(phi);
        const double cosP = std::cos(phi);
        for (int j = 0; j < lon; ++j) {
            const double theta = 2.0 * M_PI * static_cast<double>(j) / static_cast<double>(lon);
            positions.push_back(static_cast<float>(center.x + radius * std::cos(theta) * sinP));
            positions.push_back(static_cast<float>(center.y + radius * cosP));
            positions.push_back(static_cast<float>(center.z + radius * std::sin(theta) * sinP));
        }
    }

    const auto ringVertex = [&rings, ring0, lon](int r, int j) -> uint32_t {
        return ring0 + static_cast<uint32_t>(r * lon + (j + lon) % lon);
    };

    // Top fan.
    for (int j = 0; j < lon; ++j) {
        indices.push_back(north);
        indices.push_back(ringVertex(0, j + 1));
        indices.push_back(ringVertex(0, j));
    }
    // Middle bands: each quad split into two triangles.
    for (int r = 0; r < rings - 1; ++r) {
        for (int j = 0; j < lon; ++j) {
            const uint32_t a = ringVertex(r, j);
            const uint32_t b = ringVertex(r, j + 1);
            const uint32_t c = ringVertex(r + 1, j);
            const uint32_t d = ringVertex(r + 1, j + 1);
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    // Bottom fan.
    for (int j = 0; j < lon; ++j) {
        indices.push_back(south);
        indices.push_back(ringVertex(rings - 1, j));
        indices.push_back(ringVertex(rings - 1, j + 1));
    }
}

// Triangles per sphere: lon (top fan) + (rings - 1) * lon * 2 (bands)
// + lon (bottom fan).
constexpr std::size_t kTrianglesPerSphere =
    static_cast<std::size_t>(kLonSegments) * (2 * kLatSegments - 4);

[[nodiscard]] std::size_t verticesPerSphere() {
    return 2 + static_cast<std::size_t>(kLatSegments - 2) * kLonSegments;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction: build the procedural geometry, upload it, commit the BVH.
//
// Every failure path releases what was allocated and throws, so a Scene
// object that exists is always fully usable.
// ---------------------------------------------------------------------------
Scene::Scene() {
    auto cleanup = [this]() {
        if (m_scene != nullptr) {
            rtcReleaseScene(m_scene);
            m_scene = nullptr;
        }
        if (m_device != nullptr) {
            rtcReleaseDevice(m_device);
            m_device = nullptr;
        }
    };
    // Reports the device error, releases everything, and builds the exception.
    auto fail = [this, &cleanup](const char* what) -> std::runtime_error {
        const int code = static_cast<int>(rtcGetDeviceError(m_device));
        cleanup();
        return std::runtime_error(std::string("Embree: ") + what +
                                  " (device error " + std::to_string(code) + ")");
    };

    m_device = rtcNewDevice(nullptr);
    if (m_device == nullptr) {
        throw std::runtime_error("Embree: rtcNewDevice() failed");
    }

    m_scene = rtcNewScene(m_device);
    if (m_scene == nullptr) {
        cleanup();
        throw std::runtime_error("Embree: rtcNewScene() failed");
    }

    // ---- Procedural geometry: a dense field of intersecting spheres ----
    const int n = kGridSize;
    m_sphereCount = static_cast<std::size_t>(n) * n * n;

    std::vector<float>    positions;
    std::vector<uint32_t> indices;
    positions.reserve(m_sphereCount * verticesPerSphere() * 3);
    indices.reserve(m_sphereCount * kTrianglesPerSphere * 3);

    const double halfGrid = 0.5 * static_cast<double>(n - 1);
    uint32_t sphereId = 0;
    for (int iz = 0; iz < n; ++iz) {
        for (int iy = 0; iy < n; ++iy) {
            for (int ix = 0; ix < n; ++ix, ++sphereId) {
                const double u = hash01(sphereId * 3 + 0);
                const double v = hash01(sphereId * 3 + 1);
                const double w = hash01(sphereId * 3 + 2);
                // Slight position jitter so the field is not a perfect lattice.
                const Vec3 center = kFieldCenter
                    + Vec3(ix - halfGrid, iy - halfGrid, iz - halfGrid) * kSpacing
                    + Vec3(u * 2.0 - 1.0, v * 2.0 - 1.0, w * 2.0 - 1.0) * (kSpacing * 0.18);
                const double radius = kBaseRadius * (0.85 + 0.45 * hash01(sphereId * 7 + 5));
                appendSphere(center, radius, positions, indices);
            }
        }
    }

    m_triangleCount = indices.size() / 3;
    const std::size_t vertexCount   = positions.size() / 3;
    const std::size_t triangleCount = indices.size() / 3;

    // ---- Upload to Embree ----
    // v4 API: rtcSetNewGeometryBuffer() *allocates* the buffer and returns
    // a pointer to fill; Embree consumes the data at commit time.
    RTCGeometry geometry = rtcNewGeometry(m_device, RTC_GEOMETRY_TYPE_TRIANGLE);
    if (geometry == nullptr) {
        throw fail("rtcNewGeometry() failed");
    }

    float* vertices = static_cast<float*>(
        rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0,
                                RTC_FORMAT_FLOAT3, 3 * sizeof(float), vertexCount));
    if (vertices == nullptr) {
        throw fail("rtcSetNewGeometryBuffer(VERTEX) failed");
    }
    std::memcpy(vertices, positions.data(), positions.size() * sizeof(float));

    unsigned* triangleIndices = static_cast<unsigned*>(
        rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0,
                                RTC_FORMAT_UINT3, 3 * sizeof(unsigned), triangleCount));
    if (triangleIndices == nullptr) {
        throw fail("rtcSetNewGeometryBuffer(INDEX) failed");
    }
    std::memcpy(triangleIndices, indices.data(), indices.size() * sizeof(uint32_t));

    rtcCommitGeometry(geometry);

    // Attach transfers scene ownership of the geometry; drop our handle.
    rtcAttachGeometry(m_scene, geometry);
    rtcReleaseGeometry(geometry);

    // Builds the BVH over the committed geometry (default MEDIUM quality).
    rtcCommitScene(m_scene);
    if (rtcGetDeviceError(m_device) != RTC_ERROR_NONE) {
        throw fail("BVH build (rtcCommitScene) failed");
    }

    // From here on the scene is committed and read-only; intersection
    // queries are thread-safe.
}

Scene::~Scene() {
    if (m_scene != nullptr) {
        rtcReleaseScene(m_scene);
        m_scene = nullptr;
    }
    if (m_device != nullptr) {
        rtcReleaseDevice(m_device);
        m_device = nullptr;
    }
}

// ---------------------------------------------------------------------------
// traceRay: nearest-hit query via rtcIntersect1, a hard-shadow occlusion
// test via rtcOccluded1, then Lambertian shading.
//
// Every shaded pixel therefore costs two BVH traversals (primary ray +
// shadow ray) — that doubled traversal workload is the point of this
// benchmark.
//
// Thread-safety: the scene is committed (read-only) and every ray/hit
// struct below lives in caller-local stack storage, so any number of
// worker threads may call this concurrently — exactly the usage pattern
// Embree documents for its ray queries.
// ---------------------------------------------------------------------------
Vec3 Scene::traceRay(const Ray& ray) const {
    RTCRayHit rayhit{};
    rayhit.ray.org_x  = static_cast<float>(ray.origin.x);
    rayhit.ray.org_y  = static_cast<float>(ray.origin.y);
    rayhit.ray.org_z  = static_cast<float>(ray.origin.z);
    rayhit.ray.tnear  = 0.0f; // forward-only: ignore geometry behind the camera
    rayhit.ray.dir_x  = static_cast<float>(ray.direction.x);
    rayhit.ray.dir_y  = static_cast<float>(ray.direction.y);
    rayhit.ray.dir_z  = static_cast<float>(ray.direction.z);
    rayhit.ray.time   = 0.0f;
    rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
    rayhit.ray.mask   = 0xFFFFFFFFu; // all geometry
    rayhit.ray.id     = 0;
    rayhit.ray.flags  = 0;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID; // miss sentinel, as in Embree's own tests
    rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.instID[0]     = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.instPrimID[0] = RTC_INVALID_GEOMETRY_ID;

    rtcIntersect1(m_scene, &rayhit);

    if (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return backgroundColor(ray.direction);
    }

    // ---- Geometric (flat) normal of the hit triangle ----
    // The intersection kernel provides it directly; orient it against the
    // ray so double-sided hits shade correctly.
    Vec3 normal(static_cast<double>(rayhit.hit.Ng_x),
                static_cast<double>(rayhit.hit.Ng_y),
                static_cast<double>(rayhit.hit.Ng_z));
    if (normal.dot(ray.direction) > 0.0) {
        normal = normal * -1.0; // face the incoming ray
    }

    // ---- Per-sphere base color ----
    // Triangles were generated in sphere-major order with a fixed count per
    // sphere, so the sphere id is a simple division.
    const std::size_t tri      = rayhit.hit.primID;
    const std::size_t sphereId = tri / kTrianglesPerSphere;
    const double hue = std::fmod(hash01(static_cast<uint32_t>(sphereId)) * 0.6180339887, 1.0);
    const Vec3 base = hueToColor(hue);

    // ---- Hard shadow: occlusion test toward the light ----
    // Fire a shadow ray from the hit point straight at kLightDir and let
    // Embree decide whether anything blocks it (rtcOccluded1: an any-hit
    // query — no hit data is needed, just "did something get in the
    // way?"). This is a second full BVH traversal per pixel.
    //
    // Self-intersection guard (shadow acne): the origin is nudged along
    // the hit normal so the shadow ray does not hit the very triangle the
    // primary ray just struck, and tnear is set to the same bias.
    constexpr float kShadowBias = 0.001f;

    // Hit point. This Embree version reports the hit distance in ray.tfar
    // (RTCHit carries no t member here).
    const float hx = rayhit.ray.org_x + rayhit.ray.tfar * rayhit.ray.dir_x;
    const float hy = rayhit.ray.org_y + rayhit.ray.tfar * rayhit.ray.dir_y;
    const float hz = rayhit.ray.org_z + rayhit.ray.tfar * rayhit.ray.dir_z;

    RTCRay shadow_ray{};
    shadow_ray.org_x  = hx + static_cast<float>(normal.x) * kShadowBias;
    shadow_ray.org_y  = hy + static_cast<float>(normal.y) * kShadowBias;
    shadow_ray.org_z  = hz + static_cast<float>(normal.z) * kShadowBias;
    shadow_ray.tnear  = kShadowBias;
    shadow_ray.dir_x  = static_cast<float>(kLightDir.x);
    shadow_ray.dir_y  = static_cast<float>(kLightDir.y);
    shadow_ray.dir_z  = static_cast<float>(kLightDir.z);
    shadow_ray.time   = 0.0f;
    shadow_ray.tfar   = std::numeric_limits<float>::infinity();
    shadow_ray.mask   = 0xFFFFFFFFu; // every primitive may occlude
    shadow_ray.id     = 0;
    shadow_ray.flags  = 0;

    RTCOccludedArguments shadow_args{};
    rtcInitOccludedArguments(&shadow_args); // incoherent traversal, no callbacks
    rtcOccluded1(m_scene, &shadow_ray, &shadow_args);

    // Embree signals occlusion by driving tfar negative (neg_inf); an
    // unoccluded ray keeps its +inf tfar.
    const bool inShadow = shadow_ray.tfar < 0.0f;

    // ---- Simple Lambertian shading: ambient + diffuse ----
    // Deliberately brighter than a physical Lambert term: a strong diffuse
    // multiplier pushes the lit side past 1.0 for a vivid look, while a
    // shadowed point receives only the ambient floor.
    const double ndl   = normal.dot(kLightDir);
    const double shade = inShadow ? 0.25 : 0.5 + 1.5 * (ndl > 0.0 ? ndl : 0.0);

    // Clamp each channel to [0,1] before the 8-bit PPM conversion.
    return Vec3(std::clamp(base.x * shade, 0.0, 1.0),
                std::clamp(base.y * shade, 0.0, 1.0),
                std::clamp(base.z * shade, 0.0, 1.0));
}
