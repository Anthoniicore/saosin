// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_BOOKMARK_HPP
#define CHIMERA_BOOKMARK_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>

namespace Chimera {
    struct Bookmark {
        char address[64];
        std::uint16_t port;
        char password[9];
        bool brackets;
    };

    struct QueryPacketDone {
        enum Error {
            NONE = 0,
            FAILED_TO_RESOLVE,
            TIMED_OUT
        };

        Bookmark b;
        std::unordered_map<std::string, std::string> query_data;
        bool timed_out;
        Error error;
        std::size_t ping = 0;

        const char *get_data_for_key(const char *key) {
            for(auto &q : this->query_data) {
                if(q.first == key) {
                    return q.second.data();
                }
            }
            return nullptr;
        }
    };

    QueryPacketDone query_server(const Bookmark &what);
    const Bookmark &get_latest_connection() noexcept;
    void set_up_server_history() noexcept;
    std::vector<Bookmark> load_bookmarks_file(const char *file) noexcept;
    void save_bookmarks_file(const char *file, const std::vector<Bookmark> &bookmarks) noexcept;
}

#endif
