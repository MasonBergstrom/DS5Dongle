//
// Created by awalol on 2026/8/14.
//

#include <cstdint>

#include "button_remap.h"
#include "button_utils.h"
#include "config.h"
#include "utils.h"

static Direction get_dpad_direction(int index) {
    switch (index) {
        case DPadNorth: return North;
        case DPadNorthEast: return NorthEast;
        case DPadEast: return East;
        case DPadSouthEast: return SouthEast;
        case DPadSouth: return South;
        case DPadSouthWest: return SouthWest;
        case DPadWest: return West;
        case DPadNorthWest: return NorthWest;
        default: return None;
    }
}

static void disable_state_value(USBGetStateData &state_data, int index) {
    switch (index) {
        case DPadNorth:
        case DPadNorthEast:
        case DPadEast:
        case DPadSouthEast:
        case DPadSouth:
        case DPadSouthWest:
        case DPadWest:
        case DPadNorthWest:
            if (state_data.DPad == get_dpad_direction(index)) {
                state_data.DPad = None;
            }
            break;
        case ButtonSquare: state_data.ButtonSquare = 0; break;
        case ButtonCross: state_data.ButtonCross = 0; break;
        case ButtonCircle: state_data.ButtonCircle = 0; break;
        case ButtonTriangle: state_data.ButtonTriangle = 0; break;
        case ButtonL1: state_data.ButtonL1 = 0; break;
        case ButtonR1: state_data.ButtonR1 = 0; break;
        case ButtonL2: state_data.ButtonL2 = 0; break;
        case ButtonR2: state_data.ButtonR2 = 0; break;
        case ButtonCreate: state_data.ButtonCreate = 0; break;
        case ButtonOptions: state_data.ButtonOptions = 0; break;
        case ButtonL3: state_data.ButtonL3 = 0; break;
        case ButtonR3: state_data.ButtonR3 = 0; break;
        case ButtonHome: state_data.ButtonHome = 0; break;
        case ButtonPad: state_data.ButtonPad = 0; break;
        case ButtonMute: state_data.ButtonMute = 0; break;
        case ButtonLeftFunction: state_data.ButtonLeftFunction = 0; break;
        case ButtonRightFunction: state_data.ButtonRightFunction = 0; break;
        case ButtonLeftPaddle: state_data.ButtonLeftPaddle = 0; break;
        case ButtonRightPaddle: state_data.ButtonRightPaddle = 0; break;
        default: break;
    }
}

void button_remap_apply(USBGetStateData &state_data) {
    // 函数逻辑：flash 保存按键映射表
    // key: 原始按键 value: 目标按键
    //
    // 先将有映射目标的按钮的状态设为 0 未按下
    // 然后遍历按键映射表。
    // 如果目标按键有映射，那就将目标按键的状态设置为原始按键状态

    const auto &remap_table = get_button().button_remap;
    const USBGetStateData state_copy = state_data;

    // Clear every configured physical source first. Targets are populated from
    // state_copy below, so mappings do not depend on enum order.
    for (uint8_t i = 0; i < sizeof(remap_table); i++) {
        if (remap_table[i] != i) {
            disable_state_value(state_data, i);
        }
    }

    // Button remap
    for (uint8_t i = 0; i < sizeof(remap_table); i++) {
        if (remap_table[i] == i || remap_table[i] == Disable) {
            continue;
        }
        const bool pressed = button_is_pressed(state_copy, i);

        switch (remap_table[i]) {
            // DPad
            case DPadNorth:
            case DPadNorthEast:
            case DPadEast:
            case DPadSouthEast:
            case DPadSouth:
            case DPadSouthWest:
            case DPadWest:
            case DPadNorthWest: {
                if (pressed) {
                    state_data.DPad = get_dpad_direction(remap_table[i]);
                }
                break;
            }
            // Button
            case ButtonSquare: state_data.ButtonSquare |= pressed; break;
            case ButtonCross: state_data.ButtonCross |= pressed; break;
            case ButtonCircle: state_data.ButtonCircle |= pressed; break;
            case ButtonTriangle: state_data.ButtonTriangle |= pressed; break;
            case ButtonL1: state_data.ButtonL1 |= pressed; break;
            case ButtonR1: state_data.ButtonR1 |= pressed; break;
            case ButtonL2: state_data.ButtonL2 |= pressed; break;
            case ButtonR2: state_data.ButtonR2 |= pressed; break;
            case ButtonCreate: state_data.ButtonCreate |= pressed; break;
            case ButtonOptions: state_data.ButtonOptions |= pressed; break;
            case ButtonL3: state_data.ButtonL3 |= pressed; break;
            case ButtonR3: state_data.ButtonR3 |= pressed; break;
            case ButtonHome: state_data.ButtonHome |= pressed; break;
            case ButtonPad: state_data.ButtonPad |= pressed; break;
            case ButtonMute: state_data.ButtonMute |= pressed; break;
            case ButtonLeftFunction: state_data.ButtonLeftFunction |= pressed; break;
            case ButtonRightFunction: state_data.ButtonRightFunction |= pressed; break;
            case ButtonLeftPaddle: state_data.ButtonLeftPaddle |= pressed; break;
            case ButtonRightPaddle: state_data.ButtonRightPaddle |= pressed; break;
            default: {
                break;
            }
        }
    }
}
