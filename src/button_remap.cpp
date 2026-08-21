//
// Created by awalol on 2026/8/14.
//

#include <cstdint>

#include "button_remap.h"
#include "config.h"
#include "utils.h"

static uint8_t get_state_value(const USBGetStateData &state_data, int index) {
    switch (index) {
        case DPadNorth: return state_data.DPad == North;
        case DPadNorthEast: return state_data.DPad == NorthEast;
        case DPadEast: return state_data.DPad == East;
        case DPadSouthEast: return state_data.DPad == SouthEast;
        case DPadSouth: return state_data.DPad == South;
        case DPadSouthWest: return state_data.DPad == SouthWest;
        case DPadWest: return state_data.DPad == West;
        case DPadNorthWest: return state_data.DPad == NorthWest;
        case ButtonSquare: return state_data.ButtonSquare;
        case ButtonCross: return state_data.ButtonCross;
        case ButtonCircle: return state_data.ButtonCircle;
        case ButtonTriangle: return state_data.ButtonTriangle;
        case ButtonL1: return state_data.ButtonL1;
        case ButtonR1: return state_data.ButtonR1;
        case ButtonL2: return state_data.ButtonL2;
        case ButtonR2: return state_data.ButtonR2;
        case ButtonCreate: return state_data.ButtonCreate;
        case ButtonOptions: return state_data.ButtonOptions;
        case ButtonL3: return state_data.ButtonL3;
        case ButtonR3: return state_data.ButtonR3;
        case ButtonHome: return state_data.ButtonHome;
        case ButtonPad: return state_data.ButtonPad;
        case ButtonMute: return state_data.ButtonMute;
        case ButtonLeftFunction: return state_data.ButtonLeftFunction;
        case ButtonRightFunction: return state_data.ButtonRightFunction;
        case ButtonLeftPaddle: return state_data.ButtonLeftPaddle;
        case ButtonRightPaddle: return state_data.ButtonRightPaddle;
        default: return 0;
    }
}

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
                if (get_state_value(state_copy, i)) {
                    state_data.DPad = get_dpad_direction(remap_table[i]);
                }
                break;
            }
            // Button
            case ButtonSquare: state_data.ButtonSquare |= get_state_value(state_copy, i); break;
            case ButtonCross: state_data.ButtonCross |= get_state_value(state_copy, i); break;
            case ButtonCircle: state_data.ButtonCircle |= get_state_value(state_copy, i); break;
            case ButtonTriangle: state_data.ButtonTriangle |= get_state_value(state_copy, i); break;
            case ButtonL1: state_data.ButtonL1 |= get_state_value(state_copy, i); break;
            case ButtonR1: state_data.ButtonR1 |= get_state_value(state_copy, i); break;
            case ButtonL2: state_data.ButtonL2 |= get_state_value(state_copy, i); break;
            case ButtonR2: state_data.ButtonR2 |= get_state_value(state_copy, i); break;
            case ButtonCreate: state_data.ButtonCreate |= get_state_value(state_copy, i); break;
            case ButtonOptions: state_data.ButtonOptions |= get_state_value(state_copy, i); break;
            case ButtonL3: state_data.ButtonL3 |= get_state_value(state_copy, i); break;
            case ButtonR3: state_data.ButtonR3 |= get_state_value(state_copy, i); break;
            case ButtonHome: state_data.ButtonHome |= get_state_value(state_copy, i); break;
            case ButtonPad: state_data.ButtonPad |= get_state_value(state_copy, i); break;
            case ButtonMute: state_data.ButtonMute |= get_state_value(state_copy, i); break;
            case ButtonLeftFunction: state_data.ButtonLeftFunction |= get_state_value(state_copy, i); break;
            case ButtonRightFunction: state_data.ButtonRightFunction |= get_state_value(state_copy, i); break;
            case ButtonLeftPaddle: state_data.ButtonLeftPaddle |= get_state_value(state_copy, i); break;
            case ButtonRightPaddle: state_data.ButtonRightPaddle |= get_state_value(state_copy, i); break;
            default: {
                break;
            }
        }
    }
}
