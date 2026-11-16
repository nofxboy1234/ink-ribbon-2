#!/usr/bin/env python3
"""Drop X keyboard focus, the way alt+tab does, so the editor sees UNFOCUSED.

Used by tools/verify.sh. ImGui answers a focus loss by clearing its mouse state
and setting the pointer position to -FLT_MAX, which is the state that froze the
editor during a wall chain.
"""
import ctypes
import sys

x11 = ctypes.CDLL("libX11.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
dpy = x11.XOpenDisplay(None)
if not dpy:
    print("cannot open display", file=sys.stderr)
    sys.exit(1)
x11.XSetInputFocus(ctypes.c_void_p(dpy), ctypes.c_ulong(0), 0, 0)
x11.XFlush(ctypes.c_void_p(dpy))
print("focus dropped")
