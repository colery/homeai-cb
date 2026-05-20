#!/usr/bin/env python3
"""PreToolUse hook — updates buddy state when Claude Code runs a tool."""
import json, os, sys, time

STATE = '/tmp/claude_buddy_state.json'

data = json.load(sys.stdin)
tool = data.get('tool_name', '?')
inp = data.get('tool_input', {})
hint = str(next(iter(inp.values()), ''))[:43] if inp else ''

try:
    with open(STATE) as f:
        state = json.load(f)
except Exception:
    state = {'total': 0, 'running': 0, 'waiting': 0, 'entries': [], 'tokens_today': 0}

state['running'] = state.get('running', 0) + 1
state['total']   = max(state.get('total', 0), state['running'])
state['waiting'] = 0
state['msg']     = f'Tool: {tool}'
entries = state.get('entries', [])
entries = [f"{time.strftime('%H:%M')} {tool}"] + entries[:7]
state['entries'] = entries
state['ts'] = time.time()

# Remove stale prompt
state.pop('prompt', None)

tmp = STATE + '.tmp'
with open(tmp, 'w') as f:
    json.dump(state, f)
os.replace(tmp, STATE)
