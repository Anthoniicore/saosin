// SPDX-License-Identifier: GPL-3.0-only

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <windows.h>

#include "../chimera.hpp"
#include "../halo_data/multiplayer.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/player.hpp"
#include "../signature/hook.hpp"

#include "no_lead.hpp"

namespace Chimera {
    namespace {
        constexpr std::uintptr_t HALO_IMAGE_BASE = 0x00400000u;
        constexpr std::uintptr_t UPDATE_OBJECT_ADDR = 0x004E2AD0u;
        constexpr std::uintptr_t UPDATE_ALL_OBJECTS_ADDR = 0x004DFB10u;
        constexpr std::uintptr_t UPDATE_PHYSICS_ADDR = 0x004E2EF0u;

        constexpr std::size_t MAX_PLAYERS = 16;
        constexpr std::size_t HISTORY_SIZE = 128;

        struct HistoricalPosition {
            bool valid = false;
            ObjectID object_id = HaloID::null_id();
            Point3D position {};
        };

        using UpdateObjectFunction = char (*)(ObjectID);
        using UpdateAllObjectsFunction = char (*)();
        using UpdatePhysicsFunction = char (*)(ObjectID);

        static HistoricalPosition history[MAX_PLAYERS][HISTORY_SIZE] = {};
        static std::size_t history_head = 0;
        static bool history_started = false;
        static bool enabled = true;
        static bool hooked = false;

        static Hook update_object_hook;
        static Hook update_all_objects_hook;
        static const void *original_update_object_ptr = nullptr;
        static const void *original_update_all_objects_ptr = nullptr;

        static void *server_address(std::uintptr_t absolute_address) noexcept {
            auto module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandle(nullptr));
            if(module_base == 0 || absolute_address < HALO_IMAGE_BASE) {
                return nullptr;
            }
            return reinterpret_cast<void *>(module_base + (absolute_address - HALO_IMAGE_BASE));
        }

        static UpdateObjectFunction original_update_object() noexcept {
            return reinterpret_cast<UpdateObjectFunction>(
                const_cast<std::byte *>(
                    reinterpret_cast<const std::byte *>(original_update_object_ptr)
                )
            );
        }

        static UpdateAllObjectsFunction original_update_all_objects() noexcept {
            return reinterpret_cast<UpdateAllObjectsFunction>(
                const_cast<std::byte *>(
                    reinterpret_cast<const std::byte *>(original_update_all_objects_ptr)
                )
            );
        }

        static UpdatePhysicsFunction update_physics() noexcept {
            return reinterpret_cast<UpdatePhysicsFunction>(server_address(UPDATE_PHYSICS_ADDR));
        }

        static void clear_history_slot(std::size_t player_index) noexcept {
            for(std::size_t h = 0; h < HISTORY_SIZE; h++) {
                history[player_index][h].valid = false;
                history[player_index][h].object_id = HaloID::null_id();
            }
        }

        static void capture_history() noexcept {
            auto &player_table = PlayerTable::get_player_table();
            auto &object_table = ObjectTable::get_object_table();
            auto write_index = history_head;

            for(std::size_t i = 0; i < MAX_PLAYERS; i++) {
                auto &sample = history[i][write_index];
                sample.valid = false;
                sample.object_id = HaloID::null_id();

                if(i >= player_table.current_size) {
                    clear_history_slot(i);
                    continue;
                }

                auto &player = player_table.first_element[i];
                if(player.player_id == 0xFFFF || player.object_id.is_null()) {
                    clear_history_slot(i);
                    continue;
                }

                auto *object = object_table.get_dynamic_object(player.object_id);
                if(!object ||
                   object->type != ObjectType::OBJECT_TYPE_BIPED ||
                   !object->parent.is_null()) {
                    clear_history_slot(i);
                    continue;
                }

                sample.valid = true;
                sample.object_id = player.object_id;
                sample.position = object->position;
            }

            history_head = (history_head + 1) % HISTORY_SIZE;
            history_started = true;
        }

        static std::size_t ping_to_history_ticks(std::uint32_t ping_ms) noexcept {
            double ticks = static_cast<double>(ping_ms) * 30.0 / 1000.0;
            if(ticks <= 0.0) {
                return 0;
            }

            auto rounded = static_cast<std::size_t>(std::ceil(ticks));
            if(rounded >= HISTORY_SIZE - 1) {
                rounded = HISTORY_SIZE - 2;
            }
            return rounded;
        }

        static bool find_shooter_slot(ObjectID object_id, std::size_t &slot, std::uint32_t &ping_ms) noexcept {
            auto &player_table = PlayerTable::get_player_table();
            auto &object_table = ObjectTable::get_object_table();

            auto count = player_table.current_size;
            if(count > MAX_PLAYERS) {
                count = MAX_PLAYERS;
            }

            for(std::size_t i = 0; i < count; i++) {
                auto &player = player_table.first_element[i];
                if(player.player_id == 0xFFFF || player.object_id != object_id) {
                    continue;
                }

                auto *object = object_table.get_dynamic_object(object_id);
                if(!object ||
                   object->type != ObjectType::OBJECT_TYPE_BIPED ||
                   !object->parent.is_null()) {
                    return false;
                }

                slot = i;
                ping_ms = player.ping;
                return true;
            }

            return false;
        }

        static bool get_historical_position(
            std::size_t player_slot,
            ObjectID object_id,
            std::size_t delay_ticks,
            Point3D &position
        ) noexcept {
            if(!history_started || player_slot >= MAX_PLAYERS) {
                return false;
            }

            auto index = (history_head + HISTORY_SIZE - 1 - delay_ticks) % HISTORY_SIZE;
            auto &sample = history[player_slot][index];

            if(!sample.valid || sample.object_id != object_id) {
                return false;
            }

            position = sample.position;
            return true;
        }

        static char on_update_object(ObjectID object_id) {
            auto original = original_update_object();
            if(!original) {
                return 0;
            }

            if(!enabled || !history_started) {
                return original(object_id);
            }

            std::size_t shooter_slot = 0;
            std::uint32_t shooter_ping = 0;
            if(!find_shooter_slot(object_id, shooter_slot, shooter_ping)) {
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
                Point3D current_position {};
                bool rewound = false;
            };

            RewoundPlayer rewound[MAX_PLAYERS] = {};

            auto player_count = player_table.current_size;
            if(player_count > MAX_PLAYERS) {
                player_count = MAX_PLAYERS;
            }

            for(std::size_t i = 0; i < player_count; i++) {
                if(i == shooter_slot) {
                    continue;
                }

                auto &player = player_table.first_element[i];
                if(player.player_id == 0xFFFF || player.object_id.is_null()) {
                    continue;
                }

                auto *target = object_table.get_dynamic_object(player.object_id);
                if(!target ||
                   target->type != ObjectType::OBJECT_TYPE_BIPED ||
                   !target->parent.is_null()) {
                    continue;
                }

                Point3D historical_position {};
                if(!get_historical_position(i, player.object_id, delay_ticks, historical_position)) {
                    continue;
                }

                rewound[i].object = target;
                rewound[i].object_id = player.object_id;
                rewound[i].current_position = target->position;
                rewound[i].rewound = true;

                target->position = historical_position;

                if(update_physics_fn) {
                    update_physics_fn(player.object_id);
                }
            }

            char result = original(object_id);

            for(std::size_t i = 0; i < player_count; i++) {
                if(!rewound[i].rewound || !rewound[i].object) {
                    continue;
                }

                rewound[i].object->position = rewound[i].current_position;

                if(update_physics_fn) {
                    update_physics_fn(rewound[i].object_id);
                }
            }

            return result;
        }

        static char on_update_all_objects() {
            capture_history();

            auto original = original_update_all_objects();
            if(!original) {
                return 0;
            }

            return original();
        }
    }

    void set_up_no_lead_fix() noexcept {
        if(hooked || server_type() != ServerType::SERVER_DEDICATED) {
            return;
        }

        auto *update_object_address = server_address(UPDATE_OBJECT_ADDR);
        auto *update_all_objects_address = server_address(UPDATE_ALL_OBJECTS_ADDR);
        if(!update_object_address || !update_all_objects_address) {
            enabled = false;
            return;
        }

        write_function_override(
            update_object_address,
            update_object_hook,
            reinterpret_cast<const void *>(on_update_object),
            &original_update_object_ptr
        );

        write_function_override(
            update_all_objects_address,
            update_all_objects_hook,
            reinterpret_cast<const void *>(on_update_all_objects),
            &original_update_all_objects_ptr
        );

        hooked = true;
        enabled = true;
    }

    bool no_lead_enabled() noexcept {
        return enabled;
    }

    void set_no_lead_enabled(bool new_enabled) noexcept {
        enabled = new_enabled;
    }
}
