// SPDX-License-Identifier: GPL-3.0-only

#include <cmath>

#include "hitreg_forgiveness.hpp"
#include "../chimera.hpp"
#include "../event/map_load.hpp"
#include "../event/tick.hpp"
#include "../halo_data/camera.hpp"
#include "../halo_data/controls.hpp"
#include "../halo_data/multiplayer.hpp"
#include "../halo_data/object.hpp"
#include "../halo_data/player.hpp"
#include "../halo_data/tag.hpp"
#include "../math_trig/math_trig.hpp"

extern "C" {
    // Module on/off flag: when 1, the forgiveness nudge is active.
    // It does not touch the engine's aim assist (mouse aim assist was
    // removed because it fought free mouse movement).
    std::uint8_t hitreg_forgiveness_on = 1;
}

namespace Chimera {
    // Forgiveness cone in degrees. The aim nudge only pulls shots whose aim
    // is already within this angle of an enemy's center.
    static float forgiveness_cone_degrees = 3.0f;
    static bool forgiveness_events_on = false;

    // Diagnostic counters (read-only; they never change gameplay).
    static std::uint64_t forgiveness_nudges = 0;
    static std::uint64_t forgiveness_candidates = 0;
    static std::uint64_t forgiveness_no_candidate = 0;
    static float forgiveness_last_nudge_angle = -1.0f;

    // How many times the pre-tick look injection fired.
    static std::uint64_t forgiveness_injections = 0;

    // Aim-persistence diagnostic: did the previous tick's correction survive
    // the engine tick? Measured at the start of the post-tick event, before
    // re-nudging, by comparing the current aim with the direction the last
    // nudge wrote and with the aim from before that nudge. This is what the
    // shot in the tick that just ran actually used.
    // - aim_kept: the aim was still (within 0.5 deg) on the nudged direction.
    // - aim_erased: the aim was back (within 0.5 deg) to the pre-nudge aim.
    // - aim_drifted: the aim was neither (mouse moved, or partial erasure).
    static bool forgiveness_have_pending_aim_check = false;
    static Point3D forgiveness_last_nudged_dir = {};
    static Point3D forgiveness_pre_nudge_aim = {};
    static std::uint64_t forgiveness_aim_kept = 0;
    static std::uint64_t forgiveness_aim_erased = 0;
    static std::uint64_t forgiveness_aim_drifted = 0;
    static float forgiveness_last_aim_drift_deg = -1.0f;

    // Weapon-tag route: the part of the forgiveness that actually
    // reaches the shot. The aim/camera/look nudges above are all
    // erased by the engine, which re-derives the aim every tick from
    // the mouse look input (diagnostic: aim erased 41 vs aim kept 2,
    // and shots only landed with the reticle already red). The
    // engine's fire-time autoaim is different: when a weapon fires,
    // the engine itself bends the bullet toward a target inside the
    // weapon tag's autoaim angle, up to the tag's deviation angle.
    // That bend happens at fire time, after the aim is final, so
    // nothing erases it -- and it never touches the mouse or the
    // view, so free mouse movement stays untouched. Widening the
    // pistol tag's two angles to the forgiveness cone therefore
    // makes the engine's own bend engage for near-miss shots.
    // This uses the same offsets map_loading.cpp already uses for
    // the warthog gun (autoaim angle +0x3E4, deviation angle +0x3F4)
    // and the same get_tag lookup pattern.
    // Honest limit: this only works if the engine's fire-time
    // autoaim runs for mouse players. If it turns out to be gated
    // on analog movement like the per-tick magnetism, widening the
    // angles will do nothing -- the in-game test (a 2.3 deg off
    // shot at a static target making the shield flare) decides it.
    // Finding and hooking the bend itself would need the game binary
    // (haloce.exe), which is not available here: saosin has no
    // signature or address for it.
    static constexpr const char *FORGIVENESS_PISTOL_TAG_PATH = "weapons\\pistol\\pistol";
    static constexpr std::size_t WEAPON_TAG_AUTOAIM_ANGLE_OFFSET = 0x3E4;
    static constexpr std::size_t WEAPON_TAG_DEVIATION_ANGLE_OFFSET = 0x3F4;
    // Tag data currently patched (nullptr = nothing patched). The
    // stock angles are saved on the first patch so they can be put
    // back when the module is turned off or the cone is set to 0.
    static std::byte *forgiveness_pistol_tag_data = nullptr;
    static float forgiveness_pistol_stock_autoaim = 0.0f;
    static float forgiveness_pistol_stock_deviation = 0.0f;
    static bool forgiveness_map_load_event_on = false;

    // Widen (or restore) the pistol's autoaim angles. Called on map
    // load, when the module is toggled and when the cone changes, so
    // the tag always matches the current setting.
    static void apply_forgiveness_weapon_angles() noexcept {
        // Module off or cone 0: put the stock angles back.
        if(!::hitreg_forgiveness_on || forgiveness_cone_degrees <= 0.0f) {
            if(forgiveness_pistol_tag_data) {
                *reinterpret_cast<float *>(forgiveness_pistol_tag_data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET) = forgiveness_pistol_stock_autoaim;
                *reinterpret_cast<float *>(forgiveness_pistol_tag_data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET) = forgiveness_pistol_stock_deviation;
                forgiveness_pistol_tag_data = nullptr;
            }
            return;
        }

        auto *tag = get_tag(FORGIVENESS_PISTOL_TAG_PATH, TagClassInt::TAG_CLASS_WEAPON);
        if(!tag || !tag->data) {
            return;
        }

        // New tag data (a freshly loaded map): remember the stock
        // angles before overwriting them.
        if(tag->data != forgiveness_pistol_tag_data) {
            forgiveness_pistol_stock_autoaim = *reinterpret_cast<float *>(tag->data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET);
            forgiveness_pistol_stock_deviation = *reinterpret_cast<float *>(tag->data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET);
            forgiveness_pistol_tag_data = tag->data;
        }

        float angle = static_cast<float>(DEGREES_TO_RADIANS(forgiveness_cone_degrees));
        *reinterpret_cast<float *>(tag->data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET) = angle;
        *reinterpret_cast<float *>(tag->data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET) = angle;
    }

    static void hitreg_forgiveness_map_load() noexcept {
        apply_forgiveness_weapon_angles();
    }

    // Angle (degrees) between two normalized directions.
    static float angle_between(const Point3D &a, const Point3D &b) noexcept {
        float dot = a.x*b.x + a.y*b.y + a.z*b.z;
        if(dot > 1.0f) dot = 1.0f;
        if(dot < -1.0f) dot = -1.0f;
        return static_cast<float>(RADIANS_TO_DEGREES(std::acos(static_cast<double>(dot))));
    }

    // Get the local player's live biped, or nullptr.
    static UnitDynamicObject *get_client_unit() noexcept {
        auto *client_player = PlayerTable::get_player_table().get_client_player();
        if(!client_player) {
            return nullptr;
        }
        auto *unit = reinterpret_cast<UnitDynamicObject *>(ObjectTable::get_object_table().get_dynamic_object(client_player->object_id));
        if(!unit || unit->type != ObjectType::OBJECT_TYPE_BIPED) {
            return nullptr;
        }
        return unit;
    }

    // Shared enemy scan: find the best (smallest-angle) enemy inside the
    // forgiveness cone. eye is the shooter's center, aim the current
    // (normalized) aim, shooter the local player's biped (skipped as a
    // target). On success, fills best_dir (normalized direction to the
    // enemy's center) and best_angle (degrees) and returns true.
    // candidates_out, if non-null, is set to the number of enemies found
    // inside the cone (not just the best one).
    static bool find_forgiveness_target(const Point3D &eye, const Point3D &aim, UnitDynamicObject *shooter, Point3D &best_dir, float &best_angle, std::uint64_t *candidates_out) noexcept {
        auto &object_table = ObjectTable::get_object_table();
        auto &player_table = PlayerTable::get_player_table();
        auto *client_player = player_table.get_client_player();
        if(!client_player) {
            return false;
        }

        std::uint64_t candidates = 0;
        best_angle = forgiveness_cone_degrees;
        bool found = false;

        for(std::size_t i = 0; i < object_table.current_size; i++) {
            auto *target = object_table.get_dynamic_object(i);
            if(!target || target->type != ObjectType::OBJECT_TYPE_BIPED) {
                continue;
            }
            if(target == static_cast<BaseDynamicObject *>(shooter)) {
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

            float angle = angle_between(aim, to_target);
            // Count every enemy inside the cone, not just the best one.
            if(angle < forgiveness_cone_degrees) {
                candidates++;
            }
            if(angle < best_angle) {
                best_angle = angle;
                best_dir = to_target;
                found = true;
            }
        }

        if(candidates_out) {
            *candidates_out = candidates;
        }
        return found;
    }

    // Pre-tick look injection. Writing the derived aim (aim/desired_aim,
    // camera) after the tick does not reach the shot: the engine re-derives
    // the look every tick from the mouse look input (the per-tick
    // aim_left/aim_up deltas), so the correction is erased by the next
    // tick's look update before the weapon fires. The diagnostic counters
    // proved it: the nudge was written hundreds of times, yet shots at a
    // static target 36 m away never made the shield flare, and the reticle
    // never snapped red on its own. The correction therefore has to enter
    // through the look input itself: this injects the yaw/pitch needed to
    // rotate the aim onto the enemy into the per-tick look deltas, so the
    // engine's own look update applies the correction and the shot uses it.
    // It only runs while firing and only inside the cone, so the mouse
    // stays completely free when not shooting.
    static void hitreg_forgiveness_pretick() noexcept {
        if(!::hitreg_forgiveness_on || forgiveness_cone_degrees <= 0.0f) {
            return;
        }

        auto *unit = get_client_unit();
        if(!unit) {
            return;
        }

        // Dead players don't shoot.
        if(unit->health <= 0.0f) {
            return;
        }

        // Only help while the trigger is actually held.
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

        Point3D best_dir = {};
        float best_angle = 0.0f;
        if(!find_forgiveness_target(eye, aim, unit, best_dir, best_angle, nullptr)) {
            return;
        }

        // Yaw (rotation about the world Z axis, positive = left) and pitch
        // (elevation difference, positive = up) needed to rotate the aim
        // onto the target. The angles are small (<= the cone, <= 10 deg),
        // so splitting the single rotation into yaw + pitch is accurate.
        float cross_z = aim.x*best_dir.y - aim.y*best_dir.x;
        float dot_xy = aim.x*best_dir.x + aim.y*best_dir.y;
        float yaw_delta = std::atan2(cross_z, dot_xy);
        float aim_z = aim.z;
        if(aim_z > 1.0f) aim_z = 1.0f;
        if(aim_z < -1.0f) aim_z = -1.0f;
        float dir_z = best_dir.z;
        if(dir_z > 1.0f) dir_z = 1.0f;
        if(dir_z < -1.0f) dir_z = -1.0f;
        float pitch_delta = std::asin(dir_z) - std::asin(aim_z);

        // Safety net: never inject more than the cone in a single tick.
        float max_delta = static_cast<float>(DEGREES_TO_RADIANS(forgiveness_cone_degrees));
        if(yaw_delta > max_delta) yaw_delta = max_delta;
        if(yaw_delta < -max_delta) yaw_delta = -max_delta;
        if(pitch_delta > max_delta) pitch_delta = max_delta;
        if(pitch_delta < -max_delta) pitch_delta = -max_delta;

        auto &controls = get_controls();
        controls.aim_left += yaw_delta;
        controls.aim_up += pitch_delta;
        forgiveness_injections++;
    }

    // The nudge runs as a tick event, i.e. AFTER the engine's tick. This is
    // deliberate: the engine re-derives aim/desired_aim from the player's
    // look input during its tick, so a pre-tick write to the derived aim is
    // discarded before the weapon fires. Writing here makes the corrected
    // aim the starting point for the next tick, so the next shot actually
    // uses it. (The pre-tick look injection above is what makes the
    // correction survive the tick's look update in the first place; this
    // post-tick write refines the derived aim and camera to match.)
    static void hitreg_forgiveness_tick() noexcept {
        if(!::hitreg_forgiveness_on || forgiveness_cone_degrees <= 0.0f) {
            forgiveness_have_pending_aim_check = false;
            return;
        }

        auto *unit = get_client_unit();
        if(!unit) {
            forgiveness_have_pending_aim_check = false;
            return;
        }

        // Dead players don't shoot.
        if(unit->health <= 0.0f) {
            forgiveness_have_pending_aim_check = false;
            return;
        }

        // Aim direction, normalized. The engine keeps this normalized, but be safe.
        Point3D aim = unit->aim;
        float aim_len = std::sqrt(aim.x*aim.x + aim.y*aim.y + aim.z*aim.z);
        if(aim_len < 0.5f || aim_len > 2.0f) {
            forgiveness_have_pending_aim_check = false;
            return;
        }
        aim.x /= aim_len; aim.y /= aim_len; aim.z /= aim_len;

        // Only help while the trigger is actually held: this is a shooting
        // aid, not something that drags the aim around when not firing.
        if(!unit->primary_fire_button) {
            forgiveness_have_pending_aim_check = false;
            return;
        }

        // Aim-persistence diagnostic: did the previous tick's correction
        // survive the engine tick? Compare the current aim with the
        // direction the last nudge wrote and with the aim from before that
        // nudge. Measured here, before re-nudging, so it reflects what the
        // shot in the tick that just ran actually used.
        if(forgiveness_have_pending_aim_check) {
            forgiveness_have_pending_aim_check = false;
            float drift = angle_between(aim, forgiveness_last_nudged_dir);
            float back = angle_between(aim, forgiveness_pre_nudge_aim);
            forgiveness_last_aim_drift_deg = drift;
            if(drift < 0.5f) {
                forgiveness_aim_kept++;
            }
            else if(back < 0.5f) {
                forgiveness_aim_erased++;
            }
            else {
                forgiveness_aim_drifted++;
            }
        }

        // Eye position: the shooter's center is close to the head.
        Point3D eye = unit->center_position;

        Point3D best_dir = {};
        float best_angle = 0.0f;
        std::uint64_t candidates = 0;
        if(!find_forgiveness_target(eye, aim, unit, best_dir, best_angle, &candidates)) {
            // Firing, but no enemy inside the cone to pull onto.
            forgiveness_no_candidate++;
            return;
        }
        forgiveness_candidates += candidates;

        // Pull the aim onto the enemy's center. The crosshair (camera) is
        // corrected together with the aim so the two agree: the camera's
        // forward vector is derived from the aim each tick, so writing only
        // the camera would move the crosshair without moving the bullet (and
        // the interpolate module snapshots/restores the camera every frame
        // anyway). The camera is only touched in first person -- in a
        // vehicle or third person the camera is not the aim -- and only
        // while firing, so the mouse stays completely free when not shooting.
        forgiveness_pre_nudge_aim = aim;
        forgiveness_last_nudged_dir = best_dir;
        forgiveness_have_pending_aim_check = true;
        unit->aim = best_dir;
        unit->desired_aim = best_dir;
        if(camera_type() == CameraType::CAMERA_FIRST_PERSON) {
            auto &camera = camera_data();
            camera.orientation[0] = best_dir;
            // Keep the view basis valid: re-orthogonalize the up vector
            // against the new forward vector (Gram-Schmidt) and normalize.
            // If the two are (nearly) parallel, leave the up vector alone.
            float dot = camera.orientation[1].x * best_dir.x + camera.orientation[1].y * best_dir.y + camera.orientation[1].z * best_dir.z;
            Point3D up = {
                camera.orientation[1].x - best_dir.x * dot,
                camera.orientation[1].y - best_dir.y * dot,
                camera.orientation[1].z - best_dir.z * dot
            };
            float up_len = std::sqrt(up.x*up.x + up.y*up.y + up.z*up.z);
            if(up_len > 0.1f) {
                camera.orientation[1].x = up.x / up_len;
                camera.orientation[1].y = up.y / up_len;
                camera.orientation[1].z = up.z / up_len;
            }
        }
        forgiveness_nudges++;
        forgiveness_last_nudge_angle = best_angle;
    }

    void set_up_hitreg_forgiveness() noexcept {
        // The aim-assist hook itself is installed by set_up_aim_assist_fix();
        // this module only arms the extra flag and the forgiveness events.
        set_hitreg_forgiveness_enabled(true);
    }

    void set_hitreg_forgiveness_enabled(bool enabled) noexcept {
        ::hitreg_forgiveness_on = enabled ? 1 : 0;
        if(enabled && !forgiveness_events_on) {
            add_pretick_event(hitreg_forgiveness_pretick);
            add_tick_event(hitreg_forgiveness_tick);
            forgiveness_events_on = true;
        }
        else if(!enabled && forgiveness_events_on) {
            remove_pretick_event(hitreg_forgiveness_pretick);
            remove_tick_event(hitreg_forgiveness_tick);
            forgiveness_events_on = false;
        }
        if(enabled && !forgiveness_map_load_event_on) {
            add_map_load_event(hitreg_forgiveness_map_load);
            forgiveness_map_load_event_on = true;
        }
        else if(!enabled && forgiveness_map_load_event_on) {
            remove_map_load_event(hitreg_forgiveness_map_load);
            forgiveness_map_load_event_on = false;
        }
        // (Re)apply or restore the pistol's autoaim angles right away,
        // so a map that is already loaded does not need a reload.
        apply_forgiveness_weapon_angles();
    }

    bool hitreg_forgiveness_enabled() noexcept {
        return ::hitreg_forgiveness_on != 0;
    }

    void set_hitreg_forgiveness_cone(float degrees) noexcept {
        if(degrees < 0.0f) degrees = 0.0f;
        if(degrees > 10.0f) degrees = 10.0f;
        forgiveness_cone_degrees = degrees;
        // The pistol's autoaim angles follow the cone, so changing
        // the cone takes effect without reloading the map. A cone of
        // 0 restores the stock angles.
        apply_forgiveness_weapon_angles();
    }

    float hitreg_forgiveness_cone() noexcept {
        return forgiveness_cone_degrees;
    }

    std::uint64_t hitreg_forgiveness_nudges() noexcept {
        return forgiveness_nudges;
    }

    std::uint64_t hitreg_forgiveness_candidates() noexcept {
        return forgiveness_candidates;
    }

    std::uint64_t hitreg_forgiveness_no_candidate() noexcept {
        return forgiveness_no_candidate;
    }

    float hitreg_forgiveness_last_nudge_angle() noexcept {
        return forgiveness_last_nudge_angle;
    }

    std::uint64_t hitreg_forgiveness_injections() noexcept {
        return forgiveness_injections;
    }

    std::uint64_t hitreg_forgiveness_aim_kept() noexcept {
        return forgiveness_aim_kept;
    }

    std::uint64_t hitreg_forgiveness_aim_erased() noexcept {
        return forgiveness_aim_erased;
    }

    std::uint64_t hitreg_forgiveness_aim_drifted() noexcept {
        return forgiveness_aim_drifted;
    }

    float hitreg_forgiveness_last_aim_drift() noexcept {
        return forgiveness_last_aim_drift_deg;
    }

    float hitreg_forgiveness_weapon_autoaim_deg() noexcept {
        if(!forgiveness_pistol_tag_data) {
            return -1.0f;
        }
        float angle = *reinterpret_cast<float *>(forgiveness_pistol_tag_data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET);
        return static_cast<float>(RADIANS_TO_DEGREES(angle));
    }

    void hitreg_forgiveness_reset_stats() noexcept {
        forgiveness_nudges = 0;
        forgiveness_candidates = 0;
        forgiveness_no_candidate = 0;
        forgiveness_last_nudge_angle = -1.0f;
        forgiveness_injections = 0;
        forgiveness_have_pending_aim_check = false;
        forgiveness_aim_kept = 0;
        forgiveness_aim_erased = 0;
        forgiveness_aim_drifted = 0;
        forgiveness_last_aim_drift_deg = -1.0f;
    }
}
