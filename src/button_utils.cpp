#include "button_utils.h"

#include "utils.h"

bool button_is_pressed(const USBGetStateData &state_data, uint8_t button) {
    switch (button) {
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
        default: return false;
    }
}
