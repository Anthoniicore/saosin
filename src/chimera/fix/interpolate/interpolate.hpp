// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_INTERPOLATE_HPP
#define CHIMERA_INTERPOLATE_HPP

namespace Chimera {
    /**
     * If true, rendered bipeds/vehicles are extrapolated one tick ahead (see
     * object.cpp) so what you see matches where the object will be when the
     * next tick simulates it. Enabled by default; turn it off with
     * chimera_interpolate_extrapolate false to go back to plain interpolation.
     */
    extern bool interpolate_extrapolate;

    void set_up_interpolation() noexcept;
    void disable_interpolation() noexcept;
}

#endif
