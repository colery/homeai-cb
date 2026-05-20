#!/usr/bin/env python3
"""Stop hook — marks session as idle when Claude Code stops."""
import json, os, sys, time

STATE = '/tmp/claude_buddy_state.json'

json.load(sys.stdin)  # consume input

try:
    with open(STATE) as f:
        state = json.load(f)
except Exception:
    state = {}

state['running'] = 0
state['waiting'] = 0
state['msg']     = 'idle'
state['ts']      = time.time()
state.pop('prompt', None)

tmp = STATE + '.tmp'
with open(tmp, 'w') as f:
    json.dump(state, f)
os.replace(tmp, STATE)
