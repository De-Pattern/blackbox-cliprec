"""The compiled code cave (./build.sh), appended after the stock image, plus the version shown on the Tools screen."""
import os

from thumb import ROOT

STOCK_END = 0x080F1E78
VERSION = b"4.1.0"

PATCHES = [
    (0x080CF290, b"3.1.9\0", VERSION.ljust(6, b"\0")),     # Tools screen / splash (display only, same 6-byte slot)
    (STOCK_END, b"", open(os.path.join(ROOT, "out", "cave.bin"), "rb").read()),
]
