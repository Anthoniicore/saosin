// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>

#include "../../command.hpp"
#include "../../../output/output.hpp"
#include "../../../fix/interpolate/interpolate.hpp"

namespace Chimera {
    bool interpolate_command(int argc, const char **argv) {
        if(argc) {
            if(std::strcmp(argv[0], "extrapolation") == 0 ||
               std::strcmp(argv[0], "predictive") == 0) {
                set_interpolation_mode(InterpolationMode::EXTRAPOLATION);
            }
            else if(std::strcmp(argv[0], "traditional") == 0 ||
                    std::strcmp(argv[0], "true") == 0 ||
                    std::strcmp(argv[0], "1") == 0) {
                set_interpolation_mode(InterpolationMode::TRADITIONAL);
            }
            else if(std::strcmp(argv[0], "off") == 0 ||
                    std::strcmp(argv[0], "false") == 0 ||
                    std::strcmp(argv[0], "0") == 0) {
                set_interpolation_mode(InterpolationMode::OFF);
            }
            else {
                console_error("Usage: chimera_interpolate [true|false|traditional|extrapolation]");
                return false;
            }
        }

        switch(get_interpolation_mode()) {
            case InterpolationMode::TRADITIONAL:
                console_output("true");
                break;
            case InterpolationMode::EXTRAPOLATION:
                console_output("extrapolation");
                break;
            case InterpolationMode::OFF:
            default:
                console_output("false");
                break;
        }

        return true;
    }
}
