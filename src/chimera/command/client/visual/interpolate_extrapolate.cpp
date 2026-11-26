// SPDX-License-Identifier: GPL-3.0-only

#include "../../command.hpp"
#include "../../../fix/interpolate/interpolate.hpp"
#include "../../../output/output.hpp"

namespace Chimera {
    bool interpolate_extrapolate_command(int argc, const char **argv) {
        if(argc) {
            interpolate_extrapolate = STR_TO_BOOL(argv[0]);
        }
        console_output(BOOL_TO_STR(interpolate_extrapolate));
        return true;
    }
}
