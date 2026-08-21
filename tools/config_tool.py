#!/usr/bin/env python3
"""
Read and modify the ds5dongle configuration over USB HID, without reflashing.

Protocol (see src/cmd.cpp / src/config.h):
  GET feature report 0xF7 -> raw Config_body bytes
  GET feature report 0xF8 -> firmware version string
  GET/SET feature report 0xFA -> Button settings area (currently 28 bytes)
  SET feature report 0xF6:
      funcid 0x01 + body   -> update config in RAM (firmware clamps invalid values)
      funcid 0x02          -> persist config to flash
      funcid 0x03          -> reconnect the USB device

Config_body is a packed struct; this tool derives the binary layout from FIELDS.

Requires: pip install hidapi

Examples:
  python config_tool.py get
  python config_tool.py set speaker_volume=90 enable_wake=1
  python config_tool.py set haptics_gain=1.5 --no-save
  python config_tool.py remap
  python config_tool.py remap square=cross l1=disable
  python config_tool.py remap square=nomap  # clear (restore identity mapping)
  python config_tool.py fields
"""
import argparse
import struct
import sys
import time


def _load_hid():
    try:
        import hid
    except ImportError:
        sys.exit("Missing dependency. Install with:  pip install hidapi")
    return hid


VID = 0x054C
PIDS = (0x0CE6, 0x0DF2)  # DualSense, DualSense Edge
HID_USAGE_PAGE_GENERIC_DESKTOP = 0x01
HID_USAGE_GAMEPAD = 0x05

REPORT_SET = 0xF6        # SET_REPORT: write/save config
REPORT_GET_CONFIG = 0xF7  # GET_REPORT: read Config_body
REPORT_GET_VERSION = 0xF8  # GET_REPORT: firmware version string
REPORT_BUTTON = 0xFA      # GET/SET_REPORT: Button settings area

FUNC_UPDATE = 0x01       # update config in RAM
FUNC_SAVE = 0x02         # persist to flash
FUNC_RECONNECT = 0x03    # reconnect tinyusb device

SET_DATA_LEN = 63        # data bytes after the report id (descriptor report count 0x3F)
FEATURE_REPORT_LEN = SET_DATA_LEN + 1  # report id + descriptor report count

# On macOS, back-to-back IOHID feature reports can return before TinyUSB has
# dispatched the preceding SET_REPORT callback.  Besides making a following
# GET_REPORT return stale data, this can also make the separate save command
# overtake the in-RAM update.  Keep the protocol ordered with a small settling
# interval and an explicit read barrier between update and save.
HID_SET_REPORT_DELAY = 0.05

CONFIG_VERSION = 5       # src/config.cpp CONFIG_VERSION (display only)

BUTTON_NAMES = (
    "DPadNorth",
    "DPadNorthEast",
    "DPadEast",
    "DPadSouthEast",
    "DPadSouth",
    "DPadSouthWest",
    "DPadWest",
    "DPadNorthWest",
    "Square",
    "Cross",
    "Circle",
    "Triangle",
    "L1",
    "R1",
    "L2",
    "R2",
    "Create",
    "Options",
    "L3",
    "R3",
    "Home",
    "Pad",
    "Mute",
    "LeftFunction",
    "RightFunction",
    "LeftPaddle",
    "RightPaddle",
    "Disable",
)
BUTTON_COUNT = len(BUTTON_NAMES)


def normalize_button_name(name):
    return "".join(c for c in name.lower() if c.isalnum())


BUTTON_NAME_TO_ID = {
    normalize_button_name(name): index
    for index, name in enumerate(BUTTON_NAMES)
}
BUTTON_NAME_TO_ID.update({
    "up": BUTTON_NAME_TO_ID["dpadnorth"],
    "north": BUTTON_NAME_TO_ID["dpadnorth"],
    "upright": BUTTON_NAME_TO_ID["dpadnortheast"],
    "northeast": BUTTON_NAME_TO_ID["dpadnortheast"],
    "ne": BUTTON_NAME_TO_ID["dpadnortheast"],
    "right": BUTTON_NAME_TO_ID["dpadeast"],
    "east": BUTTON_NAME_TO_ID["dpadeast"],
    "downright": BUTTON_NAME_TO_ID["dpadsoutheast"],
    "southeast": BUTTON_NAME_TO_ID["dpadsoutheast"],
    "se": BUTTON_NAME_TO_ID["dpadsoutheast"],
    "down": BUTTON_NAME_TO_ID["dpadsouth"],
    "south": BUTTON_NAME_TO_ID["dpadsouth"],
    "downleft": BUTTON_NAME_TO_ID["dpadsouthwest"],
    "southwest": BUTTON_NAME_TO_ID["dpadsouthwest"],
    "sw": BUTTON_NAME_TO_ID["dpadsouthwest"],
    "left": BUTTON_NAME_TO_ID["dpadwest"],
    "west": BUTTON_NAME_TO_ID["dpadwest"],
    "upleft": BUTTON_NAME_TO_ID["dpadnorthwest"],
    "northwest": BUTTON_NAME_TO_ID["dpadnorthwest"],
    "nw": BUTTON_NAME_TO_ID["dpadnorthwest"],
    "ps": BUTTON_NAME_TO_ID["home"],
    "touchpad": BUTTON_NAME_TO_ID["pad"],
    "off": BUTTON_NAME_TO_ID["disable"],
})

# These are CLI-only conveniences, not ButtonId enum members. Clearing a remap
# now writes the source button's own ID (an identity mapping) to the table.
CLEAR_REMAP_NAMES = {"nomap", "none", "default", "self"}

# struct.pack/unpack codes per field kind.
KIND_TO_CODE = {"u8": "B", "float": "f"}

# FIELDS is the single source of truth for the packed Config_body layout
# (src/config.h). To add/remove/reorder a field, edit ONLY this table -- the
# binary format (STRUCT_FMT) is derived from the 'kind' column below.
# name, kind, validator(value)->bool, help. Order MUST match Config_body.
FIELDS = [
    ("config_version",     "u8",    lambda v: True,              "config schema version (read-only, managed by firmware)"),
    ("haptics_gain",       "float", lambda v: 1.0 <= v <= 2.0,   "[1.0, 2.0]"),
    ("speaker_volume",     "u8",    lambda v: 0 <= v <= 127,     "[0, 127]"),
    ("headset_volume",     "u8",    lambda v: 0 <= v <= 127,     "[0, 127]"),
    ("speaker_gain",       "u8",    lambda v: 0 <= v <= 7,       "[0, 7]"),
    ("inactive_time",      "u8",    lambda v: 0 <= v <= 60,      "[0, 60] minutes (0 disable)"),
    ("disable_pico_led",   "u8",    lambda v: v in (0, 1),       "0/1"),
    ("polling_rate_mode",  "u8",    lambda v: v in (0, 1, 2),    "0:250Hz 1:500Hz 2:real-time"),
    ("audio_buffer_length","u8",    lambda v: 16 <= v <= 128,    "[16, 128]"),
    ("controller_mode",    "u8",    lambda v: v in (0, 1, 2),    "0:DS5 1:DSE 2:Auto"),
    ("enable_usb_sn",      "u8",    lambda v: v in (0, 1),       "0/1 (USB serial number)"),
    ("ps_shortcut_enabled","u8",    lambda v: v in (0, 1),       "0/1 (Xbox Game Bar via HID keyboard)"),
    ("mic_select",         "u8",    lambda v: v in (0, 1, 2, 3), "0:auto 1:builtin 2:headphone 3:disable"),
    ("speaker_select",     "u8",    lambda v: v in (0, 1, 2, 3), "0:auto 1:builtin 2:headphone 3:disable"),
    ("enable_wake",        "u8",    lambda v: v in (0, 1),       "0/1 (wake host on PS press)"),
    ("trigger_reduce",     "u8",    lambda v: 0 <= v <= 10,      "[0, 10] (0: auto)"),
    ("lock_volume",        "u8",    lambda v: v in (0, 1),       "0/1 (ignore the volume change from SetStateData(game or software))"),
    ("status_gpio_pin",    "u8",    lambda v: 0 <= v <= 255,     "GPIO number (255 disables; firmware rejects board-reserved pins)"),
    ("status_gpio_mode",   "u8",    lambda v: v in (0, 1),       "0:pull high 1:200ms button pulse"),
]
FIELD_NAMES = [f[0] for f in FIELDS]
# Little-endian, no padding -- matches __attribute__((packed)) Config_body.
STRUCT_FMT = "<" + "".join(KIND_TO_CODE[f[1]] for f in FIELDS)
BODY_SIZE = struct.calcsize(STRUCT_FMT)
if BODY_SIZE > SET_DATA_LEN - 1:
    raise RuntimeError(
        f"Config_body is {BODY_SIZE} bytes, but the update report only has "
        f"{SET_DATA_LEN - 1} bytes available."
    )


def unpack_config(body):
    unpacked = iter(struct.unpack(STRUCT_FMT, body))
    cfg = {}
    for name, kind, _validator, _helptext in FIELDS:
        cfg[name] = next(unpacked)
    return cfg


def pack_config(cfg):
    values = []
    for name, kind, validator, _helptext in FIELDS:
        value = cfg[name]
        if not validator(value):
            raise ValueError(f"Invalid value for {name}: {value!r}")
        values.append(value)
    return struct.pack(STRUCT_FMT, *values)


def is_gamepad_hid(devinfo):
    return (devinfo.get("usage_page") == HID_USAGE_PAGE_GENERIC_DESKTOP and
            devinfo.get("usage") == HID_USAGE_GAMEPAD)


def fmt_hex(value):
    if value is None:
        return "?"
    return f"0x{int(value):04X}"


def describe_hid(devinfo):
    return (
        f"pid={fmt_hex(devinfo.get('product_id'))}, "
        f"interface={devinfo.get('interface_number', '?')}, "
        f"usage_page={fmt_hex(devinfo.get('usage_page'))}, "
        f"usage={fmt_hex(devinfo.get('usage'))}, "
        f"product={devinfo.get('product_string') or '?'}"
    )


def open_device():
    hid = _load_hid()
    cand = [d for d in hid.enumerate(VID) if d["product_id"] in PIDS]
    if not cand:
        sys.exit("No DualSense / ds5dongle found (VID 054C, PID 0CE6/0DF2). "
                 "Close Steam/DSX if they're holding the device.")
    gamepads = [d for d in cand if is_gamepad_hid(d)]
    if not gamepads:
        detail = "\n".join(f"  {describe_hid(d)}" for d in cand)
        sys.exit("Found DualSense / ds5dongle HID device(s), but none were the Game Pad interface "
                 "(usage_page=0x0001, usage=0x0005). Wake adds a keyboard HID; "
                 "this tool only opens the gamepad.\n" + detail)
    dev = hid.device()
    dev.open_path(gamepads[0]["path"])
    return dev


def read_config(dev):
    # Windows hidapi expects the buffer to match the HID feature report length.
    # The config body is shorter than the descriptor report count, so read the
    # full report and unpack only Config_body.
    try:
        data = dev.get_feature_report(REPORT_GET_CONFIG, FEATURE_REPORT_LEN)
    except OSError as exc:
        sys.exit(f"Failed reading config report 0x{REPORT_GET_CONFIG:02X}: {exc}")
    if not data:
        sys.exit("Empty response reading config (report 0xF7). Is the firmware current?")
    body = bytes(data[1:1 + BODY_SIZE]) if data[0] == REPORT_GET_CONFIG else bytes(data[:BODY_SIZE])
    if len(body) < BODY_SIZE:
        sys.exit(f"Short config read: got {len(body)} bytes, expected {BODY_SIZE}.")
    return unpack_config(body)

def read_version(dev):
    try:
        data = dev.get_feature_report(REPORT_GET_VERSION, FEATURE_REPORT_LEN)
    except OSError:
        return ""
    raw = bytes(data[1:]) if data and data[0] == REPORT_GET_VERSION else bytes(data or b"")
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace").strip()


def read_button(dev):
    try:
        data = dev.get_feature_report(REPORT_BUTTON, BUTTON_COUNT + 1)
    except OSError as exc:
        sys.exit(f"Failed reading Button report 0x{REPORT_BUTTON:02X}: {exc}")
    if not data:
        sys.exit("Empty response reading Button settings (report 0xFA). Is the firmware current?")
    raw = bytes(data[1:]) if data[0] == REPORT_BUTTON else bytes(data)
    if len(raw) < BUTTON_COUNT:
        sys.exit(f"Short Button settings read: got {len(raw)} bytes, expected {BUTTON_COUNT}.")
    return bytearray(raw[:BUTTON_COUNT])

def send_feature_report(dev, data, operation, report_id=REPORT_SET):
    report = bytes([report_id]) + data
    try:
        sent = dev.send_feature_report(report)
    except OSError as exc:
        sys.exit(f"Failed {operation}: {exc}")
    if sent is not None and sent != len(report):
        sys.exit(
            f"Failed {operation}: wrote {sent} of {len(report)} report bytes."
        )
    time.sleep(HID_SET_REPORT_DELAY)


def write_config(dev, cfg, save):
    body = pack_config(cfg)
    # [report id][funcid 0x01][body...] padded to SET_DATA_LEN data bytes.
    data = bytes([FUNC_UPDATE]) + body
    data = data[:SET_DATA_LEN].ljust(SET_DATA_LEN, b"\x00")
    send_feature_report(dev, data, "updating config")

    # This read is also a USB control-transfer barrier: do not submit FUNC_SAVE
    # until the firmware has applied FUNC_UPDATE.  It is the authoritative
    # value to display because firmware validation may clamp some fields.
    new_cfg = read_config(dev)

    if save:
        save_data = bytes([FUNC_SAVE]).ljust(SET_DATA_LEN, b"\x00")
        send_feature_report(dev, save_data, "saving config to flash")
        # Confirm that the device still answers after the flash operation.
        new_cfg = read_config(dev)
    return new_cfg


def write_button(dev, button, save):
    send_feature_report(
        dev, bytes(button), "updating Button settings", report_id=REPORT_BUTTON
    )
    new_button = read_button(dev)
    if save:
        save_data = bytes([FUNC_SAVE]).ljust(SET_DATA_LEN, b"\x00")
        send_feature_report(dev, save_data, "saving Button settings to flash")
        new_button = read_button(dev)
    return new_button


def fmt_value(name, value):
    if name == "haptics_gain":
        return f"{value:.3f}"
    return str(value)


def print_config(cfg):
    width = max(len(n) for n in FIELD_NAMES)
    for name, _kind, _ok, helptext in FIELDS:
        print(f"  {name:<{width}} = {fmt_value(name, cfg[name]):<8}  # {helptext}")


def parse_assignment(token):
    if "=" not in token:
        sys.exit(f"Bad assignment '{token}', expected name=value.")
    name, raw = token.split("=", 1)
    name = name.strip()
    if name not in FIELD_NAMES:
        sys.exit(f"Unknown field '{name}'. Run 'config_tool.py fields' to list them.")
    if name == "config_version":
        sys.exit("config_version is managed by the firmware and cannot be set.")
    kind = dict((f[0], f[1]) for f in FIELDS)[name]
    validator = dict((f[0], f[2]) for f in FIELDS)[name]
    try:
        value = float(raw) if kind == "float" else int(raw, 0)
    except ValueError:
        sys.exit(f"Bad value '{raw}' for {name}.")
    if not validator(value):
        helptext = dict((f[0], f[3]) for f in FIELDS)[name]
        sys.exit(f"Value {raw} out of range for {name} (expected {helptext}).")
    return name, value


def cmd_fields(_args):
    width = max(len(n) for n in FIELD_NAMES)
    print(f"Config_body ({BODY_SIZE} bytes, schema version {CONFIG_VERSION}):")
    for name, kind, _ok, helptext in FIELDS:
        ro = " (read-only)" if name == "config_version" else ""
        print(f"  {name:<{width}} {kind:<6} {helptext}{ro}")


def cmd_get(_args):
    dev = open_device()
    try:
        version = read_version(dev)
        cfg = read_config(dev)
        remap = read_button(dev)
    finally:
        dev.close()
    if version:
        print(f"Firmware: {version}")
    print("Config:")
    print_config(cfg)
    print("Button remaps:")
    print_remaps(remap)


def cmd_set(args):
    updates = dict(parse_assignment(t) for t in args.assignments)
    if not updates:
        sys.exit("Nothing to set. Pass one or more name=value pairs.")
    dev = open_device()
    try:
        cfg = read_config(dev)
        cfg.update(updates)
        new_cfg = write_config(dev, cfg, save=not args.no_save)
    finally:
        dev.close()
    print("Updated:" + ("" if args.no_save else " (saved to flash)"))
    for name in updates:
        print(f"  {name} -> {fmt_value(name, new_cfg[name])}")
    # Firmware clamps invalid values; surface any that were adjusted.
    for name, want in updates.items():
        got = new_cfg[name]
        adjusted = abs(got - want) > 1e-6 if isinstance(want, float) else got != want
        if adjusted:
            print(f"  note: {name} was clamped by firmware to {fmt_value(name, got)}")


def parse_button(raw, *, source):
    key = normalize_button_name(raw)
    if key not in BUTTON_NAME_TO_ID:
        valid_names = BUTTON_NAMES[:-1] if source else BUTTON_NAMES + ("NoMap",)
        valid = ", ".join(valid_names)
        sys.exit(f"Unknown button '{raw}'. Valid buttons: {valid}.")
    button_id = BUTTON_NAME_TO_ID[key]
    if source and button_id == BUTTON_COUNT - 1:
        sys.exit(f"'{raw}' cannot be used as a source button.")
    return button_id


def button_name(button_id):
    return BUTTON_NAMES[button_id] if 0 <= button_id < BUTTON_COUNT else f"Invalid({button_id})"


def parse_remap_assignment(token):
    if "=" not in token:
        sys.exit(f"Bad remap '{token}', expected source=target.")
    source_raw, target_raw = token.split("=", 1)
    source_id = parse_button(source_raw.strip(), source=True)
    target_key = normalize_button_name(target_raw.strip())
    target_id = (source_id if target_key in CLEAR_REMAP_NAMES else
                 parse_button(target_raw.strip(), source=False))
    return source_id, target_id


def print_remaps(remap, indent="  "):
    for source in range(BUTTON_COUNT - 1):
        target = remap[source]
        print(f"{indent}{BUTTON_NAMES[source]:<15} -> {button_name(target)}")


def cmd_remap(args):
    updates = dict(parse_remap_assignment(token) for token in args.assignments)
    dev = open_device()
    try:
        remap = read_button(dev)
        if not updates:
            print("Button remaps:")
            print_remaps(remap)
            return
        for source, target in updates.items():
            remap[source] = target
        new_remap = write_button(dev, remap, save=not args.no_save)
    finally:
        dev.close()

    for source in updates:
        target = new_remap[source]
        if target != updates[source]:
            sys.exit(
                f"Read-back verification failed for {BUTTON_NAMES[source]}: "
                f"requested {button_name(updates[source])}, got {button_name(target)}."
            )

    print("Updated button remaps:" + ("" if args.no_save else " (saved to flash)"))
    for source in updates:
        target = new_remap[source]
        print(f"  {BUTTON_NAMES[source]:<15} -> {button_name(target)}")


def main():
    parser = argparse.ArgumentParser(description="Read and modify ds5dongle config over USB HID.")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("get", help="read and print the current config").set_defaults(func=cmd_get)
    sub.add_parser("fields", help="list configurable fields and ranges").set_defaults(func=cmd_fields)

    p_set = sub.add_parser("set", help="set one or more fields (name=value ...)")
    p_set.add_argument("assignments", nargs="+", metavar="name=value")
    p_set.add_argument("--no-save", action="store_true",
                       help="update RAM only; do not persist to flash")
    p_set.set_defaults(func=cmd_set)

    p_remap = sub.add_parser(
        "remap",
        help="view or set button remaps (source=target; nomap restores identity, disable blocks)",
    )
    p_remap.add_argument("assignments", nargs="*", metavar="source=target")
    p_remap.add_argument("--no-save", action="store_true",
                         help="update RAM only; do not persist to flash")
    p_remap.set_defaults(func=cmd_remap)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
