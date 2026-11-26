// SPDX-License-Identifier: GPL-3.0-only

#include <cstdlib>
#include <cstring>

#include "../../command.hpp"
#include "../../../fix/hitreg_forgiveness.hpp"
#include "../../../output/output.hpp"

namespace Chimera {
    bool hitreg_forgiveness_command(int argc, const char **argv) {
        if(argc == 0) {
            console_output("chimera_hitreg_forgiveness: %s (cone %.1f deg)", BOOL_TO_STR(hitreg_forgiveness_enabled()), static_cast<double>(hitreg_forgiveness_cone()));
            return true;
        }

        // true/false/on/off toggles the whole module
        if(std::strcmp(argv[0], "true") == 0 || std::strcmp(argv[0], "1") == 0 || std::strcmp(argv[0], "on") == 0) {
            set_hitreg_forgiveness_enabled(true);
        }
        else if(std::strcmp(argv[0], "false") == 0 || std::strcmp(argv[0], "0") == 0 || std::strcmp(argv[0], "off") == 0) {
            set_hitreg_forgiveness_enabled(false);
        }
        else {
            // Otherwise the argument is the forgiveness cone in degrees (0-10).
            float cone = static_cast<float>(std::atof(argv[0]));
            if(cone < 0.0f || cone > 10.0f) {
                console_error("Cone must be between 0.0 and 10.0 degrees (or true/false)");
                return false;
            }
            set_hitreg_forgiveness_cone(cone);
            set_hitreg_forgiveness_enabled(true);
        }

        console_output("chimera_hitreg_forgiveness: %s (cone %.1f deg)", BOOL_TO_STR(hitreg_forgiveness_enabled()), static_cast<double>(hitreg_forgiveness_cone()));
        return true;
    }
}
