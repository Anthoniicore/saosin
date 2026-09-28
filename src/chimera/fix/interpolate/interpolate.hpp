// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_INTERPOLATE_HPP
#define CHIMERA_INTERPOLATE_HPP

namespace Chimera {
    /**
     * How remote/object interpolation should be rendered.
     *
     * TRADITIONAL preserves the existing previous-tick -> current-tick
     * interpolation. EXTRAPOLATION renders bipeds from the current state
     * forward by the fraction of the current tick.
     */
    enum class InterpolationMode {
        OFF,
        TRADITIONAL,
        EXTRAPOLATION
    };

    void set_up_interpolation() noexcept;
    void disable_interpolation() noexcept;

    void set_interpolation_mode(InterpolationMode mode) noexcept;
    InterpolationMode get_interpolation_mode() noexcept;
}

#endif
