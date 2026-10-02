#pragma once

#include <GL/gl.h> // GLuint (OpenGL entry points used by the GL methods)

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Double-buffered framebuffer for the benchmark.
//
// Concurrency contract: workers write only into the back buffer (m_pixels).
// The TileDispatcher hands out disjoint tiles and each worker only ever
// writes pixels inside the tile it received, so setPixel() needs no
// synchronization. Do NOT relax the tile-disjointness guarantee without
// adding a lock there.
//
// Live preview: updateTexture() copies the back buffer into the front buffer
// (m_pixels_front) under m_front_mutex and uploads that copy to OpenGL, so
// the main thread never reads a pixel a worker is still writing — that is
// what keeps live rendering during a run deadlock-free.
//
// OpenGL contract: getTextureID()/updateTexture() MUST be called from the
// main thread with a current GL context.
class Framebuffer {
public:
    // Throws std::invalid_argument on non-positive dimensions.
    Framebuffer(int width, int height);

    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }

    // Precondition: 0 <= x < width() and 0 <= y < height() (guaranteed by the
    // clamped tile geometry; kept branch-free to stay out of the hot path).
    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

    // Fills the image with black. Call only when no worker is writing
    // (i.e. from the main thread between runs).
    void clear();

    // Writes the image as a binary P6 PPM file.
    // Throws std::runtime_error on I/O failure.
    void savePPM(const std::string& filename) const;

    // --- OpenGL (main thread only, current GL context required) -------------

    // GL texture id; 0 until updateTexture() has created the texture.
    [[nodiscard]] GLuint getTextureID() const noexcept { return m_textureID; }

    // Creates the texture on first call (GL_RGB, 8-bit, linear filtering),
    // then copies the back buffer into the front buffer (under
    // m_front_mutex) and uploads it with glTexSubImage2D. Safe to call
    // every frame, even while workers are still writing; throws
    // std::runtime_error on GL failure.
    void updateTexture();

private:
    int                  m_width;
    int                  m_height;
    std::vector<uint8_t> m_pixels;       // back buffer: RGB, 3 bytes per pixel, row-major: (y * width + x) * 3
    std::vector<uint8_t> m_pixels_front; // front buffer: latest back-buffer copy, uploaded to OpenGL

    mutable std::mutex   m_front_mutex;  // guards the back-to-front copy in updateTexture()

    GLuint m_textureID    = 0;
    bool   m_textureReady = false; // true once glTexImage2D allocated storage
};
