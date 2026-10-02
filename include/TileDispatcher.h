#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

// A rectangular block of the image that a worker has to render.
struct Tile {
    int x;      // Left edge in pixels
    int y;      // Top edge in pixels
    int width;  // Width in pixels (may be smaller than the nominal tile size at the border)
    int height; // Height in pixels (may be smaller than the nominal tile size at the border)
};

// Divides an image into fixed-size tiles and dispenses them to worker threads
// one at a time. All public members are safe to use from multiple threads.
class TileDispatcher {
public:
    // Divides an image_width x image_height image into tile_size x tile_size
    // tiles. Edge tiles are clamped so the whole image is covered exactly once.
    // Throws std::invalid_argument on non-positive dimensions.
    TileDispatcher(int image_width, int image_height, int tile_size);

    // Returns the next unclaimed tile, or std::nullopt once every tile has been
    // dispensed. Workers loop on this call until it returns std::nullopt.
    std::optional<Tile> getNextTile() const;

    // True once every tile has been dispensed (i.e. getNextTile() would
    // return std::nullopt). Thread-safe like the rest of the interface;
    // used by ThreadPool to wait for a fully drained frame.
    [[nodiscard]] bool empty() const;

    // Total number of tiles in the image (fixed after construction).
    [[nodiscard]] std::size_t tileCount() const noexcept;

private:
    std::vector<Tile>    m_tiles;  // Precomputed work queue (immutable after construction)
    mutable std::size_t  m_next = 0; // Index of the next free tile
    mutable std::mutex   m_mutex;  // Guards m_next
};
