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

        // show/reset are diagnostic subcommands: they only read or clear the
        // counters, they never change the module's settings or gameplay.
        // show prints the diagnostic counters on several short lines,
        // so they fit in the in-game console (one long line gets cut off).
        if(std::strcmp(argv[0], "show") == 0) {
            float last_angle = hitreg_forgiveness_last_nudge_angle();
            float last_drift = hitreg_forgiveness_last_aim_drift();
            float weapon_autoaim = hitreg_forgiveness_weapon_autoaim_deg();
            console_output("chimera_hitreg_forgiveness: %s (cone %.1f deg)", BOOL_TO_STR(hitreg_forgiveness_enabled()), static_cast<double>(hitreg_forgiveness_cone()));
            if(weapon_autoaim < -1.5f) {
                console_output("pistol autoaim: tag not found");
            }
            else if(weapon_autoaim < 0.0f) {
                console_output("pistol autoaim: no map");
            }
            else {
                console_output("pistol autoaim %.1f deg", static_cast<double>(weapon_autoaim));
            }
            if(weapon_autoaim != -1.0f) {
                console_output("tags %llu | weapon tags %llu", static_cast<unsigned long long>(hitreg_forgiveness_tag_count()), static_cast<unsigned long long>(hitreg_forgiveness_weapon_tag_count()));
            }
            console_output("nudges %llu | candidates %llu | no candidate %llu", static_cast<unsigned long long>(hitreg_forgiveness_nudges()), static_cast<unsigned long long>(hitreg_forgiveness_candidates()), static_cast<unsigned long long>(hitreg_forgiveness_no_candidate()));
            if(last_angle < 0.0f) {
                console_output("last nudge: none yet | injections %llu", static_cast<unsigned long long>(hitreg_forgiveness_injections()));
            }
            else {
                console_output("last nudge %.2f deg | injections %llu", static_cast<double>(last_angle), static_cast<unsigned long long>(hitreg_forgiveness_injections()));
            }
            // Bend in the look increments: how many times the setter
            // hooks armed a non-zero nudge. If bends is 0 while
            // nudges grows, the bend never ran (module off, not
            // firing, or no enemy in the cone at input time).
            console_output("bends %llu", static_cast<unsigned long long>(hitreg_forgiveness_bends()));
            console_output("aim kept %llu | aim erased %llu | aim drifted %llu", static_cast<unsigned long long>(hitreg_forgiveness_aim_kept()), static_cast<unsigned long long>(hitreg_forgiveness_aim_erased()), static_cast<unsigned long long>(hitreg_forgiveness_aim_drifted()));
            if(last_drift < 0.0f) {
                console_output("last drift: none yet");
            }
            else {
                console_output("last drift %.2f deg", static_cast<double>(last_drift));
            }
            // Outgoing aim diagnostic (read-only): the nudge the
            // forgiveness applied, vs the look increments the
            // client sends to the server, both captured at the last
            // post-tick nudge (while firing at a live target inside
            // the cone). With the bend in the setters, the out
            // values should carry the nudge (out = raw mouse delta
            // + nudge); if they do not, the correction still never
            // reaches the action the server receives.
            float out_yaw = 0.0f, out_pitch = 0.0f, nudge_yaw = 0.0f, nudge_pitch = 0.0f;
            if(hitreg_forgiveness_last_outgoing_aim(out_yaw, out_pitch, nudge_yaw, nudge_pitch)) {
                console_output("nudge yaw %.2f deg | pitch %.2f deg", static_cast<double>(nudge_yaw), static_cast<double>(nudge_pitch));
                console_output("out yaw %.2f deg | pitch %.2f deg", static_cast<double>(out_yaw), static_cast<double>(out_pitch));
            }
            else {
                console_output("out aim: none yet (fire at an enemy first)");
            }
            return true;
        }
        if(std::strcmp(argv[0], "reset") == 0) {
            hitreg_forgiveness_reset_stats();
            console_output("chimera_hitreg_forgiveness: diagnostic counters reset");
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
