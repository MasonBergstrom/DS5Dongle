#ifndef DS5_BRIDGE_BUTTON_UTILS_H
#define DS5_BRIDGE_BUTTON_UTILS_H

#include <cstdint>

struct USBGetStateData;

enum ButtonId : uint8_t {
    DPadNorth = 0,
    DPadNorthEast,
    DPadEast,
    DPadSouthEast,
    DPadSouth,
    DPadSouthWest,
    DPadWest,
    DPadNorthWest,

    ButtonSquare,
    ButtonCross,
    ButtonCircle,
    ButtonTriangle,
    ButtonL1,
    ButtonR1,
    ButtonL2,
    ButtonR2,
    ButtonCreate,
    ButtonOptions,
    ButtonL3,
    ButtonR3,
    ButtonHome,
    ButtonPad,
    ButtonMute,
    ButtonLeftFunction,
    ButtonRightFunction,
    ButtonLeftPaddle,
    ButtonRightPaddle,

    Disable,
};

bool button_is_pressed(const USBGetStateData& state_data, uint8_t button);

#endif // DS5_BRIDGE_BUTTON_UTILS_H
