#!/usr/bin/env python3
"""PostToolUse hook — decrements running count when a tool finishes."""
import json, os, sys, time

STATE = '/tmp/claude_buddy_state.json'

data = json.load(sys.stdin)
tool = data.get('tool_name', '?')

try:
    with open(STATE) as f:
        state = json.load(f)
except Exception:
    state = {}

running = max(0, state.get('running', 1) - 1)
state['running'] = running
state['msg']     = 'idle' if running == 0 else state.get('msg', '')
state['ts']      = time.time()

tmp = STATE + '.tmp'
with open(tmp, 'w') as f:
    json.dump(state, f)
os.replace(tmp, STATE)
