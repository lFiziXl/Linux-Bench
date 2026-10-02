#include "TileDispatcher.h"

#include <algorithm>
#include <stdexcept>

TileDispatcher::TileDispatcher(int image_width, int image_height, int tile_size) {
    if (image_width <= 0 || image_height <= 0 || tile_size <= 0) {
        throw std::invalid_argument("TileDispatcher: image dimensions and tile size must be positive");
    }

    const auto cols = static_cast<std::size_t>((image_width + tile_size - 1) / tile_size);
    const auto rows = static_cast<std::size_t>((image_height + tile_size - 1) / tile_size);
    m_tiles.reserve(cols * rows);

    for (int y = 0; y < image_height; y += tile_size) {
        for (int x = 0; x < image_width; x += tile_size) {
            m_tiles.push_back(Tile{
                x, y,
                std::min(tile_size, image_width - x),
                std::min(tile_size, image_height - y)
            });
        }
    }
}

std::optional<Tile> TileDispatcher::getNextTile() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_next >= m_tiles.size()) {
        return std::nullopt;
    }
    return m_tiles[m_next++];
}

std::size_t TileDispatcher::tileCount() const noexcept {
    return m_tiles.size();
}

bool TileDispatcher::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_next >= m_tiles.size();
}
