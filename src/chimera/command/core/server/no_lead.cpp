// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>

#include "../../command.hpp"
#include "../../../output/output.hpp"
#include "../../../fix/no_lead.hpp"

namespace Chimera {
    bool no_lead_command(int argc, const char **argv) {
        if(argc) {
            bool enable;

            if(
                std::strcmp(argv[0], "true") == 0 ||
                std::strcmp(argv[0], "1") == 0 ||
                std::strcmp(argv[0], "on") == 0
            ) {
                enable = true;
            }
            else if(
                std::strcmp(argv[0], "false") == 0 ||
                std::strcmp(argv[0], "0") == 0 ||
                std::strcmp(argv[0], "off") == 0
            ) {
                enable = false;
            }
            else {
                console_error("Usage: chimera_no_lead [true|false]");
                return false;
            }

            set_no_lead_enabled(enable);
        }

        console_output(BOOL_TO_STR(no_lead_enabled()));
        return true;
    }
}
