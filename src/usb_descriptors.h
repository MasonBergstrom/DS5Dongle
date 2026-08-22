#ifndef DS5_BRIDGE_USB_DESCRIPTORS_H
#define DS5_BRIDGE_USB_DESCRIPTORS_H

#include <cstdint>

// Feature Report payload size (Report Count 0x3F in the vendor Feature items).
constexpr uint16_t BUTTON_REPORT_SIZE = 63;

// Emitted as the Usage/Logical Maximum of desc_hid_report_kbd and
// desc_hid_report_consumer, and enforced on stored shortcuts, so a shortcut can
// never ask for a usage the report descriptor does not declare.
constexpr uint8_t SHORTCUT_KEY_USAGE_MAX = 0x73; // Keyboard F24
constexpr uint16_t SHORTCUT_CONSUMER_USAGE_MAX = 0x02FF;

#endif //DS5_BRIDGE_USB_DESCRIPTORS_H
