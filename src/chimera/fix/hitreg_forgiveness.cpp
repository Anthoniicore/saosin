// SPDX-License-Identifier: GPL-3.0-only

#include <cmath>

#include "hitreg_forgiveness.hpp"
#include "../chimera.hpp"
#include "../event/tick.hpp"
#include "../halo_data/multiplayer.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/player.hpp"
#include "../math_trig/math_trig.hpp"

extern "C" {
    // Read by fix/aim_assist.S: when 1, the engine's aim assist also runs for
    // mouse players instead of being skipped.
    std::uint8_t hitreg_forgiveness_on = 1;
}

namespace Chimera {
    // Forgiveness cone in degrees. The aim nudge only pulls shots whose aim
    // is already within this angle of an enemy's center.
    static float forgiveness_cone_degrees = 3.0f;
    static bool forgiveness_events_on = false;

    static void hitreg_forgiveness_pretick() noexcept {
        // Only while the module is on and the cone allows a nudge.
        if(!::hitreg_forgiveness_on || forgiveness_cone_degrees <= 0.0f) {
            return;
        }

        auto *client_player = PlayerTable::get_player_table().get_client_player();
        if(!client_player) {
            return;
        }

        auto *unit = reinterpret_cast<UnitDynamicObject *>(ObjectTable::get_object_table().get_dynamic_object(client_player->object_id));
        if(!unit || unit->type != ObjectType::OBJECT_TYPE_BIPED) {
            return;
        }

        // Dead players don't shoot.
        if(unit->health <= 0.0f) {
            return;
        }

        // Only help while the trigger is actually held: this is a shooting
        // aid, not something that drags the aim around when not firing.
        if(!unit->primary_fire_button) {
            return;
        }

        // Aim direction, normalized. The engine keeps this normalized, but be safe.
        Point3D aim = unit->aim;
        float aim_len = std::sqrt(aim.x*aim.x + aim.y*aim.y + aim.z*aim.z);
        if(aim_len < 0.5f || aim_len > 2.0f) {
            return;
        }
        aim.x /= aim_len; aim.y /= aim_len; aim.z /= aim_len;

        // Eye position: the shooter's center is close to the head.
        Point3D eye = unit->center_position;

        auto &object_table = ObjectTable::get_object_table();
        auto &player_table = PlayerTable::get_player_table();

        // Best (smallest-angle) enemy inside the forgiveness cone.
        float best_angle = forgiveness_cone_degrees;
        Point3D best_dir = {};
        bool found = false;

        for(std::size_t i = 0; i < object_table.current_size; i++) {
            auto *target = object_table.get_dynamic_object(i);
            if(!target || target->type != ObjectType::OBJECT_TYPE_BIPED) {
                continue;
            }
            if(target == static_cast<BaseDynamicObject *>(unit)) {
                continue;
            }
            auto *target_unit = reinterpret_cast<UnitDynamicObject *>(target);
            if(target_unit->health <= 0.0f) {
                continue;
            }
            // Don't snap to heavily camouflaged players.
            if(target_unit->invisibility > 0.5f) {
                continue;
            }
            // Must belong to another player.
            auto *target_player = player_table.get_player(target_unit->player_id);
            if(!target_player) {
                continue;
            }
            // Teammates are not targets in team games.
            if(is_team() && target_player->team == client_player->team) {
                continue;
            }

            Point3D to_target = {
                target->center_position.x - eye.x,
                target->center_position.y - eye.y,
                target->center_position.z - eye.z
            };
            float dist = std::sqrt(to_target.x*to_target.x + to_target.y*to_target.y + to_target.z*to_target.z);
            if(dist < 0.5f) {
                continue;
            }
            to_target.x /= dist; to_target.y /= dist; to_target.z /= dist;

            float dot = aim.x*to_target.x + aim.y*to_target.y + aim.z*to_target.z;
            if(dot > 1.0f) dot = 1.0f;
            if(dot < -1.0f) dot = -1.0f;
            float angle = static_cast<float>(RADIANS_TO_DEGREES(std::acos(static_cast<double>(dot))));
            if(angle < best_angle) {
                best_angle = angle;
                best_dir = to_target;
                found = true;
            }
        }

        if(!found) {
            return;
        }

        // Pull the aim onto the enemy's center. The crosshair (camera) does
        // not move; only the direction the weapon fires along is corrected.
        unit->aim = best_dir;
        unit->desired_aim = best_dir;
    }

    void set_up_hitreg_forgiveness() noexcept {
        // The aim-assist hook itself is installed by set_up_aim_assist_fix();
        // this module only arms the extra flag and the pretick nudge.
        set_hitreg_forgiveness_enabled(true);
    }

    void set_hitreg_forgiveness_enabled(bool enabled) noexcept {
        ::hitreg_forgiveness_on = enabled ? 1 : 0;
        if(enabled && !forgiveness_events_on) {
            add_pretick_event(hitreg_forgiveness_pretick);
            forgiveness_events_on = true;
        }
        else if(!enabled && forgiveness_events_on) {
            remove_pretick_event(hitreg_forgiveness_pretick);
            forgiveness_events_on = false;
        }
    }

    bool hitreg_forgiveness_enabled() noexcept {
        return ::hitreg_forgiveness_on != 0;
    }

    void set_hitreg_forgiveness_cone(float degrees) noexcept {
        if(degrees < 0.0f) degrees = 0.0f;
        if(degrees > 10.0f) degrees = 10.0f;
        forgiveness_cone_degrees = degrees;
    }

    float hitreg_forgiveness_cone() noexcept {
        return forgiveness_cone_degrees;
    }
}
