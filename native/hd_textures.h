#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gpu/texture.h"

// HD texture pack: the game's textures are matched by content to the files of "BLACK HD Reimagined" through a map
// (textures/black-hd-reimagined.map, see textures/README.md). Every call is from the vulkan thread.
namespace hd {

// a pack file ready to upload: its levels back to back, block compressed or BGRA
struct Image {
    enum Format : uint8_t { BC1, BC2, BC3, BGRA8 } format = BGRA8;
    uint32_t width = 0, height = 0, levels = 0;
    std::vector<size_t> offset;  // per level
    std::vector<uint8_t> data;
    uint32_t file_width = 0, file_height = 0;  // the file's own size, before levels were left out
};

// the map entry for a guest texture (level 0 at `guest`), -1 for none. Counts hits and misses whatever the setting is;
// -1 too unless Textures is HD and the pack folder is there
int Find(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, const uint8_t* guest);

// the pack file of a map entry, levels wider or taller than max_side left out (0: none are). False when it cannot be used.
bool Load(int entry, uint32_t max_side, Image& out);

// 1 while Textures is HD and a pack is there, else 0: part of what makes a cached upload stale
uint32_t State();

}  // namespace hd
