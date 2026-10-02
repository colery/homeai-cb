#!/usr/bin/env python3
"""
Claude Buddy hub.

Claude Code hooks (on any machine) POST small events to this hub; the hub keeps
per-session state, merges it, and pushes a heartbeat to the buddy over USB
serial (115200, newline-delimited JSON — same protocol the BLE firmware speaks).

    hooks --HTTP POST /event--> [hub] --USB serial--> CYD / M5StickC

Run it on the machine the buddy is plugged into:
    python3 bridge.py                      # auto-detect serial port, HTTP on :8765
    python3 bridge.py --port /dev/ttyUSB0 --http-port 8765

State is per session, so several concurrent Claude Code sessions (and several
machines) don't corrupt each other, and every piece of "in progress" state
expires, so an interrupted turn can never leave the display stuck on WORKING.
"""

import argparse
import glob
import json
import os
import sys
import threading
import time
from collections import deque
from datetime import date, datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

KEEPALIVE = 2.0      # device goes to SLEEP after 15 s of silence; beat well inside that
MIN_GAP = 0.12       # min seconds between pushes when state is changing quickly
THINK_TTL = 120      # turn with no tool in flight and no event for this long => idle
TOOL_TTL = 600       # a tool call with no PostToolUse after this long was lost
WAIT_TTL = 600       # a permission prompt nobody answered
SESSION_TTL = 3600   # forget sessions that have been silent this long
MAX_BODY = 16384


def now():
    return time.time()


class Hub:
    def __init__(self):
        self.lock = threading.Lock()
        self.sessions = {}
        self.entries = deque(maxlen=8)
        self.tokens_today = 0
        self.day = date.today()
        self.rev = 0                       # bumped on every accepted event
        self.calls = deque(maxlen=6000)    # timestamps of tool calls, for the activity chart
        self.resync = threading.Event()    # device just booted: resend the clock and a heartbeat
        self.device = {'connected': False, 'port': None, 'last_send': 0, 'last_rx': 0}

    # ── events from hooks ────────────────────────────────────────────────
    def event(self, ev):
        t = ev.get('t')
        sid = str(ev.get('session') or 'unknown')[:64]
        ts = now()
        with self.lock:
            if t == 'end':
                self.sessions.pop(sid, None)
                self.rev += 1
                return
            s = self.sessions.setdefault(sid, {
                'host': str(ev.get('host', ''))[:24], 'turn': False, 'tools': {},
                'prompt': None, 'last': ts, 'last_tool': None,
            })
            s['last'] = ts

            if t == 'prompt':
                s['turn'] = True
                s['prompt'] = None
                s['tools'].clear()
                self._entry(ts, '> prompt', s)
            elif t == 'pre':
                tool = str(ev.get('tool', '?'))[:20]
                hint = str(ev.get('hint', ''))[:43]
                s['turn'] = True
                s['prompt'] = None
                self.calls.append(ts)
                s['tools'][str(ev.get('id') or f'{tool}-{ts}')] = (tool, ts)
                s['last_tool'] = (tool, hint)
                self._entry(ts, f'{tool} {hint}'.strip(), s)
            elif t == 'post':
                s['tools'].pop(str(ev.get('id')), None)
                s['prompt'] = None
            elif t == 'notify':
                kind = ev.get('ntype', '')
                msg = str(ev.get('message', ''))
                if kind == 'permission_prompt' or (not kind and 'permission' in msg.lower()):
                    tool, hint = s['last_tool'] or ('Permission', msg[:43])
                    s['prompt'] = {'id': f'cc:{sid[-6:]}:{int(ts)}', 'tool': tool,
                                   'hint': hint, 'info': True, 'ts': ts}
                elif kind == 'idle_prompt':
                    # Claude is sitting at the input box: turn is over even if no
                    # Stop hook fired (e.g. the user interrupted with Esc).
                    s['turn'] = False
                    s['tools'].clear()
                    s['prompt'] = None
            elif t == 'stop':
                s['turn'] = False
                s['tools'].clear()
                s['prompt'] = None
                self._tokens(int(ev.get('tokens', 0) or 0))
            self.rev += 1

    def _entry(self, ts, text, s):
        tag = f"{s['host'][:6]} " if len(self._hosts()) > 1 else ''
        self.entries.appendleft(f"{datetime.fromtimestamp(ts):%H:%M} {tag}{text}"[:91])

    def _hosts(self):
        return {s['host'] for s in self.sessions.values()}

    def _tokens(self, delta):
        if date.today() != self.day:
            self.day = date.today()
            self.tokens_today = 0
        self.tokens_today += max(0, delta)

    # ── merged view ──────────────────────────────────────────────────────
    def _expire(self, ts):
        for sid in list(self.sessions):
            s = self.sessions[sid]
            for tid, (_, t0) in list(s['tools'].items()):
                if ts - t0 > TOOL_TTL:
                    del s['tools'][tid]
            if s['turn'] and not s['tools'] and ts - s['last'] > THINK_TTL:
                s['turn'] = False
            if s['prompt'] and ts - s['prompt']['ts'] > WAIT_TTL:
                s['prompt'] = None
            if ts - s['last'] > SESSION_TTL:
                del self.sessions[sid]

    def snapshot(self):
        ts = now()
        with self.lock:
            self._expire(ts)
            if date.today() != self.day:
                self._tokens(0)
            live = list(self.sessions.values())
            active = [s for s in live if s['turn']]
            waiting = [s for s in live if s['prompt']]
            hb = {
                'total': len(live),
                'running': len(active),
                'waiting': len(waiting),
                'msg': 'idle',
                'entries': list(self.entries),
                'tokens': 0,
                'tokens_today': self.tokens_today,
            }
            if active:
                recent = max(active, key=lambda s: s['last'])
                if recent['tools']:
                    tool, t0 = max(recent['tools'].values(), key=lambda v: v[1])
                    hb['msg'] = f'Tool: {tool}'
                else:
                    hb['msg'] = 'thinking...'
            if waiting:
                p = max(waiting, key=lambda s: s['prompt']['ts'])['prompt']
                hb['prompt'] = {k: p[k] for k in ('id', 'tool', 'hint', 'info')}
            hb['msg'] = hb['msg'][:47]
            # tool calls per minute over the last 30 minutes, oldest first
            spark = [0] * 30
            for t in self.calls:
                age = ts - t
                if 0 <= age < 1800:
                    spark[29 - int(age // 60)] += 1
            hb['spark'] = [min(v, 99) for v in spark]
            return hb

    def debug(self):
        ts = now()
        with self.lock:
            return {
                'device': dict(self.device),
                'tokens_today': self.tokens_today,
                'sessions': {
                    sid: {'host': s['host'], 'turn': s['turn'], 'tools': len(s['tools']),
                          'prompt': bool(s['prompt']), 'idle_s': round(ts - s['last'], 1)}
                    for sid, s in self.sessions.items()
                },
            }


HUB = Hub()


# ── HTTP ─────────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body=b'', ctype='application/json'):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != '/event':
            return self._send(404)
        try:
            n = int(self.headers.get('Content-Length', 0))
            if n > MAX_BODY:
                return self._send(413)
            ev = json.loads(self.rfile.read(n) or b'{}')
            if not isinstance(ev, dict):
                raise ValueError
        except (ValueError, json.JSONDecodeError):
            return self._send(400)
        HUB.event(ev)
        self._send(204)

    def do_GET(self):
        if self.path == '/health':
            return self._send(200, b'ok', 'text/plain')
        if self.path == '/state':
            return self._send(200, json.dumps({**HUB.debug(), 'heartbeat': HUB.snapshot()}, indent=1).encode())
        self._send(404)


# ── Serial ───────────────────────────────────────────────────────────────
def find_port(pref):
    if pref:
        return pref
    cands = sorted(glob.glob('/dev/serial/by-id/*')) or sorted(glob.glob('/dev/ttyUSB*') + glob.glob('/dev/ttyACM*'))
    return cands[0] if cands else None


def open_port(path):
    # Assert neither DTR nor RTS before opening: on CH340/CP210x boards they drive
    # EN/IO0, and opening the port normally would reset the ESP32 every connect.
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = path, 115200, 0.1
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def log(msg):
    print(f'[{datetime.now():%H:%M:%S}] {msg}', flush=True)


def reader(ser, stop):
    buf = b''
    while not stop.is_set():
        try:
            chunk = ser.read(256)
        except (serial.SerialException, OSError):
            return
        if not chunk:
            continue
        buf += chunk
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.strip().decode('utf-8', 'replace')
            if not line:
                continue
            HUB.device['last_rx'] = now()
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue          # boot noise
            if msg.get('cmd') == 'permission':
                log(f"device: {msg.get('decision')} -> {msg.get('id')}")
            elif 'hello' in msg:
                log(f"device up: {msg.get('name')}")
                HUB.resync.set()
        buf = buf[-2048:]


def write_line(ser, obj):
    ser.write((json.dumps(obj, separators=(',', ':')) + '\n').encode())
    ser.flush()


def serial_loop(pref):
    while True:
        path = find_port(pref)
        if not path:
            HUB.device.update(connected=False, port=None)
            time.sleep(2)
            continue
        try:
            ser = open_port(path)
        except (serial.SerialException, OSError) as e:
            HUB.device.update(connected=False, port=path)
            log(f'cannot open {path}: {e}')
            time.sleep(3)
            continue
        log(f'opened {path}')
        HUB.device.update(connected=True, port=path)
        stop = threading.Event()
        threading.Thread(target=reader, args=(ser, stop), daemon=True).start()
        try:
            tz = -(time.altzone if time.localtime().tm_isdst > 0 else time.timezone)
            write_line(ser, {'time': [int(now()), tz]})
            last_rev, last_send, last_time = -1, 0.0, now()
            while True:
                t = now()
                if HUB.resync.is_set():
                    HUB.resync.clear()
                    write_line(ser, {'time': [int(t), tz]})
                    last_time, last_rev = t, -1
                hb = HUB.snapshot()
                with HUB.lock:
                    rev = HUB.rev
                changed = rev != last_rev
                if (changed and t - last_send >= MIN_GAP) or t - last_send >= KEEPALIVE:
                    write_line(ser, hb)
                    last_rev, last_send = rev, t
                    HUB.device['last_send'] = t
                if t - last_time >= 3600:
                    write_line(ser, {'time': [int(t), tz]})
                    last_time = t
                time.sleep(0.05)
        except (serial.SerialException, OSError) as e:
            log(f'serial lost: {e}')
        finally:
            stop.set()
            HUB.device['connected'] = False
            try:
                ser.close()
            except Exception:
                pass
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default=os.environ.get('BUDDY_PORT'), help='serial port (default: auto-detect)')
    ap.add_argument('--http-port', type=int, default=int(os.environ.get('BUDDY_HTTP_PORT', 8765)))
    ap.add_argument('--bind', default=os.environ.get('BUDDY_BIND', '0.0.0.0'))
    args = ap.parse_args()

    threading.Thread(target=serial_loop, args=(args.port,), daemon=True).start()
    srv = ThreadingHTTPServer((args.bind, args.http_port), Handler)
    log(f'hub listening on {args.bind}:{args.http_port}')
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log('stopped')


if __name__ == '__main__':
    sys.exit(main())
