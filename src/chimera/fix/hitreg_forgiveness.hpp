// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_HITREG_FORGIVENESS_HPP
#define CHIMERA_HITREG_FORGIVENESS_HPP

#include <cstdint>

namespace Chimera {
    /**
     * Set up the hitreg forgiveness module. The part that reaches the
     * shot is the weapon-tag route: the pistol's autoaim angle (and
     * deviation cap) in the loaded map's tags are widened to the
     * forgiveness cone, so the engine's own fire-time autoaim bends
     * near-miss shots onto the enemy. The older aim/camera/look
     * nudges are kept as a refinement, but the engine re-derives the
     * aim every tick from the mouse look input and erases them
     * (diagnostic: aim erased 41 vs aim kept 2), so on their own
     * they do not make shots land.
     */
    void set_up_hitreg_forgiveness() noexcept;

    /** Enable or disable the whole module. When disabled, the aim nudge stops and the engine's aim assist behaves exactly like stock saosin. */
    void set_hitreg_forgiveness_enabled(bool enabled) noexcept;
    bool hitreg_forgiveness_enabled() noexcept;

    /** Forgiveness cone in degrees. Shots fired while the aim is within this angle of an enemy's center are pulled onto the enemy. 0 disables the nudge. */
    void set_hitreg_forgiveness_cone(float degrees) noexcept;
    float hitreg_forgiveness_cone() noexcept;

    /**
     * Diagnostic counters for the forgiveness nudge. Read-only: they never
     * change gameplay, they only record what the nudge did.
     * - nudges: times the aim was actually pulled onto an enemy.
     * - candidates: enemies found inside the cone while firing.
     * - no_candidate: scans while firing that found no enemy in the cone.
     * - last_nudge_angle: angle (degrees) of the last nudge, or -1 if none yet.
     * - injections: times the pre-tick look injection fired.
     * - aim_kept / aim_erased / aim_drifted: whether the previous tick's
     *   correction was still on the aim when the next tick ran (kept),
     *   was back to the pre-nudge aim (erased), or neither (drifted).
     * - last_aim_drift: angle (degrees) between the aim and the last
     *   nudged direction at the last check, or -1 if none yet.
     */
    std::uint64_t hitreg_forgiveness_nudges() noexcept;
    std::uint64_t hitreg_forgiveness_candidates() noexcept;
    std::uint64_t hitreg_forgiveness_no_candidate() noexcept;
    float hitreg_forgiveness_last_nudge_angle() noexcept;
    std::uint64_t hitreg_forgiveness_injections() noexcept;
    std::uint64_t hitreg_forgiveness_aim_kept() noexcept;
    std::uint64_t hitreg_forgiveness_aim_erased() noexcept;
    std::uint64_t hitreg_forgiveness_aim_drifted() noexcept;
    float hitreg_forgiveness_last_aim_drift() noexcept;
    void hitreg_forgiveness_reset_stats() noexcept;

    /**
     * Outgoing look increments vs the nudge components, captured at
     * the last post-tick nudge (firing at a live target inside the
     * cone), in degrees. Read-only diagnostic: it only reads game
     * memory, it never writes it.
     * - out_yaw_deg / out_pitch_deg: the per-player action look
     *   increments (yaw and pitch deltas, in radians in the game)
     *   for the local player -- the aim the client sends to the
     *   server in the player action. The client does NOT send
     *   absolute yaw/pitch; it sends these per-tick increments.
     * - nudge_yaw_deg / nudge_pitch_deg: the yaw/pitch components
     *   of the correction the forgiveness applied at that moment
     *   (whose total angle is the `last nudge` line).
     * Returns false if no nudge has fired yet (fire at an enemy
     * first). If the out values do not include the nudge, the
     * correction never reaches the action the server receives --
     * which is why the injected correction does not make off-red
     * shots land. (The pre-tick injection writes Controls, which
     * the engine's action-apply function zeroes and rebuilds from
     * the action increments every tick, so the nudge can only
     * reach the server by bending the increments themselves.)
     */
    bool hitreg_forgiveness_last_outgoing_aim(float &out_yaw_deg, float &out_pitch_deg, float &nudge_yaw_deg, float &nudge_pitch_deg) noexcept;

    /**
     * Pistol autoaim angle currently applied by the module, in
     * degrees, or -1 if no map is loaded (menu) and -2 if a map is
     * loaded but no weapon tag was found/patched. This is what
     * the engine's fire-time autoaim will bend bullets within; it
     * should match the forgiveness cone. Calling this in a loaded
     * map retries the patch if it was never applied (the map-load
     * event fires before the tags are in). The pistol is found by
     * path first; if that fails, every weapon-class tag in the map
     * is patched instead (the tag class is never scrambled, even
     * on protected/custom maps whose paths are), and this reports
     * the angle of the first one patched.
     */
    float hitreg_forgiveness_weapon_autoaim_deg() noexcept;

    /**
     * Number of tags in the loaded map (tag_count from the tag
     * header), and number of weapon-class tags found by scanning
     * the tag array by class (no path is read). Both are 0 when no
     * map is loaded. Read-only diagnostics for the weapon-tag
     * lookup: if the tag count is > 0 but the pistol is "not
     * found", the path lookup is the problem, not the tag array.
     */
    std::uint64_t hitreg_forgiveness_tag_count() noexcept;
    std::uint64_t hitreg_forgiveness_weapon_tag_count() noexcept;
}

#endif
