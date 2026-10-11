#!/usr/bin/env python3
"""Scripted serial console for the TilkieTalkie device (ESP32-S3 USB-Serial/JTAG).
Usage: ttserial.py --port P [--baud 115200] [--reset] [--timeout S] [--idle S] [--cmd "text" --wait S]... [--log file] [--quiet-boot]
Sends each --cmd after its matching --wait (seconds after start), prints timestamped lines, exits after --timeout seconds.
Survives USB re-enumeration (reopens the port). Always closes the port on exit."""
import argparse, sys, time, re, serial

ap = argparse.ArgumentParser()
ap.add_argument('--port', required=True)
ap.add_argument('--baud', type=int, default=115200)
ap.add_argument('--reset', action='store_true')
ap.add_argument('--timeout', type=float, default=20.0)
ap.add_argument('--idle', type=float, default=0)
ap.add_argument('--cmd', action='append', default=[])
ap.add_argument('--wait', action='append', type=float, default=[])
ap.add_argument('--log', default=None)
ap.add_argument('--quiet-boot', action='store_true', help='hide bootloader D/V lines')
a = ap.parse_args()

waits = a.wait + [2.0] * (len(a.cmd) - len(a.wait))
log = open(a.log, 'a') if a.log else None
BOOTNOISE = re.compile(r'^\s*[DV] \(\d+\) ')
def out(s, raw=False):
    if a.quiet_boot and not raw and BOOTNOISE.match(s.split('] ',1)[-1] if '] ' in s else s): 
        if log: log.write(s); log.flush()
        return
    sys.stdout.write(s); sys.stdout.flush()
    if log: log.write(s); log.flush()

def open_port():
    s = serial.Serial(a.port, a.baud, timeout=0.05, rtscts=False, dsrdtr=False, exclusive=True)
    return s

ser = open_port()
t0 = time.time()
try:
    if a.reset:
        ser.dtr = False; ser.rts = True; time.sleep(0.1); ser.rts = False
        try: ser.reset_input_buffer()
        except Exception: pass
    last_rx = t0
    cmds = list(zip(waits, a.cmd)); next_cmd_at = t0 + (cmds[0][0] if cmds else 1e9)
    buf = b''
    while True:
        now = time.time()
        if now - t0 > a.timeout: break
        if a.idle and now - last_rx > a.idle and not cmds: break
        if cmds and now >= next_cmd_at:
            w, c = cmds.pop(0)
            out(f"\n>>> [{now-t0:7.2f}s] SEND: {c}\n", raw=True)
            try:
                ser.write((c + '\n').encode()); ser.flush()
            except Exception as e:
                out(f"!!! write failed: {e}\n", raw=True)
            next_cmd_at = now + (cmds[0][0] if cmds else 1e9)
        try:
            data = ser.read(4096)
        except serial.SerialException as e:
            out(f"!!! [{time.time()-t0:7.2f}s] port dropped ({e.__class__.__name__}); reopening...\n", raw=True)
            try: ser.close()
            except Exception: pass
            deadline = time.time() + 15
            while time.time() < deadline:
                try:
                    ser = open_port(); out(f"!!! [{time.time()-t0:7.2f}s] port reopened\n", raw=True); break
                except Exception:
                    time.sleep(0.25)
            else:
                out("!!! could not reopen port\n", raw=True); break
            continue
        if data:
            last_rx = time.time(); buf += data
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                out(f"[{time.time()-t0:7.2f}s] {line.decode('utf-8','replace').rstrip()}\n")
    if buf: out(f"[{time.time()-t0:7.2f}s] {buf.decode('utf-8','replace')}\n")
finally:
    try: ser.close()
    except Exception: pass
    if log: log.close()
