//
// Created by awalol on 2026/8/14.
//

#ifndef DS5_BRIDGE_BUTTON_MAP_H
#define DS5_BRIDGE_BUTTON_MAP_H
#include <cstdint>

struct USBGetStateData;

enum ButtonId: uint8_t {
    // DPad
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
    ButtonLeftFunction, // DualSense Edge
    ButtonRightFunction, // DualSense Edge
    ButtonLeftPaddle, // DualSense Edge
    ButtonRightPaddle, // DualSense Edge

    Disable,
};

struct ButtonMapInfo {
};

void button_remap_apply(USBGetStateData& state_data);


#endif //DS5_BRIDGE_BUTTON_MAP_H
