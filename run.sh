#!/bin/bash

# Define the Unix Domain Socket path used for IPC (Inter-Process Communication)
SOCKET_PATH="/tmp/cam.sock"

# Function to handle resource cleanup on exit
cleanup() {
    echo ""
    echo "Stopping processes and cleaning up resources..."
    
    # Kill all background processes started by this script (Python & C++)
    kill $(jobs -p) 2>/dev/null
    
    # Ensure the Socket file is removed to prevent 'Address already in use' on next run
    rm -f $SOCKET_PATH
    
    echo "Cleanup complete. Exiting."
    exit
}

# Trap system signals (Ctrl+C and termination) to trigger the cleanup function
trap cleanup SIGINT SIGTERM

# --- Pre-launch Environment Setup ---
# Remove any stale socket file from previous failed runs
rm -f $SOCKET_PATH

echo "Starting Camera Producer..."
# Start the libcamera producer in the background
./cam_export &

echo "Starting AI Inference Engine..."
# Start the Python MediaPipe worker in the background
python cam_ai.py &

# --- Intelligent Socket Synchronization ---
echo "Waiting for Unix Socket to be initialized..."
MAX_RETRIES=15
COUNT=0

# Loop until the socket file exists and is a valid Socket type (-S)
while [ ! -S "$SOCKET_PATH" ]; do
    sleep 1 # Check every second
    COUNT=$((COUNT+1))
    
    if [ $COUNT -ge $MAX_RETRIES ]; then
        echo "Error: AI startup timed out. $SOCKET_PATH not found."
        cleanup
    fi
done

echo "-------------------------------------------------------"
echo "Control chain established. System monitoring active."
echo "Press [Ctrl+C] to stop all processes and clean up."
echo "-------------------------------------------------------"

# Block the script and keep it running until background jobs finish or Ctrl+C is pressed
wait