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
// A kGridSize^3 grid of non-intersecting spheres (2r = 0.17 < kSpacing = 0.32,
// and even with ±kPosJitter the closest pair of centers stays ~0.256 apart,
// more than the sum of two radii) plus a large planar floor disk at y = 0.
// Both are tessellated into triangle meshes and uploaded as separate Embree
// geometries — the floor first (geomID 0), the sphere field second (geomID 1)
// — so Embree builds one BVH over ~10.5M triangles: the heavy workload behind
// traceRay().
// ---------------------------------------------------------------------------
constexpr int   kGridSize    = 28;    // spheres per axis (28^3 = 21 952 spheres)
constexpr float kSpacing     = 0.32f; // center-to-center distance
constexpr float kBaseRadius  = 0.085f; // sphere radius (2r < spacing: no intersection)
constexpr float kPosJitter   = 0.032f; // ± position jitter (still non-intersecting)
constexpr int   kLonSegments = 20;    // longitude segments per sphere
constexpr int   kLatSegments = 14;    // latitude bands per sphere
constexpr Vec3  kFieldCenter{0.0, 6.0, -11.0}; // in front of the camera

// Floor: a planar disk at y = 0.
constexpr double kFloorRadius   = 200.0;
constexpr int    kFloorSegments = 128;

// ---------------------------------------------------------------------------
// Dusk sky — the only light source in the scene.
// ---------------------------------------------------------------------------
const Vec3 kSunDir = Vec3(0.45, 0.14, -0.88).normalize(); // low, in front of the field

[[nodiscard]] Vec3 skyColor(const Vec3& dir) {
    const double t = std::clamp(dir.y, 0.0, 1.0); // 0 = horizon, 1 = zenith
    const Vec3 horizon(1.00, 0.60, 0.38);
    const Vec3 zenith(0.05, 0.13, 0.33);
    Vec3 sky = horizon * (1.0 - t) + zenith * t;

    // Sun: a tight core plus a broad glow.
    const double cosSun = std::max(0.0, dir.dot(kSunDir));
    const double sun = std::pow(cosSun, 900.0) * 3.0 + std::pow(cosSun, 8.0) * 0.35;
    return sky + Vec3(1.0, 0.55, 0.25) * sun;
}

// ---------------------------------------------------------------------------
// Deterministic 32-bit mixer (splitmix-style): stable per-sphere colors and
// material assignment without any per-frame randomness or shared RNG state.
// ---------------------------------------------------------------------------
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
// lifted slightly toward white so shading has headroom).
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

// ---------------------------------------------------------------------------
// Materials.
// ---------------------------------------------------------------------------
enum class MaterialType { Matte, Metal, Glass };

struct Material {
    MaterialType type = MaterialType::Matte;
    Vec3  albedo{1.0, 1.0, 1.0};
    float roughness = 0.0f; // Metal only: random fuzz added to the reflection
};

// The floor: a dark, nearly mirror-smooth metal.
constexpr Material kFloorMaterial{MaterialType::Metal, Vec3(0.12, 0.12, 0.13), 0.08f};

// Sphere metals: a bit rougher than the floor for visual variety.
constexpr float kSphereMetalRoughness = 0.2f;

// 10% glass, 40% metal, 50% matte — decided by the sphere id hash.
[[nodiscard]] Material sphereMaterial(uint32_t sphereId, const Vec3& baseColor) {
    const double roll = hash01(sphereId);
    if (roll < 0.10) {
        return {MaterialType::Glass, baseColor, 0.0f};
    }
    if (roll < 0.50) {
        return {MaterialType::Metal, baseColor, kSphereMetalRoughness};
    }
    return {MaterialType::Matte, baseColor, 0.0f};
}

// ---------------------------------------------------------------------------
// Sampling & shading math (unit vectors, double precision).
// ---------------------------------------------------------------------------

// Uniform random direction on the unit sphere.
[[nodiscard]] Vec3 randomUnitVector(XorShift32& rng) {
    const double c = 2.0 * rng.nextFloat() - 1.0;
    const double a = 2.0 * M_PI * rng.nextFloat();
    const double s = std::sqrt(std::max(0.0, 1.0 - c * c));
    return {s * std::cos(a), c, s * std::sin(a)};
}

// Cosine-weighted direction in the hemisphere about n. The sampling weight
// cancels the cosine of the BRDF, so the complete BSDF term is just albedo.
[[nodiscard]] Vec3 cosineHemisphere(const Vec3& n, XorShift32& rng) {
    const double u = 2.0 * M_PI * rng.nextFloat();
    const double r = std::sqrt(rng.nextFloat());
    const Vec3 local = {r * std::cos(u), r * std::sin(u), std::sqrt(std::max(0.0, 1.0 - r * r))};

    const Vec3 any = (std::fabs(n.y) < 0.999) ? Vec3(0.0, 1.0, 0.0) : Vec3(1.0, 0.0, 0.0);
    const Vec3 t1 = any.cross(n).normalize();
    const Vec3 t2 = n.cross(t1);
    return local.x * t1 + local.y * t2 + local.z * n;
}

// Specular reflection of unit d about unit n.
[[nodiscard]] Vec3 reflectVec(const Vec3& d, const Vec3& n) {
    return d - 2.0 * d.dot(n) * n;
}

// Standard refraction: i is a unit direction, n is a unit normal oriented
// against i, eta is the relative index of refraction (n1/n2). Returns a
// zero vector on total internal reflection.
[[nodiscard]] Vec3 refractVec(const Vec3& i, const Vec3& n, double eta) {
    const double c  = std::min(1.0, -i.dot(n));
    const double s2 = eta * eta * (1.0 - c * c);
    if (s2 >= 1.0) {
        return {};
    }
    return eta * i + (eta * c - std::sqrt(1.0 - s2)) * n;
}

// Fresnel–Schlick (Duff, "Notes on Rendering").
[[nodiscard]] double schlickFresnel(double cosI, double cosT, double eta) {
    const double r0 = (1.0 - eta) / (1.0 + eta);
    const double r1 = eta * eta * (cosI * cosI - cosT * cosT) / (cosI * cosI + cosT * cosT);
    return r0 + r1 * std::pow(1.0 - cosI, 5.0);
}

// ---------------------------------------------------------------------------
// Mesh construction.
// ---------------------------------------------------------------------------

// Appends a planar disk at y = 0 as a triangle fan: one center vertex plus
// `segments` ring vertices → `segments` triangles. Embree hits both sides of
// a triangle, so winding is irrelevant — the normal is oriented against the
// ray in traceRay().
void appendFloorDisk(double radius,
                     int segments,
                     std::vector<float>& positions,
                     std::vector<uint32_t>& indices) {
    const uint32_t center = static_cast<uint32_t>(positions.size() / 3);
    positions.push_back(0.0f);
    positions.push_back(0.0f);
    positions.push_back(0.0f);
    for (int i = 0; i < segments; ++i) {
        const double a = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(segments);
        positions.push_back(static_cast<float>(radius * std::cos(a)));
        positions.push_back(0.0f);
        positions.push_back(static_cast<float>(radius * std::sin(a)));
    }
    for (int i = 0; i < segments; ++i) {
        indices.push_back(center);
        indices.push_back(center + 1u + static_cast<uint32_t>(i));
        indices.push_back(center + 1u + static_cast<uint32_t>((i + 1) % segments));
    }
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

    // Uploads a triangle-mesh buffer pair to Embree and attaches it to the
    // scene. Attachment order is the geometry id: the floor is attached
    // first (geomID 0), the sphere field second (geomID 1).
    auto attachMesh = [this, &fail](const std::vector<float>& positions,
                                    const std::vector<uint32_t>& indices) {
        const std::size_t vertexCount   = positions.size() / 3;
        const std::size_t triangleCount = indices.size() / 3;

        RTCGeometry geometry = rtcNewGeometry(m_device, RTC_GEOMETRY_TYPE_TRIANGLE);
        if (geometry == nullptr) {
            throw fail("rtcNewGeometry() failed");
        }

        // v4 API: rtcSetNewGeometryBuffer() *allocates* the buffer and
        // returns a pointer to fill; Embree consumes the data at commit.
        float* vertices = static_cast<float*>(
            rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0,
                                    RTC_FORMAT_FLOAT3, 3 * sizeof(float), vertexCount));
        if (vertices == nullptr) {
            rtcReleaseGeometry(geometry);
            throw fail("rtcSetNewGeometryBuffer(VERTEX) failed");
        }
        std::memcpy(vertices, positions.data(), positions.size() * sizeof(float));

        unsigned* triangleIndices = static_cast<unsigned*>(
            rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0,
                                    RTC_FORMAT_UINT3, 3 * sizeof(unsigned), triangleCount));
        if (triangleIndices == nullptr) {
            rtcReleaseGeometry(geometry);
            throw fail("rtcSetNewGeometryBuffer(INDEX) failed");
        }
        std::memcpy(triangleIndices, indices.data(), indices.size() * sizeof(uint32_t));

        rtcCommitGeometry(geometry);

        // Attach transfers scene ownership of the geometry; drop our handle.
        rtcAttachGeometry(m_scene, geometry);
        rtcReleaseGeometry(geometry);
    };

    // ---- geomID 0: the floor disk (planar fan at y = 0) ----
    {
        std::vector<float>    floorPositions;
        std::vector<uint32_t> floorIndices;
        appendFloorDisk(kFloorRadius, kFloorSegments, floorPositions, floorIndices);
        attachMesh(floorPositions, floorIndices);
        m_triangleCount += floorIndices.size() / 3;
    }

    // ---- geomID 1: the sphere field ----
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
                // Slight position jitter so the field is not a perfect lattice
                // — the worst case stays non-intersecting (see header math).
                const Vec3 center = kFieldCenter
                    + Vec3(ix - halfGrid, iy - halfGrid, iz - halfGrid) * kSpacing
                    + Vec3(u * 2.0 - 1.0, v * 2.0 - 1.0, w * 2.0 - 1.0) * kPosJitter;
                appendSphere(center, kBaseRadius, positions, indices);
            }
        }
    }

    attachMesh(positions, indices);
    m_triangleCount += indices.size() / 3;

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
// traceRay: a path tracer with at most 4 bounces.
//
// Every bounce resolves the nearest hit via rtcIntersect1, then samples the
// local BRDF:
//   * Matte — cosine-weighted hemisphere (Lambert sampling);
//   * Metal — specular reflection plus a random fuzz scaled by roughness;
//   * Glass — Schlick fresnel decides reflection vs refraction (IOR 1.5,
//     total internal reflection when the sine of the refraction angle
//     overflows).
// The light is entirely the analytic dusk sky (gradient + sun), so the loop
// accumulates throughput * sky on the first miss. Paths whose throughput
// drops below 0.001 in any relevant sense are cut early.
//
// Thread-safety: the scene is committed (read-only) and every ray/hit struct
// lives in caller-local stack storage, so any number of worker threads may
// call this concurrently — exactly the usage pattern Embree documents.
// ---------------------------------------------------------------------------
Vec3 Scene::traceRay(const Ray& ray, XorShift32& rng) const {
    constexpr int    kMaxBounces    = 4;
    constexpr double kRayBias       = 1e-4;  // self-intersection guard
    constexpr double kMinThroughput = 0.001; // early-exit threshold
    constexpr double kGlassIOR      = 1.5;

    Vec3 throughput{1.0, 1.0, 1.0}; // running product of BSDF terms
    Vec3 radiance{};                // accumulated radiance
    Ray  curRay = ray;              // local, mutable copy (the parameter is const)

    for (int bounce = 0; bounce < kMaxBounces; ++bounce) {
        RTCRayHit rayhit{};
        rayhit.ray.org_x  = static_cast<float>(curRay.origin.x);
        rayhit.ray.org_y  = static_cast<float>(curRay.origin.y);
        rayhit.ray.org_z  = static_cast<float>(curRay.origin.z);
        rayhit.ray.tnear  = static_cast<float>(kRayBias); // skip the surface we just left
        rayhit.ray.dir_x  = static_cast<float>(curRay.direction.x);
        rayhit.ray.dir_y  = static_cast<float>(curRay.direction.y);
        rayhit.ray.dir_z  = static_cast<float>(curRay.direction.z);
        rayhit.ray.time   = 0.0f;
        rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
        rayhit.ray.mask   = 0xFFFFFFFFu; // all geometry
        rayhit.ray.id     = 0;
        rayhit.ray.flags  = 0;
        rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID; // miss sentinel
        rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;
        rayhit.hit.instID[0]     = RTC_INVALID_GEOMETRY_ID;
        rayhit.hit.instPrimID[0] = RTC_INVALID_GEOMETRY_ID;

        rtcIntersect1(m_scene, &rayhit);

        if (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            // Miss: collect the sky along this direction.
            radiance = radiance + throughput * skyColor(curRay.direction);
            break;
        }

        // ---- Geometric (flat) normal, oriented against the ray ----
        Vec3 n(static_cast<double>(rayhit.hit.Ng_x),
               static_cast<double>(rayhit.hit.Ng_y),
               static_cast<double>(rayhit.hit.Ng_z));
        // Before the flip: a ray hitting the outside of glass comes in from
        // the air (eta = 1/IOR); from inside it exits (eta = IOR).
        const bool entering = n.dot(curRay.direction) < 0.0;
        if (n.dot(curRay.direction) > 0.0) {
            n = n * -1.0; // face the incoming ray
        }

        // ---- Hit point (this Embree build reports the distance in ray.tfar) ----
        const Vec3 p = curRay.origin + curRay.direction * rayhit.ray.tfar;

        // ---- Material: geomID 0 is the floor, geomID 1 the sphere field ----
        Material mat;
        if (rayhit.hit.geomID == 0) {
            mat = kFloorMaterial;
        } else {
            // Triangles were generated in sphere-major order with a fixed
            // count per sphere, so the sphere id is a simple division.
            const uint32_t sphereId =
                static_cast<uint32_t>(rayhit.hit.primID / kTrianglesPerSphere);
            const double hue = std::fmod(hash01(sphereId) * 0.6180339887, 1.0);
            mat = sphereMaterial(sphereId, hueToColor(hue));
        }

        // ---- Sample the local BRDF ----
        Vec3 nextDir{};
        Vec3 bsdf{1.0, 1.0, 1.0};
        switch (mat.type) {
            case MaterialType::Matte:
                nextDir = cosineHemisphere(n, rng);
                bsdf    = mat.albedo;
                break;

            case MaterialType::Metal: {
                Vec3 r = reflectVec(curRay.direction, n);
                if (mat.roughness > 0.0f) {
                    // Reflection + random fuzz; keep the result in front of
                    // the surface when the fuzz flips it behind.
                    r = (r + randomUnitVector(rng) * mat.roughness).normalize();
                    if (r.dot(n) <= 0.0) {
                        r = reflectVec(curRay.direction, n);
                    }
                }
                nextDir = r;
                bsdf    = mat.albedo;
                break;
            }

            case MaterialType::Glass: {
                const double eta   = entering ? (1.0 / kGlassIOR) : kGlassIOR;
                const double cosI  = std::min(1.0, -n.dot(curRay.direction));
                const double sin2T = eta * eta * (1.0 - cosI * cosI);
                if (sin2T >= 1.0) {
                    nextDir = reflectVec(curRay.direction, n); // total internal reflection
                } else {
                    const double cosT = std::sqrt(1.0 - sin2T);
                    const double F    = schlickFresnel(cosI, cosT, eta);
                    if (rng.nextFloat() < F) {
                        nextDir = reflectVec(curRay.direction, n);
                    } else {
                        nextDir = refractVec(curRay.direction, n, eta);
                    }
                }
                bsdf = mat.albedo;
                break;
            }
        }

        // ---- Update the throughput and bail out if the path is dead ----
        throughput = throughput * bsdf;
        if (std::max(throughput.x, std::max(throughput.y, throughput.z)) < kMinThroughput) {
            break;
        }
        if (bounce + 1 == kMaxBounces) {
            break; // bounded depth
        }

        // Continue from just in front of the hit point, along the normal.
        curRay = Ray{p + n * kRayBias, nextDir.normalize()};
    }

    return radiance;
}
