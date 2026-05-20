#!/usr/bin/env python3
"""
Claude Buddy Bridge — M5StickC USB serial companion for Claude Code.

Reads /tmp/claude_buddy_state.json (written by Claude Code hooks) and
forwards heartbeat JSON to the device at 115200 baud. Receives button
events (approve/deny) from the device and writes responses to
/tmp/claude_buddy_response.json for hooks to pick up.

Usage: python3 bridge.py [/dev/ttyUSB0]
"""

import json
import os
import sys
import time
import threading
import serial
from datetime import datetime

PORT = '/dev/ttyUSB0'
BAUD = 115200
STATE_FILE = '/tmp/claude_buddy_state.json'
RESPONSE_FILE = '/tmp/claude_buddy_response.json'
HEARTBEAT_INTERVAL = 5  # seconds


def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def build_heartbeat(state):
    running = state.get('running', 0)
    waiting = state.get('waiting', 0)
    total = state.get('total', max(running + waiting, 1) if (running or waiting) else 0)
    return {
        'total': total,
        'running': running,
        'waiting': waiting,
        'msg': state.get('msg', 'idle')[:47],
        'entries': [str(e)[:91] for e in state.get('entries', [])[:8]],
        'tokens': state.get('tokens', 0),
        'tokens_today': state.get('tokens_today', 0),
        **({'prompt': {
            'id':   state['prompt'].get('id', ''),
            'tool': state['prompt'].get('tool', ''),
            'hint': str(state['prompt'].get('hint', ''))[:43],
        }} if 'prompt' in state else {}),
    }


def handle_device_line(line):
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        return

    cmd = msg.get('cmd')

    if cmd == 'permission':
        req_id = msg.get('id', '')
        decision = msg.get('decision', 'deny')
        ts = datetime.now().strftime('%H:%M:%S')
        print(f"[{ts}] Device: {decision} → {req_id}")
        tmp = RESPONSE_FILE + '.tmp'
        with open(tmp, 'w') as f:
            json.dump({'id': req_id, 'decision': decision}, f)
        os.replace(tmp, RESPONSE_FILE)


def reader_thread(ser):
    buf = b''
    while True:
        try:
            c = ser.read(1)
            if not c:
                continue
            if c in (b'\n', b'\r'):
                if buf:
                    line = buf.decode('utf-8', errors='replace').strip()
                    if line:
                        handle_device_line(line)
                    buf = b''
            else:
                buf += c
                if len(buf) > 1024:
                    buf = b''
        except serial.SerialException:
            print('[buddy] Serial read error — exiting reader')
            break
        except Exception as e:
            print(f'[buddy] Reader error: {e}')


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else PORT
    print(f'[buddy] Opening {port} at {BAUD} baud...')

    try:
        ser = serial.Serial(port, BAUD, timeout=1)
    except serial.SerialException as e:
        print(f'[buddy] Failed: {e}')
        sys.exit(1)

    print('[buddy] Connected. Starting heartbeat loop. Ctrl-C to stop.')

    t = threading.Thread(target=reader_thread, args=(ser,), daemon=True)
    t.start()

    # Time sync on connect
    tz_offset = -(time.timezone if not time.daylight else time.altzone)
    ser.write((json.dumps({'time': [int(time.time()), tz_offset]}) + '\n').encode())

    last_hb = 0
    try:
        while True:
            now = time.time()
            if now - last_hb >= HEARTBEAT_INTERVAL:
                state = load_state()
                hb = build_heartbeat(state)
                line = json.dumps(hb) + '\n'
                try:
                    ser.write(line.encode())
                    ts = datetime.now().strftime('%H:%M:%S')
                    print(f'[{ts}] → run={hb["running"]} wait={hb["waiting"]} '
                          f'msg="{hb["msg"][:24]}"')
                except serial.SerialException as e:
                    print(f'[buddy] Write error: {e}')
                    break
                last_hb = now
            time.sleep(0.1)
    except KeyboardInterrupt:
        print('\n[buddy] Stopped.')
    finally:
        ser.close()


if __name__ == '__main__':
    main()
