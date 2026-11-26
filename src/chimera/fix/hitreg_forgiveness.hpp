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
     * Pistol autoaim angle currently applied by the module, in
     * degrees, or -1 if the pistol tag was not found in the loaded
     * map. This is what the engine's fire-time autoaim will bend
     * bullets within; it should match the forgiveness cone.
     */
    float hitreg_forgiveness_weapon_autoaim_deg() noexcept;
}

#endif
