;# SPDX-License-Identifier: GPL-3.0-only

.intel_syntax noprefix

.text
.globl _on_aim_assist
_on_aim_assist:
    mov eax, [_using_analog_movement]
    cmp byte ptr [eax], 1
    jz do_aim_assist
    cmp byte ptr [_hitreg_forgiveness_on], 1
    jz do_aim_assist
    jmp dword ptr [_not_using_analog_movement_jmp]
    do_aim_assist:
    jmp dword ptr [_yes_using_analog_movement_jmp]
