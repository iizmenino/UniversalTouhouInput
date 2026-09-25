"""Minimal Windows key injection tester for UniversalTouhouInput.

Usage examples:
    python test_sendinput.py vk Z
    python test_sendinput.py scancode Z
    python test_sendinput.py vk SPACE --hold-ms 100
    python test_sendinput.py unicode z

This is only a diagnostic helper. The important distinction is that the
scancode and virtual-key modes correspond to ordinary keyboard injection,
while unicode mode is VK_PACKET and may not be useful to a gameplay input
reader.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)

INPUT_KEYBOARD = 1
KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
KEYEVENTF_SCANCODE = 0x0008

ULONG_PTR = wintypes.WPARAM


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("type", wintypes.DWORD),
        ("u", INPUTUNION),
    ]


user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SendInput.restype = wintypes.UINT
user32.MapVirtualKeyW.argtypes = (wintypes.UINT, wintypes.UINT)
user32.MapVirtualKeyW.restype = wintypes.UINT
user32.VkKeyScanW.argtypes = (wintypes.WCHAR,)
user32.VkKeyScanW.restype = wintypes.SHORT

VK_TOOLS = {
    "SPACE": 0x20,
    "ENTER": 0x0D,
    "ESC": 0x1B,
    "TAB": 0x09,
    "SHIFT": 0x10,
    "CTRL": 0x11,
    "ALT": 0x12,
    "LEFT": 0x25,
    "UP": 0x26,
    "RIGHT": 0x27,
    "DOWN": 0x28,
}


def parse_vk(value: str) -> int:
    value = value.upper()
    if value in VK_TOOLS:
        return VK_TOOLS[value]
    if len(value) == 1:
        code = user32.VkKeyScanW(value)
        if code == -1:
            raise ValueError(f"Cannot map {value!r} to a virtual key")
        return code & 0xFF
    if value.startswith("F") and value[1:].isdigit():
        n = int(value[1:])
        if 1 <= n <= 24:
            return 0x70 + n - 1
    if value.startswith("0X"):
        return int(value, 16)
    raise ValueError(f"Unknown key: {value}")


def send_vk(vk: int, hold_ms: int) -> None:
    scan = user32.MapVirtualKeyW(vk, 0)
    down = INPUT(INPUT_KEYBOARD, KEYBDINPUT(vk, scan, 0, 0, 0))
    up = INPUT(INPUT_KEYBOARD, KEYBDINPUT(vk, scan, KEYEVENTF_KEYUP, 0, 0))
    if user32.SendInput(1, ctypes.byref(down), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError(ctypes.get_last_error())
    time.sleep(hold_ms / 1000)
    if user32.SendInput(1, ctypes.byref(up), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError(ctypes.get_last_error())


def send_scancode(vk: int, hold_ms: int) -> None:
    scan = user32.MapVirtualKeyW(vk, 0)
    if not scan:
        raise ValueError(f"No scan code for VK 0x{vk:02X}")
    flags = KEYEVENTF_SCANCODE
    if scan & 0xE000:
        flags |= KEYEVENTF_EXTENDEDKEY
        scan &= 0xFF
    down = INPUT(INPUT_KEYBOARD, KEYBDINPUT(0, scan, flags, 0, 0))
    up = INPUT(INPUT_KEYBOARD, KEYBDINPUT(0, scan, flags | KEYEVENTF_KEYUP, 0, 0))
    if user32.SendInput(1, ctypes.byref(down), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError(ctypes.get_last_error())
    time.sleep(hold_ms / 1000)
    if user32.SendInput(1, ctypes.byref(up), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError(ctypes.get_last_error())


def send_unicode(ch: str, hold_ms: int) -> None:
    if len(ch) != 1:
        raise ValueError("Unicode mode expects exactly one character")
    code = ord(ch)
    down = INPUT(INPUT_KEYBOARD, KEYBDINPUT(0, code, KEYEVENTF_UNICODE, 0, 0))
    up = INPUT(INPUT_KEYBOARD, KEYBDINPUT(0, code, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, 0, 0))
    if user32.SendInput(1, ctypes.byref(down), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError(ctypes.get_last_error())
    time.sleep(hold_ms / 1000)
    if user32.SendInput(1, ctypes.byref(up), ctypes.sizeof(INPUT)) != 1:
        raise ctypes.WinError(ctypes.get_last_error())


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["vk", "scancode", "unicode"])
    parser.add_argument("key")
    parser.add_argument("--hold-ms", type=int, default=50)
    args = parser.parse_args()

    if args.mode == "unicode":
        send_unicode(args.key, args.hold_ms)
        return

    vk = parse_vk(args.key)
    if args.mode == "vk":
        send_vk(vk, args.hold_ms)
    else:
        send_scancode(vk, args.hold_ms)


if __name__ == "__main__":
    main()
