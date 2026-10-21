#!/usr/bin/env python3
# Log netconsole UDP packets from the test laptop, timestamped. Runs on the host that
# test-session.sh sends the log to (by default the build host):
#   setsid nohup python3 netconsole-listen.py &
# SGX_NETCONSOLE_PORT picks the UDP port (6666), SGX_NETCONSOLE_LOG the log file
# (~/netconsole.log).
import os, socket, time, pathlib
port = int(os.environ.get("SGX_NETCONSOLE_PORT", "6666"))
log = pathlib.Path(os.environ.get("SGX_NETCONSOLE_LOG",
                                  pathlib.Path.home() / "netconsole.log"))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
with log.open("a", buffering=1) as f:
    while True:
        data, addr = s.recvfrom(65535)
        f.write(time.strftime("%H:%M:%S ") + data.decode("utf-8", "replace"))
