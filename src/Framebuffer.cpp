#include "Framebuffer.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <stdexcept>

Framebuffer::Framebuffer(int width, int height)
    : m_width(width), m_height(height) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Framebuffer: dimensions must be positive");
    }
    m_pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u, 0u);
    m_pixels_front.resize(m_pixels.size(), 0u);
}

void Framebuffer::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    // Single pointer chase, three stores. No bounds check by design (see header).
    auto* p = m_pixels.data() + (static_cast<std::size_t>(y) * m_width + static_cast<std::size_t>(x)) * 3u;
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

void Framebuffer::clear() {
    std::fill(m_pixels.begin(), m_pixels.end(), 0);
}

void Framebuffer::savePPM(const std::string& filename) const {
    std::ofstream out(filename, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Framebuffer::savePPM: cannot open '" + filename + "' for writing");
    }

    // P6 = binary RGB. Header must stay under the 70-char line limit.
    out << "P6\n" << m_width << ' ' << m_height << "\n255\n";

    // One bulk write instead of per-pixel writes: a 1920x1080 image is a
    // single 6.2 MB memcpy into the stream buffer.
    out.write(reinterpret_cast<const char*>(m_pixels.data()),
              static_cast<std::streamsize>(m_pixels.size()));
    out.flush();

    if (!out) {
        throw std::runtime_error("Framebuffer::savePPM: failed writing '" + filename + "'");
    }
}

void Framebuffer::updateTexture() {
    if (m_textureID == 0) {
        glGenTextures(1, &m_textureID);
        if (m_textureID == 0) {
            throw std::runtime_error("Framebuffer::updateTexture: glGenTextures failed");
        }
    }

    glBindTexture(GL_TEXTURE_2D, m_textureID);

    if (!m_textureReady) {
        // Allocate storage and set sampling state once; the contents are
        // refreshed on every call with glTexSubImage2D below.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_width, m_height, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        m_textureReady = true;
    }

    // Publish the back buffer into the front buffer, then upload the copy:
    // the GL read below never races the worker writes into m_pixels.
    {
        std::lock_guard<std::mutex> lock(m_front_mutex);
        std::copy(m_pixels.begin(), m_pixels.end(), m_pixels_front.begin());
    }

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height,
                    GL_RGB, GL_UNSIGNED_BYTE, m_pixels_front.data());

    glBindTexture(GL_TEXTURE_2D, 0); // Leave the global texture binding as we found it.
}
