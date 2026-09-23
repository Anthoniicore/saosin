// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_MAP_LOADING_HPP
#define CHIMERA_MAP_LOADING_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Chimera {
    struct LoadedMap {
        std::string name;
        std::filesystem::path path;
        std::optional<std::uint32_t> tmp_file;
        std::optional<std::byte *> memory_location;
        std::size_t buffer_size;
        std::size_t decompressed_size;
        std::size_t loaded_size;
        std::size_t file_size;
        std::filesystem::file_time_type timestamp;
    };

    LoadedMap *get_loaded_map(const char *name) noexcept;
    void set_up_map_loading();
    LoadedMap *load_map(const char *map_name);
}
#endif
