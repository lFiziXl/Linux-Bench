#pragma once

#include <cmath>

// Minimal 3D vector for the ray tracer. Header-only, value semantics, no
// dependencies.
//
// Every operation returns a new Vec3. A Vec3 is 24 bytes and copies are a
// single register move, so per-thread use in the renderer is effectively free
// (no sharing, no locking, no aliasing between workers).
//
// Double precision is used deliberately: the analytic sphere test is a
// quadratic solve and the gradient math is scale-sensitive, and on modern
// x86-64 the extra cost of doubles over floats is negligible.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3() = default;
    constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    // Component-wise addition.
    [[nodiscard]] constexpr Vec3 operator+(const Vec3& rhs) const {
        return {x + rhs.x, y + rhs.y, z + rhs.z};
    }

    // Component-wise subtraction.
    [[nodiscard]] constexpr Vec3 operator-(const Vec3& rhs) const {
        return {x - rhs.x, y - rhs.y, z - rhs.z};
    }

    // Scalar multiplication (v * s and s * v).
    [[nodiscard]] constexpr Vec3 operator*(double s) const {
        return {x * s, y * s, z * s};
    }

    // Hadamard (component-wise) product — used to multiply per-channel
    // quantities like BSDF terms into the running path throughput.
    [[nodiscard]] constexpr Vec3 operator*(const Vec3& rhs) const { return {x * rhs.x, y * rhs.y, z * rhs.z}; }

    [[nodiscard]] constexpr double dot(const Vec3& rhs) const {
        return x * rhs.x + y * rhs.y + z * rhs.z;
    }

    // Cross product (right-handed coordinate system).
    [[nodiscard]] constexpr Vec3 cross(const Vec3& rhs) const {
        return {y * rhs.z - z * rhs.y,
                z * rhs.x - x * rhs.z,
                x * rhs.y - y * rhs.x};
    }

    [[nodiscard]] double length() const {
        return std::sqrt(dot(*this));
    }

    // Returns a unit vector in the same direction.
    // Degenerate input (zero length) is returned unchanged.
    [[nodiscard]] Vec3 normalize() const {
        const double len = length();
        if (len <= 0.0) {
            return *this;
        }
        // One reciprocal + one multiply per component instead of a division
        // per component (division is the slow floating-point op).
        return *this * (1.0 / len);
    }

    friend constexpr Vec3 operator*(double s, const Vec3& v) {
        return v * s;
    }
};
