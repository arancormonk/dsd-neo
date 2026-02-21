# UDP Retune Control (RTL/RTL-TCP)

DSD-neo includes a small UDP listener that can accept external retune commands when using RTL-based inputs.

This is useful for integrating DSD-neo into setups where another process (or another machine) decides when to retune,
but you still want DSD-neo to own the RTL demod pipeline.

## Enable

Start `dsd-neo` with RTL or RTL-TCP input and add `--rtl-udp-control <port>`:

```bash
dsd-neo -i rtl:0:851.375M:22:-2:24:0:2 --rtl-udp-control 9911 -N
```

Notes:

- The listener binds to `0.0.0.0:<port>` (all interfaces).
- There is no authentication. Use a firewall or run only on trusted networks.

## Message Format

Each UDP datagram is **exactly 5 bytes**:

- Byte 0: command (`0x00` = retune)
- Bytes 1..4: unsigned 32-bit little-endian frequency in **Hz**

Example: tune to 851.375 MHz

- Frequency Hz: `851375000`
- Payload bytes: `00 18 2A C4 32` (little-endian `0x32C42A18`)

## Send A Retune Command

Python example (local machine):

```bash
python3 - <<'PY'
import socket, struct
port = 9911
freq_hz = 851_375_000
msg = b"\x00" + struct.pack("<I", freq_hz)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(msg, ("127.0.0.1", port))
PY
```

Remote example (send to another host running DSD-neo):

- Replace `127.0.0.1` with that host's IP address.
- Ensure the UDP port is reachable (firewall/NAT).

