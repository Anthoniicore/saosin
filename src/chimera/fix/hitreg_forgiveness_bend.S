;# SPDX-License-Identifier: GPL-3.0-only
#
# Call-after stubs for the forgiveness bend in the look increments.
#
# The game writes the per-tick look increments (the aim the client sends
# to the server in the player action) through set_digital_yaw_increment
# (VA 0x4849B0) and set_digital_pitch_increment (VA 0x484A80). Each
# setter converts its milli-turns value to radians (0x48f490) and stores
# the float from the x87 stack (fstps) into the action struct
# (0x6ABB40/0x6ABB44). The forgiveness hooks those setters with a
# call-after trampoline right after the converter call, so when these
# stubs run, ST(0) is the converted increment in radians and the
# following fstps will store it. Each stub adds the bend nudge (in
# radians, computed by the C++ call-before and stored in the matching
# global) to ST(0), so the stored increment is the mouse delta plus
# the bend onto the enemy. The stubs only touch the x87 stack (one
# push, one add-and-pop, so the stack depth is unchanged) and return;
# the hook stub preserves the general registers around them.

.intel_syntax noprefix

.text
.globl _on_forgiveness_bend_yaw_after
_on_forgiveness_bend_yaw_after:
    fld dword ptr [_forgiveness_bend_yaw_rad]
    faddp st(1), st
    ret

.globl _on_forgiveness_bend_pitch_after
_on_forgiveness_bend_pitch_after:
    fld dword ptr [_forgiveness_bend_pitch_rad]
    faddp st(1), st
    ret
