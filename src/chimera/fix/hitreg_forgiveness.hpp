// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_HITREG_FORGIVENESS_HPP
#define CHIMERA_HITREG_FORGIVENESS_HPP

#include <cstdint>

namespace Chimera {
    /**
     * Set up the hitreg forgiveness module. This starts the post-tick
     * aim nudge that helps shots land when the crosshair is very
     * close to (but not quite on) an enemy. The nudge runs after the
     * engine's tick (a pre-tick write is discarded by the engine's
     * per-tick aim recomputation) and corrects the aim together with
     * the first-person camera, so the crosshair and the bullet agree.
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
     */
    std::uint64_t hitreg_forgiveness_nudges() noexcept;
    std::uint64_t hitreg_forgiveness_candidates() noexcept;
    std::uint64_t hitreg_forgiveness_no_candidate() noexcept;
    float hitreg_forgiveness_last_nudge_angle() noexcept;
    void hitreg_forgiveness_reset_stats() noexcept;
}

#endif
