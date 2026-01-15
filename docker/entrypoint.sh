#!/bin/bash
set -e

# FuelWise Platform Entrypoint
# Starts both the API server and nginx

echo "Starting FuelWise Platform..."

# Start API server in background
echo "Starting API server on port 8080..."
/app/fuelwise-api -p 8080 &
API_PID=$!

# Wait for API to be ready
echo "Waiting for API server to be ready..."
for i in {1..30}; do
    if curl -sf http://127.0.0.1:8080/api/v1/health > /dev/null 2>&1; then
        echo "API server is ready"
        break
    fi
    sleep 1
done

# Start nginx in foreground
echo "Starting nginx on port 80..."
nginx -g "daemon off;" &
NGINX_PID=$!

# Trap signals and forward to child processes
trap 'kill $API_PID $NGINX_PID 2>/dev/null' SIGTERM SIGINT

echo "FuelWise Platform is running"
echo "  - UI: http://localhost/"
echo "  - API: http://localhost:8080/api/v1/"

# Wait for either process to exit
wait -n $API_PID $NGINX_PID

# If one exits, kill the other
kill $API_PID $NGINX_PID 2>/dev/null || true
