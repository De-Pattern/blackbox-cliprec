"""Clip take recording (src/cliprec.c, src/cliprec_thunk.S): the hooks into the stock firmware. Apply with `cave`."""
import os

from thumb import ROOT, bl, symbols, word

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


def entry(table, event_type, stock, name):
    at = table + 4 * (event_type - 2)
    return (at, word(stock | 1), word(sym[name] | 1))


PAD_A, PAD_B = 0x0805971C, 0x08068018    # the two pad engines' event jump tables

PATCHES = [
    # app event pump: timed-event read -> write open takes once recording stops
    hook(0x080A2936, 0x0804C754, "cliprec_pop"),
    # app event pump: live recorder -> Toggle-pad presses become takes
    hook(0x080A2946, 0x080A02C0, "cliprec_record"),
    # pad engines: sequencer note on / off skipped for pads with a take open
    entry(PAD_A, 0x11, 0x0805A734, "cliprec_a_on"),
    entry(PAD_A, 0x12, 0x0805A6FE, "cliprec_a_off"),
    entry(PAD_B, 0x11, 0x080686D2, "cliprec_b_on"),
    entry(PAD_B, 0x12, 0x080682B2, "cliprec_b_off"),
]
