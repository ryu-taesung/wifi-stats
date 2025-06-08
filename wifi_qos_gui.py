#!/usr/bin/env python3
import os
import socket
import struct
import sys
import tkinter as tk

# --- PARAMETERS ---
# Match your Node version's default:
SOCK_PATH = os.environ.get(
    "QOS_SOCK",
    f"/run/user/{os.getuid()}/wifi_qos.sock"
)

POLL_INTERVAL_MS = 1000   # how often to check for new socket data
# ------------------

# If a stale socket file exists, remove it:
if os.path.exists(SOCK_PATH):
    try:
        os.unlink(SOCK_PATH)
    except OSError:
        print(f"Could not unlink {SOCK_PATH}, exiting.", file=sys.stderr)
        sys.exit(1)

# Create an AF_UNIX / SOCK_DGRAM socket, bind to path, set non-blocking
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
try:
    sock.bind(SOCK_PATH)
except OSError as e:
    print(f"Failed to bind to {SOCK_PATH}: {e}", file=sys.stderr)
    sys.exit(1)

# Make it non-blocking so recv() won't hang
sock.setblocking(False)


# Build a minimal tkinter window
root = tk.Tk()
root.title("WiFi QoS Monitor")
# If you want to force a small size (e.g. to fit an 8" touchscreen),
# you can uncomment below, or let it auto-size to the Label's content.
# root.geometry("400x200")

# A single Label with large, easy-to-read font:
font_spec = ("Helvetica", 24, "bold")
label = tk.Label(
    root,
    text="Waiting for data...",
    font=font_spec,
    justify="center",
    padx=20,
    pady=20
)
label.pack(expand=True, fill="both")


def poll_socket():
    """
    Called periodically (every POLL_INTERVAL_MS). Tries to recv a single
    datagram (24 bytes). If successful and length == 24, unpack it:
      - tsNs: 8 bytes, little-endian unsigned long long
      - rssi: 4 bytes, little-endian signed int
      - ok:   4 bytes, little-endian unsigned int
      - retry:4 bytes, little-endian unsigned int
      - fail: 4 bytes, little-endian unsigned int
    Then recompute signal% and efficiency, and update the Label.
    """
    try:
        data, _addr = sock.recvfrom(1024)
    except BlockingIOError:
        # no data available right now
        pass
    else:
        if len(data) == 24:
            # Unpack exactly as Node did:
            ts_ns, rssi, ok, retry, fail = struct.unpack("<Q i I I I", data)
            total = ok + retry + fail
            eff = (ok / total) * 100 if total else 0.0

            # Convert rssi to signal percent:
            signal_pct = 2 * (rssi + 100)
            if signal_pct > 100:
                signal_pct = 100
            if signal_pct < 0:
                signal_pct = 0

            # (Optionally convert ts_ns to something human-readable. For now,
            # we'll just show raw nanoseconds or you could omit ts entirely.)
            # Convert nanoseconds since epoch to seconds with decimals:
            ts_s = ts_ns / 1e9

            label_text = (
                f"Time: {ts_s:.3f} s\n"
                f"RSSI: {rssi} dBm   ({signal_pct:.0f} %)\n"
                f"OK: {ok}   Retry: {retry}   Fail: {fail}\n"
                f"Eff: {eff:0.2f} %"
            )
            label.config(text=label_text)
        else:
            # Unexpected length; just ignore it
            pass

    # Schedule the next poll
    root.after(POLL_INTERVAL_MS, poll_socket)


# Kick off the periodic polling
root.after(POLL_INTERVAL_MS, poll_socket)

# When the user closes the window, clean up the socket path:
def on_close():
    try:
        sock.close()
    except:
        pass
    try:
        os.unlink(SOCK_PATH)
    except:
        pass
    root.destroy()

root.protocol("WM_DELETE_WINDOW", on_close)

# Start the tkinter mainloop
root.mainloop()

