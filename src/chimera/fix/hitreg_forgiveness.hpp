// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_HITREG_FORGIVENESS_HPP
#define CHIMERA_HITREG_FORGIVENESS_HPP

namespace Chimera {
    /**
     * Set up the hitreg forgiveness module. This starts the pre-tick
     * aim nudge that helps shots land when the crosshair is very
     * close to (but not quite on) an enemy.
     */
    void set_up_hitreg_forgiveness() noexcept;

    /** Enable or disable the whole module. When disabled, the aim nudge stops and the engine's aim assist behaves exactly like stock saosin. */
    void set_hitreg_forgiveness_enabled(bool enabled) noexcept;
    bool hitreg_forgiveness_enabled() noexcept;

    /** Forgiveness cone in degrees. Shots fired while the aim is within this angle of an enemy's center are pulled onto the enemy. 0 disables the nudge. */
    void set_hitreg_forgiveness_cone(float degrees) noexcept;
    float hitreg_forgiveness_cone() noexcept;
}

#endif
