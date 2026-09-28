// SPDX-License-Identifier: GPL-3.0-only

#ifndef CHIMERA_NO_LEAD_HPP
#define CHIMERA_NO_LEAD_HPP

namespace Chimera {
    void set_up_no_lead_fix() noexcept;
    bool no_lead_enabled() noexcept;
    void set_no_lead_enabled(bool enabled) noexcept;
}

#endif
