#!/usr/bin/env python3
"""
Claude Code hook -> Claude Buddy hub.

One script for every event (SessionStart, UserPromptSubmit, PreToolUse,
PostToolUse, Notification, Stop, SessionEnd). Posts a tiny JSON event to the hub
and exits 0 with no output, always: this must never slow down, block or break
Claude Code. If the hub is unreachable it backs off for 20 s so a down hub costs
at most one short timeout, not one per tool call.

Hub URL: $CLAUDE_BUDDY_URL (default http://100.64.149.28:8765/event)
"""
import json
import os
import socket
import sys
import tempfile
import time
import urllib.request

URL = os.environ.get('CLAUDE_BUDDY_URL', 'http://100.64.149.28:8765/event')
DOWN = os.path.join(tempfile.gettempdir(), 'claude_buddy_down')
TIMEOUT = 0.6


def hint_for(tool, inp):
    if not isinstance(inp, dict) or not inp:
        return ''
    for key in ('command', 'file_path', 'path', 'pattern', 'url', 'query', 'description', 'prompt'):
        v = inp.get(key)
        if isinstance(v, str) and v:
            if key in ('file_path', 'path'):
                v = os.path.basename(v) or v
            return ' '.join(v.split())[:43]
    for v in inp.values():
        if isinstance(v, str) and v:
            return ' '.join(v.split())[:43]
    return ''


def token_delta(data, sid):
    """Tokens produced since the last Stop for this session (input + output +
    cache writes; cache reads excluded, they'd dwarf everything). Reads only the
    bytes appended to the transcript since last time."""
    path = data.get('transcript_path')
    if not path or not os.path.exists(path):
        return 0
    off_file = os.path.join(tempfile.gettempdir(), f'claude_buddy_off_{sid}')
    size = os.path.getsize(path)
    try:
        with open(off_file) as f:
            off = int(f.read().strip() or 0)
    except (OSError, ValueError):
        # No baseline: a session that predates the hook. Count from now on rather
        # than backfilling its whole history into "tokens today".
        with open(off_file, 'w') as f:
            f.write(str(size))
        return 0
    if off > size:
        off = 0
    per_msg = {}
    with open(path, 'rb') as f:
        f.seek(off)
        for raw in f:
            try:
                m = json.loads(raw).get('message') or {}
            except ValueError:
                continue
            u = m.get('usage')
            if m.get('role') != 'assistant' or not isinstance(u, dict):
                continue
            n = (u.get('input_tokens') or 0) + (u.get('output_tokens') or 0) + (u.get('cache_creation_input_tokens') or 0)
            key = m.get('id') or raw[:64]
            per_msg[key] = max(per_msg.get(key, 0), n)   # a message spans several lines
    try:
        with open(off_file, 'w') as f:
            f.write(str(size))
    except OSError:
        pass
    return sum(per_msg.values())


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return
    name = data.get('hook_event_name', '')
    sid = str(data.get('session_id', 'unknown'))
    host = socket.gethostname()
    ev = {'session': f'{host}:{sid[:8]}', 'host': host}

    if not name and ('rate_limits' in data or 'context_window' in data):
        # Claude Code status line input: plan usage windows + context fill
        rl = data.get('rate_limits') or {}

        def win(k):
            w = rl.get(k) or {}
            p = w.get('used_percentage')
            return None if p is None else {'p': float(p), 'r': int(w.get('resets_at') or 0)}

        ev.update(t='usage', five=win('five_hour'), seven=win('seven_day'),
                  ctx=(data.get('context_window') or {}).get('used_percentage'))
    elif name == 'SessionStart':
        ev['t'] = 'start'
        try:   # new session: its transcript starts empty, so count everything in it
            with open(os.path.join(tempfile.gettempdir(), f'claude_buddy_off_{sid[:8]}'), 'w') as f:
                f.write('0')
        except OSError:
            pass
    elif name == 'UserPromptSubmit':
        ev['t'] = 'prompt'
    elif name == 'PreToolUse':
        tool = data.get('tool_name', '?')
        ev.update(t='pre', tool=tool, id=data.get('tool_use_id', ''), hint=hint_for(tool, data.get('tool_input')))
    elif name in ('PostToolUse', 'PostToolUseFailure'):
        ev.update(t='post', id=data.get('tool_use_id', ''))
    elif name == 'Notification':
        ev.update(t='notify', message=str(data.get('message', ''))[:120],
                  ntype=data.get('notification_type', ''))
    elif name == 'Stop':
        ev['t'] = 'stop'
        try:
            ev['tokens'] = token_delta(data, sid[:8])
        except Exception:
            ev['tokens'] = 0
    elif name == 'SessionEnd':
        ev['t'] = 'end'
    else:
        return

    try:
        if time.time() - os.path.getmtime(DOWN) < 20:
            return
    except OSError:
        pass
    try:
        req = urllib.request.Request(URL, json.dumps(ev).encode(), {'Content-Type': 'application/json'})
        urllib.request.urlopen(req, timeout=TIMEOUT).close()
    except Exception:
        try:
            open(DOWN, 'w').close()
        except OSError:
            pass


if __name__ == '__main__':
    try:
        main()
    except Exception:
        pass
    sys.exit(0)
