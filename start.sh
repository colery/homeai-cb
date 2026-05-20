#!/bin/bash
# Start the Claude Buddy bridge in the background.
# Usage: ./start.sh [/dev/ttyUSB0]
PORT="${1:-/dev/ttyUSB0}"
PIDFILE="/tmp/claude_buddy.pid"
LOG="/tmp/claude_buddy.log"

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  echo "Bridge already running (PID $(cat "$PIDFILE")). Use ./stop.sh to stop it."
  exit 0
fi

python3 -u "$(dirname "$0")/bridge.py" "$PORT" > "$LOG" 2>&1 &
echo $! > "$PIDFILE"
echo "Claude Buddy bridge started (PID $!, log: $LOG)"
