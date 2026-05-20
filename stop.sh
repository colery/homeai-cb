#!/bin/bash
PIDFILE="/tmp/claude_buddy.pid"
if [ -f "$PIDFILE" ]; then
  PID=$(cat "$PIDFILE")
  kill "$PID" 2>/dev/null && echo "Stopped bridge (PID $PID)" || echo "Process not running"
  rm -f "$PIDFILE"
else
  echo "No bridge PID file found."
fi
