#!/usr/bin/env python3
"""List JACK port names as PipeWire's JACK layer exposes them (what chain.json "connect" needs).

    jack-ports.py [FILTER]      e.g. jack-ports.py scarlett

These are "<node.description>:<port>", not the "<node.name>:<port>" form pw-link prints.
"""
import ctypes as C
import sys

j = C.CDLL("libjack.so.0")
j.jack_client_open.restype = C.c_void_p
j.jack_client_open.argtypes = [C.c_char_p, C.c_int, C.c_void_p]
j.jack_get_ports.restype = C.POINTER(C.c_char_p)
j.jack_get_ports.argtypes = [C.c_void_p, C.c_char_p, C.c_char_p, C.c_ulong]
j.jack_port_by_name.restype = C.c_void_p
j.jack_port_by_name.argtypes = [C.c_void_p, C.c_char_p]
j.jack_port_flags.argtypes = [C.c_void_p]
j.jack_client_close.argtypes = [C.c_void_p]

JACK_NO_START_SERVER, IS_INPUT = 1, 0x1

client = j.jack_client_open(b"jack-ports", JACK_NO_START_SERVER, None)
if not client:
    sys.exit("cannot connect to JACK/PipeWire")

needle = sys.argv[1].lower() if len(sys.argv) > 1 else ""
ports = j.jack_get_ports(client, None, None, 0)
i = 0
while ports and ports[i]:
    name = ports[i].decode()
    if needle in name.lower():
        flags = j.jack_port_flags(j.jack_port_by_name(client, ports[i]))
        print(("in   " if flags & IS_INPUT else "out  ") + name)
    i += 1
j.jack_client_close(client)
