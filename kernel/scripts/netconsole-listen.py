#!/usr/bin/env python3
# Log netconsole UDP packets from the Cedarview laptop to ~/netconsole.log, timestamped.
# Runs on cachix: setsid nohup python3 netconsole-listen.py &
import socket, time, pathlib
log = pathlib.Path.home() / "netconsole.log"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 6666))
with log.open("a", buffering=1) as f:
    while True:
        data, addr = s.recvfrom(65535)
        f.write(time.strftime("%H:%M:%S ") + data.decode("utf-8", "replace"))
