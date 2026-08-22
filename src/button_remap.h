//
// Created by awalol on 2026/8/14.
//

#ifndef DS5_BRIDGE_BUTTON_MAP_H
#define DS5_BRIDGE_BUTTON_MAP_H

#include <cstdint>

constexpr uint8_t BUTTON_REMAP_COUNT = 28;

struct USBGetStateData;
void button_remap_apply(USBGetStateData& state_data);


#endif //DS5_BRIDGE_BUTTON_MAP_H
