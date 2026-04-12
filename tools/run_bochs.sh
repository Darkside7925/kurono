#!/bin/bash
# Run Bochs in background, connect dummy VNC client, capture logs
set -e

cd /mnt/c/Users/genie/OS
rm -f bochs.log bochs_serial.log

# Start Bochs in background (pipe 'c' to debugger stdin, capture console output)
echo -e "c\n" | bochs -f bochsrc.txt -q > /tmp/bochs_console.log 2>&1 &
BOCHS_PID=$!
echo "Bochs PID: $BOCHS_PID"

# Wait for RFB server to start
sleep 3

# Connect dummy VNC client (keeps alive 45 seconds)
python3 tools/vnc_dummy.py 45 2>&1 || echo "(VNC client exited)"

echo "--- Waiting for Bochs to settle ---"
sleep 3

# Kill Bochs
kill $BOCHS_PID 2>/dev/null || true
wait $BOCHS_PID 2>/dev/null || true

echo ""
echo "========================================="
echo "  BOCHS LOG (last 150 lines)"
echo "========================================="
tail -150 bochs.log 2>/dev/null || echo "(no bochs.log)"

echo ""
echo "========================================="
echo "  SERIAL LOG"
echo "========================================="
cat bochs_serial.log 2>/dev/null || echo "(empty)"

echo ""
echo "========================================="
echo "  BOCHS CONSOLE OUTPUT"
echo "========================================="
cat /tmp/bochs_console.log 2>/dev/null || echo "(empty)"
