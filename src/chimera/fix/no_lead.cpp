// SPDX-License-Identifier: GPL-3.0-only

#include <cstddef>
#include <cstdint>
#include <windows.h>

#include "../chimera.hpp"
#include "../event/tick.hpp"
#include "../halo_data/multiplayer.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/player.hpp"
#include "../signature/hook.hpp"

#include "no_lead.hpp"

namespace Chimera {
    namespace {
        constexpr std::uintptr_t HALO_IMAGE_BASE = 0x00400000u;

        // Halo CE 1.10.10.0621 / haloceded.exe.
        // These are the same server-side functions used by the historical
        // Halo CE no-lead implementation.
        constexpr std::uintptr_t UPDATE_OBJECT_ADDR = 0x004E2AD0u;
        constexpr std::uintptr_t UPDATE_PHYSICS_ADDR = 0x004E2EF0u;

        // Candidate ownership fields in the Halo object header. 0x50/0x54
        // are part of the otherwise undocumented base-object area in the
        // current Saosin layout. We accept a candidate only when it matches
        // a live player's current biped object ID.
        constexpr std::size_t OWNER_OFFSET_A = 0x50;
        constexpr std::size_t OWNER_OFFSET_B = 0x54;

        constexpr std::size_t MAX_PLAYERS = 16;
        constexpr std::size_t HISTORY_SIZE = 128;

        struct HistoricalPosition {
            bool valid = false;
            ObjectID object_id = HaloID::null_id();
            Point3D position {};
            Point3D center {};
        };

        using UpdateObjectFunction = char (*)(ObjectID);
        using UpdatePhysicsFunction = char (*)(ObjectID);

        static HistoricalPosition history[MAX_PLAYERS][HISTORY_SIZE] = {};
        static std::size_t history_head = 0;
        static bool history_started = false;

        static bool enabled = true;
        static bool hooked = false;

        static Hook update_object_hook;
        static const void *original_update_object_ptr = nullptr;

        static void *absolute_address(std::uintptr_t address) noexcept {
            auto module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandle(nullptr));
            if(module_base == 0 || address < HALO_IMAGE_BASE) {
                return nullptr;
            }

            return reinterpret_cast<void *>(module_base + (address - HALO_IMAGE_BASE));
        }

        static UpdateObjectFunction original_update_object() noexcept {
            return reinterpret_cast<UpdateObjectFunction>(
                const_cast<std::byte *>(
                    reinterpret_cast<const std::byte *>(original_update_object_ptr)
                )
            );
        }

        static UpdatePhysicsFunction update_physics() noexcept {
            return reinterpret_cast<UpdatePhysicsFunction>(absolute_address(UPDATE_PHYSICS_ADDR));
        }

        static void reset_player_history(std::size_t player_index) noexcept {
            for(std::size_t i = 0; i < HISTORY_SIZE; i++) {
                history[player_index][i].valid = false;
                history[player_index][i].object_id = HaloID::null_id();
            }
        }

        static void capture_history() noexcept {
            if(server_type() != ServerType::SERVER_DEDICATED) {
                return;
            }

            auto &player_table = PlayerTable::get_player_table();
            auto &object_table = ObjectTable::get_object_table();

            for(std::size_t i = 0; i < MAX_PLAYERS; i++) {
                auto &sample = history[i][history_head];
                sample.valid = false;
                sample.object_id = HaloID::null_id();

                if(i >= player_table.current_size) {
                    reset_player_history(i);
                    continue;
                }

                auto &player = player_table.first_element[i];
                if(player.player_id == 0xFFFF || player.object_id.is_null()) {
                    reset_player_history(i);
                    continue;
                }

                auto *object = object_table.get_dynamic_object(player.object_id);
                if(!object ||
                   object->type != ObjectType::OBJECT_TYPE_BIPED ||
                   !object->parent.is_null()) {
                    reset_player_history(i);
                    continue;
                }

                sample.valid = true;
                sample.object_id = player.object_id;
                sample.position = object->position;
                sample.center = object->center_position;
            }

            history_head = (history_head + 1) % HISTORY_SIZE;
            history_started = true;
        }

        static std::size_t ping_to_history_ticks(std::uint32_t ping_ms) noexcept {
            // Remote-position compensation is expressed in Halo ticks.
            // Round upward so the rewind never undershoots the requested
            // network delay.
            auto ticks = (static_cast<std::uint64_t>(ping_ms) * 30u + 999u) / 1000u;
            if(ticks >= HISTORY_SIZE - 1) {
                ticks = HISTORY_SIZE - 2;
            }
            return static_cast<std::size_t>(ticks);
        }

        static bool object_id_matches_player(
            ObjectID candidate,
            std::size_t &player_slot,
            std::uint32_t &ping_ms
        ) noexcept {
            if(candidate.is_null()) {
                return false;
            }

            auto &player_table = PlayerTable::get_player_table();
            auto count = player_table.current_size;
            if(count > MAX_PLAYERS) {
                count = MAX_PLAYERS;
            }

            for(std::size_t i = 0; i < count; i++) {
                auto &player = player_table.first_element[i];

                if(
                    player.player_id == 0xFFFF ||
                    player.object_id.is_null() ||
                    player.object_id != candidate
                ) {
                    continue;
                }

                player_slot = i;
                ping_ms = player.ping;
                return true;
            }

            return false;
        }

        static bool find_shooter(
            BaseDynamicObject *updated_object,
            std::size_t &shooter_slot,
            std::uint32_t &shooter_ping
        ) noexcept {
            auto *bytes = reinterpret_cast<std::byte *>(updated_object);

            const auto owner_a = *reinterpret_cast<const ObjectID *>(bytes + OWNER_OFFSET_A);
            if(object_id_matches_player(owner_a, shooter_slot, shooter_ping)) {
                return true;
            }

            const auto owner_b = *reinterpret_cast<const ObjectID *>(bytes + OWNER_OFFSET_B);
            if(object_id_matches_player(owner_b, shooter_slot, shooter_ping)) {
                return true;
            }

            // A weapon/projectile can also be parented to the shooter's biped.
            // This path is only accepted when the parent is a live player's
            // current biped, so normal child objects cannot accidentally
            // activate lag compensation.
            if(object_id_matches_player(updated_object->parent, shooter_slot, shooter_ping)) {
                return true;
            }

            return false;
        }

        static bool get_historical_position(
            std::size_t player_slot,
            ObjectID object_id,
            std::size_t delay_ticks,
            HistoricalPosition &out
        ) noexcept {
            if(!history_started || player_slot >= MAX_PLAYERS) {
                return false;
            }

            auto index = (history_head + HISTORY_SIZE - 1 - delay_ticks) % HISTORY_SIZE;
            auto &sample = history[player_slot][index];

            if(!sample.valid || sample.object_id != object_id) {
                return false;
            }

            out = sample;
            return true;
        }

        static char on_update_object(ObjectID object_id) {
            auto original = original_update_object();
            if(!original) {
                return 0;
            }

            if(
                !enabled ||
                !history_started ||
                server_type() != ServerType::SERVER_DEDICATED
            ) {
                return original(object_id);
            }

            auto *updated_object = ObjectTable::get_object_table().get_dynamic_object(object_id);
            if(!updated_object) {
                return original(object_id);
            }

            std::size_t shooter_slot = 0;
            std::uint32_t shooter_ping = 0;
            if(!find_shooter(updated_object, shooter_slot, shooter_ping)) {
                return original(object_id);
            }

            auto delay_ticks = ping_to_history_ticks(shooter_ping);
            if(delay_ticks == 0) {
                return original(object_id);
            }

            auto &player_table = PlayerTable::get_player_table();
            auto &object_table = ObjectTable::get_object_table();
            auto update_physics_fn = update_physics();

            struct RewoundPlayer {
                BaseDynamicObject *object = nullptr;
                ObjectID object_id = HaloID::null_id();
                Point3D position {};
                Point3D center {};
                bool active = false;
            };

            RewoundPlayer rewound[MAX_PLAYERS] = {};

            auto count = player_table.current_size;
            if(count > MAX_PLAYERS) {
                count = MAX_PLAYERS;
            }

            for(std::size_t i = 0; i < count; i++) {
                if(i == shooter_slot) {
                    continue;
                }

                auto &player = player_table.first_element[i];
                if(player.player_id == 0xFFFF || player.object_id.is_null()) {
                    continue;
                }

                auto *target = object_table.get_dynamic_object(player.object_id);
                if(
                    !target ||
                    target->type != ObjectType::OBJECT_TYPE_BIPED ||
                    !target->parent.is_null()
                ) {
                    continue;
                }

                HistoricalPosition historical {};
                if(!get_historical_position(i, player.object_id, delay_ticks, historical)) {
                    continue;
                }

                rewound[i].object = target;
                rewound[i].object_id = player.object_id;
                rewound[i].position = target->position;
                rewound[i].center = target->center_position;
                rewound[i].active = true;

                // Rewind only for the duration of this object update.
                target->position = historical.position;
                target->center_position = historical.center;

                if(update_physics_fn) {
                    update_physics_fn(player.object_id);
                }
            }

            auto result = original(object_id);

            // Restore immediately. The server never keeps the historical
            // position as its authoritative state.
            for(std::size_t i = 0; i < count; i++) {
                if(!rewound[i].active || !rewound[i].object) {
                    continue;
                }

                rewound[i].object->position = rewound[i].position;
                rewound[i].object->center_position = rewound[i].center;

                if(update_physics_fn) {
                    update_physics_fn(rewound[i].object_id);
                }
            }

            return result;
        }
    }

    void set_up_no_lead_fix() noexcept {
        if(
            hooked ||
            server_type() != ServerType::SERVER_DEDICATED
        ) {
            return;
        }

        auto *update_object_address = absolute_address(UPDATE_OBJECT_ADDR);
        if(!update_object_address) {
            enabled = false;
            return;
        }

        write_function_override(
            update_object_address,
            update_object_hook,
            reinterpret_cast<const void *>(on_update_object),
            &original_update_object_ptr
        );

        add_tick_event(capture_history);

        hooked = true;
        enabled = true;
    }

    bool no_lead_enabled() noexcept {
        return enabled && hooked;
    }

    void set_no_lead_enabled(bool new_enabled) noexcept {
        enabled = new_enabled;
    }
}
