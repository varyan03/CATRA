#!/usr/bin/env bash
# Build the CATRA daemon: ./build.sh  ->  daemon/catra
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/daemon"

echo "[build] compiling Treelite model (C)..."
gcc -O3 -std=c11 -c main.c -o model.o

echo "[build] compiling + linking daemon (C++20)..."
g++ -std=c++20 -O3 -pthread -Wall -Wextra catra_daemon.cpp telemetry_sensor.cpp anchor_engine.cpp ml_wrapper.cpp model.o -lm -o catra

echo "[build] OK -> $(pwd)/catra"
