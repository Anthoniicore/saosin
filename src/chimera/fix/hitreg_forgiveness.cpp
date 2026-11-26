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
#include "../signature/hook.hpp"

extern "C" {
    // Module on/off flag: when 1, the forgiveness nudge is active.
    // It does not touch the engine's aim assist (mouse aim assist was
    // removed because it fought free mouse movement).
    std::uint8_t hitreg_forgiveness_on = 1;

    // Bend nudge (radians) to add to the look increments in the
    // set_digital_yaw/pitch_increment setters. Written by the C++
    // call-before, read by the .S call-after stubs
    // (hitreg_forgiveness_bend.S). See compute_forgiveness_bend().
    float forgiveness_bend_yaw_rad = 0.0f;
    float forgiveness_bend_pitch_rad = 0.0f;

    // Call-after stubs (hitreg_forgiveness_bend.S). Each adds the
    // matching bend nudge above to ST(0) (the converter's return
    // value, in radians), so the setter's fstps stores the mouse
    // delta plus the bend onto the enemy.
    void on_forgiveness_bend_yaw_after();
    void on_forgiveness_bend_pitch_after();
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

    // How many times the setter bend was armed (the yaw setter's
    // call-before computed a non-zero nudge). One per input update
    // while firing at an enemy inside the cone (the pitch setter
    // recomputes the same nudge but does not count, so this is not
    // doubled).
    static std::uint64_t forgiveness_bends = 0;

    // Setter hooks for the bend (see install_forgiveness_bend_hooks).
    // Stored statically: the Hook owns the trampoline code, so it
    // must outlive the setup call.
    static Hook forgiveness_bend_yaw_hook;
    static Hook forgiveness_bend_pitch_hook;
    static bool forgiveness_bend_hooks_installed = false;

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

    // Outgoing-aim diagnostic (read-only): the nudge the forgiveness
    // applies, vs the look increments the client sends to the server.
    // The client does NOT send absolute yaw/pitch: it sends player
    // actions, and the aim in an action is the per-tick look
    // increment (yaw and pitch deltas, in radians). They live in
    // the per-player action struct at 0x6ABB38 (stride 0x85C,
    // index 0 = local player): +0x08 is the yaw increment,
    // +0x0C the pitch increment. Evidence from haloce.exe: the
    // game's own get_digital_yaw_increment (VA 0x484950) and
    // get_digital_pitch_increment (VA 0x484A20) read exactly these
    // two floats (distinct addresses, confirmed) and convert
    // radians to milli-turns (x1000/2pi, constant 0x6122d8 =
    // 159.15494), the setters set_digital_yaw_increment
    // (0x4849B0) / set_digital_pitch_increment (0x484A80) write
    // them back, and the action-apply function (0x48F850+, the one
    // containing controls_sig) adds them into Controls.aim_left /
    // aim_up (0x6AD4C0/0x6AD4C4). The two reads are therefore
    // correct; if out yaw and out pitch ever print the same value
    // it is the game data, not the read: the apply function only
    // READS the increments and never zeroes them, so they persist
    // from the last input (a diagonal last movement leaves equal
    // yaw and pitch). These addresses are inside the game's own
    // image (0x400000-0x82C000), so they are always mapped and
    // safe to read.
    //
    // Why the nudge is captured in the tick (post-tick) event and
    // NOT re-read from Controls in the pre-tick injection (the old
    // code did that and always printed 0.00): the pre-tick
    // injection runs before the engine tick, when unit->aim was
    // already aligned onto the target by the previous post-tick
    // nudge, so the delta it computes is ~0; and Controls at that
    // moment still holds the previous tick's already-consumed mouse
    // delta (the apply function has not rebuilt it yet). Worse,
    // the apply function ZEROES the whole Controls struct at the
    // start of every tick (xor edx,edx at 0x48F8DC, then mov edx
    // into 0x6AD498-0x6AD4C8) and rebuilds it from the action
    // queue, so any pre-tick write to Controls.aim_left/aim_up is
    // wiped during the same tick, before the look update uses it.
    // That is the structural reason the injection never reaches
    // the action the server receives: the correction has to go
    // into the action increments themselves (hook the setters),
    // not into Controls. So this diagnostic captures, in the
    // post-tick event (after a target is found, while firing),
    // the nudge components (the yaw/pitch needed to rotate the
    // pre-nudge aim onto the target -- the real correction, the
    // same best_angle the `last nudge` line reports) and the
    // outgoing increments at the same moment. If the nudge is
    // non-zero while the out increments do not include it, the
    // correction is not in the action the server receives. Once
    // the setter hook bends the increments, `out` must show the
    // nudge included -- that is how the bend will be verified.
    static constexpr std::uintptr_t FORGIVENESS_OUTGOING_YAW_INCREMENT_ADDRESS = 0x6ABB40;
    static constexpr std::uintptr_t FORGIVENESS_OUTGOING_PITCH_INCREMENT_ADDRESS = 0x6ABB44;
    static bool forgiveness_have_last_outgoing_aim = false;
    static float forgiveness_last_out_yaw_deg = 0.0f;
    static float forgiveness_last_out_pitch_deg = 0.0f;
    static float forgiveness_last_nudge_yaw_deg = 0.0f;
    static float forgiveness_last_nudge_pitch_deg = 0.0f;

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
    // Has ANY map actually finished loading since DLL load? This is
    // the only gate that may be checked during DLL init: it is plain
    // module memory, not game memory. It is set only by the map-load
    // event below, which fires once a map is loaded and its tags are
    // valid (the same point where map_loading.cpp patches the
    // warthog's angles).
    static bool forgiveness_map_loaded = false;

    // Fallback when the pistol cannot be found by path: the tag
    // paths in the map may be scrambled (protected/custom map) or
    // the pistol tag may simply not be found by name, so the path
    // lookup fails even with the map fully loaded. The tag CLASS is
    // never scrambled (the game needs it), so the fallback patches
    // every weapon-class tag in the array instead -- the pistol is
    // a weapon tag, so it is covered whatever its path is. Stock
    // angles are saved per tag so they can be put back. This only
    // runs when the path lookup failed; on a normal map the pistol
    // is found by path and this never runs.
    struct ForgivenessPatchedWeaponTag {
        std::byte *data;
        float stock_autoaim;
        float stock_deviation;
    };
    static constexpr std::size_t FORGIVENESS_MAX_PATCHED_WEAPON_TAGS = 32;
    static ForgivenessPatchedWeaponTag forgiveness_patched_weapon_tags[FORGIVENESS_MAX_PATCHED_WEAPON_TAGS];
    static std::size_t forgiveness_patched_weapon_tag_count = 0;
    static bool forgiveness_weapon_tags_patched = false;

    // Is a map's tag data actually loaded? The tag header lives at a
    // fixed address (0x40440000) and carries the "tags" magic at
    // +0x24 when a map is loaded. NOTE: this check alone is NOT
    // enough during DLL init, and that is exactly why the 575f51d
    // build still did not load. Chimera's constructor runs at DLL
    // load, before any map loads, and set_up_hitreg_forgiveness()
    // calls apply_forgiveness_weapon_angles() from there -- and this
    // function itself reads the header at 0x40440000 to check it.
    // Before the first map load that address is not mapped yet, so
    // the check faults right there in the guard, during DLL init,
    // and Windows reports "strings.dll is missing" even though the
    // file is right there. The 562f3b8 build crashed the same way by
    // reading the tag array directly; 575f51d only moved the fault
    // into the guard. The previous build (2edd35b) never touched tags
    // at init and loaded fine. So: never read ANY tag memory --
    // not even this header check -- until forgiveness_map_loaded
    // says a map has actually loaded. This function is only a second
    // layer, called after that gate passes.
    //
    // The magic is a fourcc, NOT the ASCII string "tags": Halo
    // fourccs put the first character in the most significant byte,
    // so 'tags' as a uint32 is 0x74616773 (1952540531, per the map
    // format doc), which is stored little-endian as the bytes
    // "sgat". The 575f51d guard compared the raw bytes against the
    // string "tags" with memcmp, which can never match a valid map
    // -- that is why `show` reported tags 0 / weapon tags 0 and
    // "tag not found" even with a map fully loaded (Blood Gulch,
    // pistol in hand). Compare the uint32 value instead. The
    // raw-ASCII value 0x73676174 is also accepted, in case some
    // tool writes the magic as a plain string.
    static bool forgiveness_tags_loaded() noexcept {
        const auto &header = get_tag_data_header();
        return (header.tags_literal == 0x74616773 || header.tags_literal == 0x73676174) &&
               header.tag_array != nullptr &&
               header.tag_count > 0 && header.tag_count < 100000;
    }

    // A tag data pointer is usable only if it is a real pointer. Some
    // tags in a map are "Missingno" entries whose data is 0xFFFFFFFF
    // (see map_is_protected); writing angles there would fault.
    static bool forgiveness_tag_data_usable(const std::byte *data) noexcept {
        return data != nullptr && reinterpret_cast<std::uintptr_t>(data) != 0xFFFFFFFF;
    }

    // Put back the stock angles of every weapon tag this module
    // patched (the pistol by path, and/or the weapon tags found by
    // the class scan) and forget them. Only call this while tag
    // memory is valid (apply_forgiveness_weapon_angles gates on
    // that). Each stored pointer is checked against the current
    // map's tag array first, so a pointer from a previous map is
    // never written to.
    static void restore_forgiveness_weapon_angles() noexcept {
        if(forgiveness_pistol_tag_data) {
            // Only write back if this is still the pistol tag: after
            // a map change the stored pointer may be stale.
            auto *tag = get_tag(FORGIVENESS_PISTOL_TAG_PATH, TagClassInt::TAG_CLASS_WEAPON);
            if(tag && tag->data == forgiveness_pistol_tag_data) {
                *reinterpret_cast<float *>(forgiveness_pistol_tag_data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET) = forgiveness_pistol_stock_autoaim;
                *reinterpret_cast<float *>(forgiveness_pistol_tag_data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET) = forgiveness_pistol_stock_deviation;
            }
            forgiveness_pistol_tag_data = nullptr;
        }
        if(forgiveness_patched_weapon_tag_count > 0) {
            const auto &header = get_tag_data_header();
            auto *tags = header.tag_array;
            auto tag_count = header.tag_count;
            for(std::size_t i = 0; i < tag_count; i++) {
                if(tags[i].primary_class != TagClassInt::TAG_CLASS_WEAPON) {
                    continue;
                }
                auto *data = tags[i].data;
                if(!forgiveness_tag_data_usable(data)) {
                    continue;
                }
                for(std::size_t j = 0; j < forgiveness_patched_weapon_tag_count; j++) {
                    if(forgiveness_patched_weapon_tags[j].data == data) {
                        *reinterpret_cast<float *>(data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET) = forgiveness_patched_weapon_tags[j].stock_autoaim;
                        *reinterpret_cast<float *>(data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET) = forgiveness_patched_weapon_tags[j].stock_deviation;
                        break;
                    }
                }
            }
            forgiveness_patched_weapon_tag_count = 0;
            forgiveness_weapon_tags_patched = false;
        }
    }

    // Widen (or restore) the pistol's autoaim angles. Called on map
    // load, when the module is toggled and when the cone changes, so
    // the tag always matches the current setting.
    static void apply_forgiveness_weapon_angles() noexcept {
        // No map has finished loading yet (DLL init, ...): do nothing
        // AT ALL. In particular, do not even call
        // forgiveness_tags_loaded() here: checking the header reads
        // 0x40440000, which is not mapped before the first map load,
        // and faulting there during DLL init is what kept the DLL
        // from loading in both 562f3b8 and 575f51d. The map-load
        // event below sets the flag and applies the angles once a map
        // is actually loaded, so nothing is lost by skipping the
        // init-time call.
        if(!forgiveness_map_loaded) {
            return;
        }
        if(!forgiveness_tags_loaded()) {
            return;
        }

        // Module off or cone 0: put the stock angles back.
        if(!::hitreg_forgiveness_on || forgiveness_cone_degrees <= 0.0f) {
            restore_forgiveness_weapon_angles();
            return;
        }

        float angle = static_cast<float>(DEGREES_TO_RADIANS(forgiveness_cone_degrees));

        // Primary: the pistol, by path. On a normal map this finds
        // it and nothing else below runs.
        auto *tag = get_tag(FORGIVENESS_PISTOL_TAG_PATH, TagClassInt::TAG_CLASS_WEAPON);
        if(tag && forgiveness_tag_data_usable(tag->data)) {
            // New tag data (a freshly loaded map): remember the stock
            // angles before overwriting them.
            if(tag->data != forgiveness_pistol_tag_data) {
                forgiveness_pistol_stock_autoaim = *reinterpret_cast<float *>(tag->data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET);
                forgiveness_pistol_stock_deviation = *reinterpret_cast<float *>(tag->data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET);
                forgiveness_pistol_tag_data = tag->data;
            }
            *reinterpret_cast<float *>(tag->data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET) = angle;
            *reinterpret_cast<float *>(tag->data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET) = angle;
            return;
        }

        // Fallback: the pistol was not found by path even with the
        // map fully loaded. The tag paths in this map may be
        // scrambled (protected/custom map) or the pistol tag may
        // simply not be found by name -- either way, no path-based
        // lookup can work here. The tag CLASS is never scrambled
        // (the game needs it), so patch every weapon-class tag in
        // the array instead: the pistol is a weapon tag, so it is
        // covered whatever its path is. Stock angles are saved per
        // tag the first time so they can be put back. Already-patched
        // tags just get the (possibly new) angle re-written, so a
        // cone change takes effect without reloading the map. If no
        // weapon tag is found at all, nothing is patched and the
        // retry keeps trying until the tag is patched.
        const auto &header = get_tag_data_header();
        auto *tags = header.tag_array;
        auto tag_count = header.tag_count;
        for(std::size_t i = 0; i < tag_count; i++) {
            if(tags[i].primary_class != TagClassInt::TAG_CLASS_WEAPON) {
                continue;
            }
            auto *data = tags[i].data;
            if(!forgiveness_tag_data_usable(data)) {
                continue;
            }
            bool known = false;
            for(std::size_t j = 0; j < forgiveness_patched_weapon_tag_count; j++) {
                if(forgiveness_patched_weapon_tags[j].data == data) {
                    known = true;
                    break;
                }
            }
            if(!known) {
                if(forgiveness_patched_weapon_tag_count >= FORGIVENESS_MAX_PATCHED_WEAPON_TAGS) {
                    continue;
                }
                forgiveness_patched_weapon_tags[forgiveness_patched_weapon_tag_count].data = data;
                forgiveness_patched_weapon_tags[forgiveness_patched_weapon_tag_count].stock_autoaim = *reinterpret_cast<float *>(data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET);
                forgiveness_patched_weapon_tags[forgiveness_patched_weapon_tag_count].stock_deviation = *reinterpret_cast<float *>(data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET);
                forgiveness_patched_weapon_tag_count++;
            }
            *reinterpret_cast<float *>(data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET) = angle;
            *reinterpret_cast<float *>(data + WEAPON_TAG_DEVIATION_ANGLE_OFFSET) = angle;
        }
        if(forgiveness_patched_weapon_tag_count > 0) {
            forgiveness_weapon_tags_patched = true;
        }
    }

    static void hitreg_forgiveness_map_load() noexcept {
        // A map has STARTED loading. Set the flag FIRST: it is what
        // allows any tag memory to be read at all (see apply above).
        // NOTE: the map-load event fires at the START of the game's
        // map-load function -- on_map_load_client_sig (VA 0x443600 in
        // haloce.exe) is a function prologue, and Chimera's on_map_load
        // is installed as call_before there, so these events run
        // BEFORE the game has copied the map's tags into 0x40440000
        // (the disassembly of 0x443600 shows it is itself a
        // tag-loading function: it calls the 0x443410 tag-entry helper
        // and iterates tag entries). Do NOT trust the flag alone as
        // "tags are ready", and do NOT apply the angles here: on the
        // first map load the header is still zeroes (the tag region
        // was just allocated), and on a map change it is still the
        // PREVIOUS map's header, so applying here would patch stale
        // tags and mark the module as patched, and the in-game retry
        // would then never run for the new map. Just forget the old
        // pointers and let the retry
        // (retry_forgiveness_weapon_angles, from the tick event once
        // the player is alive in the loaded map) do the patching.
        forgiveness_map_loaded = true;
        // Forget any tag patched in a previous map: after a map change
        // the stored pointer is stale and must not be trusted.
        forgiveness_pistol_tag_data = nullptr;
        forgiveness_patched_weapon_tag_count = 0;
        forgiveness_weapon_tags_patched = false;
    }

    // Retry the pistol patch once the player is actually in a loaded
    // map. A live client biped proves the map is fully loaded and the
    // tag region (0x40440000) is mapped, so calling apply here is
    // safe. This runs from the tick event, so it retries every tick
    // until the tag is patched and then costs nothing (the stored
    // pointer is non-null, so it returns immediately).
    static void retry_forgiveness_weapon_angles(UnitDynamicObject *unit) noexcept {
        if(forgiveness_pistol_tag_data || forgiveness_weapon_tags_patched || !unit || unit->health <= 0.0f) {
            return;
        }
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
        // (No diagnostic capture here: at pre-tick time Controls
        // still holds the previous tick's consumed mouse delta, and
        // the delta just injected is ~0 because the post-tick nudge
        // already aligned the aim -- re-reading Controls here always
        // printed 0.00. The outgoing-aim diagnostic is captured in
        // the tick event instead, where the nudge is real.)
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

        // The player is alive in a loaded map: if the pistol tag was
        // never patched (the map-load event fired before the tags
        // were in), retry the patch now. This runs before the firing
        // check on purpose, so the angles are applied even before
        // the first shot.
        retry_forgiveness_weapon_angles(unit);

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

        // Read-only diagnostic: capture the nudge components (the
        // yaw/pitch needed to rotate the pre-nudge aim onto the
        // target -- the real correction, whose total angle is the
        // `last nudge` line) and the outgoing look increments
        // (0x6ABB40/0x6ABB44) at the same moment, so `show` can
        // compare the correction with the aim the client sends to
        // the server. Captured here, in the tick event, because
        // this is where the correction is non-zero: the pre-tick
        // injection sees an aim already aligned by the previous
        // tick's nudge and computes ~0.
        {
            float nudge_cross_z = aim.x*best_dir.y - aim.y*best_dir.x;
            float nudge_dot_xy = aim.x*best_dir.x + aim.y*best_dir.y;
            float nudge_yaw = std::atan2(nudge_cross_z, nudge_dot_xy);
            float nudge_aim_z = aim.z;
            if(nudge_aim_z > 1.0f) nudge_aim_z = 1.0f;
            if(nudge_aim_z < -1.0f) nudge_aim_z = -1.0f;
            float nudge_dir_z = best_dir.z;
            if(nudge_dir_z > 1.0f) nudge_dir_z = 1.0f;
            if(nudge_dir_z < -1.0f) nudge_dir_z = -1.0f;
            float nudge_pitch = std::asin(nudge_dir_z) - std::asin(nudge_aim_z);
            forgiveness_last_nudge_yaw_deg = static_cast<float>(RADIANS_TO_DEGREES(nudge_yaw));
            forgiveness_last_nudge_pitch_deg = static_cast<float>(RADIANS_TO_DEGREES(nudge_pitch));
            forgiveness_last_out_yaw_deg = static_cast<float>(RADIANS_TO_DEGREES(*reinterpret_cast<float *>(FORGIVENESS_OUTGOING_YAW_INCREMENT_ADDRESS)));
            forgiveness_last_out_pitch_deg = static_cast<float>(RADIANS_TO_DEGREES(*reinterpret_cast<float *>(FORGIVENESS_OUTGOING_PITCH_INCREMENT_ADDRESS)));
            forgiveness_have_last_outgoing_aim = true;
        }

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

    // Compute the bend (yaw/pitch deltas, radians) to rotate the
    // shooter's aim onto the best enemy inside the cone, for the
    // setter hooks. Returns false (and stores 0, 0) unless the
    // module is on, the cone is > 0, the local player is alive and
    // firing, and an enemy is inside the cone.
    //
    // Base aim: the aim the MOUSE is producing, not the nudged aim.
    // The post-tick nudge overwrites unit->aim/desired_aim with the
    // target direction, and the engine re-derives the aim from the
    // mouse look input every tick, so the aim the server will build
    // from the increments is the mouse aim. The record of it is
    // forgiveness_pre_nudge_aim: the aim just before the last
    // post-tick nudge (set in the tick event). If no nudge ran in
    // the previous tick (no pending aim check), the current aim IS
    // the mouse aim (nothing overwrote it) and it is used directly.
    // The base aim is one tick stale when it comes from the stored
    // pre-nudge aim; the error is small (the aim moves little in
    // one tick when the mouse is steady) and the bend is recomputed
    // on every setter call, so it tracks.
    static bool compute_forgiveness_bend(float &yaw_delta_rad, float &pitch_delta_rad) noexcept {
        yaw_delta_rad = 0.0f;
        pitch_delta_rad = 0.0f;
        if(!::hitreg_forgiveness_on || forgiveness_cone_degrees <= 0.0f) {
            return false;
        }

        auto *unit = get_client_unit();
        if(!unit || unit->health <= 0.0f) {
            return false;
        }

        // Only help while the trigger is actually held.
        if(!unit->primary_fire_button) {
            return false;
        }

        Point3D aim;
        if(forgiveness_have_pending_aim_check) {
            // A post-tick nudge ran last tick: the current aim is the
            // nudged aim, not the mouse aim. Use the aim from before
            // that nudge.
            aim = forgiveness_pre_nudge_aim;
        }
        else {
            aim = unit->aim;
        }
        float aim_len = std::sqrt(aim.x*aim.x + aim.y*aim.y + aim.z*aim.z);
        if(aim_len < 0.5f || aim_len > 2.0f) {
            return false;
        }
        aim.x /= aim_len; aim.y /= aim_len; aim.z /= aim_len;

        Point3D eye = unit->center_position;
        Point3D best_dir = {};
        float best_angle = 0.0f;
        if(!find_forgiveness_target(eye, aim, unit, best_dir, best_angle, nullptr)) {
            return false;
        }

        // Yaw (rotation about the world Z axis, positive = left) and
        // pitch (elevation difference, positive = up) needed to
        // rotate the aim onto the target. The angles are small (<=
        // the cone, <= 10 deg), so splitting the single rotation
        // into yaw + pitch is accurate.
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

        // Safety net: never bend more than the cone in a single
        // input update (within a normal mouse movement per tick).
        float max_delta = static_cast<float>(DEGREES_TO_RADIANS(forgiveness_cone_degrees));
        if(yaw_delta > max_delta) yaw_delta = max_delta;
        if(yaw_delta < -max_delta) yaw_delta = -max_delta;
        if(pitch_delta > max_delta) pitch_delta = max_delta;
        if(pitch_delta < -max_delta) pitch_delta = -max_delta;

        yaw_delta_rad = yaw_delta;
        pitch_delta_rad = pitch_delta;
        return true;
    }

    // Call-before for the yaw setter hook: compute the bend nudge
    // (both components) and store it for the call-after stubs.
    // Counts the bend (yaw setter only, so one per input update).
    static void hitreg_forgiveness_bend_yaw_before() noexcept {
        float yaw = 0.0f, pitch = 0.0f;
        if(compute_forgiveness_bend(yaw, pitch)) {
            forgiveness_bends++;
        }
        forgiveness_bend_yaw_rad = yaw;
        forgiveness_bend_pitch_rad = pitch;
    }

    // Call-before for the pitch setter hook: same nudge, recomputed
    // (the aim/target have not changed between the two setter calls
    // in one input update). Does not count, so `bends` is one per
    // input update, not two.
    static void hitreg_forgiveness_bend_pitch_before() noexcept {
        float yaw = 0.0f, pitch = 0.0f;
        compute_forgiveness_bend(yaw, pitch);
        forgiveness_bend_yaw_rad = yaw;
        forgiveness_bend_pitch_rad = pitch;
    }

    // Install the setter hooks for the bend. The setters are the
    // only writers of the look increments (verified in haloce.exe:
    // the only fstps to 0x6ABB40/0x6ABB44 are in the two setters),
    // so all look input -- mouse included -- flows through them.
    // The hook point is the converter call (0x48f490) inside each
    // setter: the overwritten instructions (mov edx,[eax+4]; push
    // edx; call 0x48f490) parse with Chimera's instruction reader,
    // and the call-after runs with the converted increment
    // (radians) in ST(0), so the stub adds the bend and the
    // following fstps stores it. The hook only fires when the
    // setter is actually going to write: the hook point is past
    // the index validation (0..3) and the null check, and after
    // it there is no further branch before the fstps.
    // The bend only applies to the local player: on PC there is a
    // single local player (index 0) and the setters are the local
    // input path, so the nudge is added to the local player's
    // increments. (The setter's player index is not visible to the
    // call-before, so this relies on that; split-screen/local
    // multi-player is out of scope for Halo CE PC.)
    static void install_forgiveness_bend_hooks() noexcept {
        if(forgiveness_bend_hooks_installed) {
            return;
        }
        forgiveness_bend_hooks_installed = true;
        // set_digital_yaw_increment (VA 0x4849B0): converter call at 0x4849EA.
        write_jmp_call(reinterpret_cast<void *>(0x4849EA), forgiveness_bend_yaw_hook, reinterpret_cast<const void *>(hitreg_forgiveness_bend_yaw_before), reinterpret_cast<const void *>(on_forgiveness_bend_yaw_after));
        // set_digital_pitch_increment (VA 0x484A80): converter call at 0x484ABA.
        write_jmp_call(reinterpret_cast<void *>(0x484ABA), forgiveness_bend_pitch_hook, reinterpret_cast<const void *>(hitreg_forgiveness_bend_pitch_before), reinterpret_cast<const void *>(on_forgiveness_bend_pitch_after));
    }

    void set_up_hitreg_forgiveness() noexcept {
        // The aim-assist hook itself is installed by set_up_aim_assist_fix();
        // this module only arms the extra flag and the forgiveness events.
        install_forgiveness_bend_hooks();
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

    std::uint64_t hitreg_forgiveness_bends() noexcept {
        return forgiveness_bends;
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

    // Outgoing look increments vs the nudge components, captured at
    // the last post-tick nudge (firing at a live target inside the
    // cone), in degrees. Returns false if no nudge has fired yet
    // (nothing captured). Read-only: this only reads game memory,
    // it never writes it.
    bool hitreg_forgiveness_last_outgoing_aim(float &out_yaw_deg, float &out_pitch_deg, float &nudge_yaw_deg, float &nudge_pitch_deg) noexcept {
        if(!forgiveness_have_last_outgoing_aim) {
            return false;
        }
        out_yaw_deg = forgiveness_last_out_yaw_deg;
        out_pitch_deg = forgiveness_last_out_pitch_deg;
        nudge_yaw_deg = forgiveness_last_nudge_yaw_deg;
        nudge_pitch_deg = forgiveness_last_nudge_pitch_deg;
        return true;
    }

    // Pistol autoaim angle currently applied, in degrees.
    // -1: no map loaded (menu, or DLL init) -- tag memory is never
    //     touched in this case.
    // -2: a map IS loaded but the pistol tag was not found/patched.
    // >= 0: the angle actually in the tag (should match the cone).
    float hitreg_forgiveness_weapon_autoaim_deg() noexcept {
        if(!forgiveness_map_loaded) {
            return -1.0f;
        }
        // Only touch tag memory when a live client biped proves a map
        // is fully loaded. In the menu the tag region may be unmapped,
        // and reading it would fault -- the same crash that kept the
        // DLL from loading in 562f3b8/575f51d. This also makes `show`
        // self-healing: the first `show` in a loaded map applies the
        // patch (the map-load event fired before the tags were in).
        auto *unit = get_client_unit();
        if(!unit || unit->health <= 0.0f) {
            return -1.0f;
        }
        retry_forgiveness_weapon_angles(unit);
        // Pistol patched by path: report its angle, but only if the
        // stored pointer is still this map's pistol tag: after a map
        // change it may be stale, and reading it would return garbage
        // (or fault if unmapped).
        if(forgiveness_pistol_tag_data && forgiveness_tags_loaded()) {
            auto *tag = get_tag(FORGIVENESS_PISTOL_TAG_PATH, TagClassInt::TAG_CLASS_WEAPON);
            if(tag && tag->data && tag->data == forgiveness_pistol_tag_data) {
                float angle = *reinterpret_cast<float *>(forgiveness_pistol_tag_data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET);
                return static_cast<float>(RADIANS_TO_DEGREES(angle));
            }
            return -2.0f;
        }
        // Patched by the class scan: every patched tag got the same
        // angle, so report the first one's.
        if(forgiveness_patched_weapon_tag_count > 0) {
            float angle = *reinterpret_cast<float *>(forgiveness_patched_weapon_tags[0].data + WEAPON_TAG_AUTOAIM_ANGLE_OFFSET);
            return static_cast<float>(RADIANS_TO_DEGREES(angle));
        }
        return -2.0f;
    }

    // Number of tags in the loaded map's tag array, or 0 if no map
    // is loaded. Only touches tag memory when a live client biped
    // proves a map is fully loaded (see the getter above).
    std::uint64_t hitreg_forgiveness_tag_count() noexcept {
        if(!forgiveness_map_loaded) {
            return 0;
        }
        auto *unit = get_client_unit();
        if(!unit || unit->health <= 0.0f) {
            return 0;
        }
        if(!forgiveness_tags_loaded()) {
            return 0;
        }
        return get_tag_data_header().tag_count;
    }

    // Number of weapon-class tags in the loaded map's tag array, or
    // 0 if no map is loaded. This scans by class only and never
    // reads a tag path, so it works even on maps whose paths are
    // scrambled (protected/custom maps). If this is > 0 while the
    // pistol is "not found", the path lookup is the problem, not
    // the tag array.
    std::uint64_t hitreg_forgiveness_weapon_tag_count() noexcept {
        if(!forgiveness_map_loaded) {
            return 0;
        }
        auto *unit = get_client_unit();
        if(!unit || unit->health <= 0.0f) {
            return 0;
        }
        if(!forgiveness_tags_loaded()) {
            return 0;
        }
        const auto &header = get_tag_data_header();
        std::uint64_t count = 0;
        for(std::size_t i = 0; i < header.tag_count; i++) {
            if(header.tag_array[i].primary_class == TagClassInt::TAG_CLASS_WEAPON) {
                count++;
            }
        }
        return count;
    }

    void hitreg_forgiveness_reset_stats() noexcept {
        forgiveness_nudges = 0;
        forgiveness_candidates = 0;
        forgiveness_no_candidate = 0;
        forgiveness_last_nudge_angle = -1.0f;
        forgiveness_injections = 0;
        forgiveness_bends = 0;
        forgiveness_bend_yaw_rad = 0.0f;
        forgiveness_bend_pitch_rad = 0.0f;
        forgiveness_have_pending_aim_check = false;
        forgiveness_aim_kept = 0;
        forgiveness_aim_erased = 0;
        forgiveness_aim_drifted = 0;
        forgiveness_last_aim_drift_deg = -1.0f;
        forgiveness_have_last_outgoing_aim = false;
        forgiveness_last_out_yaw_deg = 0.0f;
        forgiveness_last_out_pitch_deg = 0.0f;
        forgiveness_last_nudge_yaw_deg = 0.0f;
        forgiveness_last_nudge_pitch_deg = 0.0f;
    }
}
