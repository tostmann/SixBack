#!/usr/bin/env python3
"""DTR/RTS-Setzer fuer den Node-Harness.

Warum es das gibt: node-serialport ruft in seinem set() unbedingt TIOCSBRK bzw.
TIOCCBRK, bevor es TIOCMSET macht (bindings-cpp/src/serialport_unix.cpp). Der
cdc_acm-Treiber kann break nicht -> EOPNOTSUPP -> die Funktion bricht ab, und
DTR/RTS werden nie gesetzt. Ohne DTR/RTS gibt es keine Reset-Sequenz und damit
keinen Download-Modus.

Dieser Helfer macht nur die beiden ioctls, ohne termios anzufassen (also ohne
Nodes Baudrate/Modus zu stoeren). Er haelt den fd offen und liest Kommandos
zeilenweise von stdin:  "<dtr> <rts>" mit je 0/1, Antwort "ok".
"""
import fcntl
import os
import struct
import sys
import termios

path = sys.argv[1]
# O_NONBLOCK: nicht auf DCD warten. Kein termios-Setzen -> Nodes Portzustand bleibt.
fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)


def bits():
    return struct.unpack("I", fcntl.ioctl(fd, termios.TIOCMGET, struct.pack("I", 0)))[0]


sys.stdout.write("ready\n")
sys.stdout.flush()

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    if line == "quit":
        break
    try:
        dtr, rts = (v == "1" for v in line.split())
        b = bits()
        b = (b | termios.TIOCM_DTR) if dtr else (b & ~termios.TIOCM_DTR)
        b = (b | termios.TIOCM_RTS) if rts else (b & ~termios.TIOCM_RTS)
        fcntl.ioctl(fd, termios.TIOCMSET, struct.pack("I", b))
        sys.stdout.write("ok\n")
    except Exception as exc:  # noqa: BLE001 - Fehler gehoert zum Aufrufer
        sys.stdout.write(f"err {exc}\n")
    sys.stdout.flush()

os.close(fd)
