#!/usr/bin/env bash

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
LOG_FILE="$SCRIPT_DIR/app_stress.log"

cd "$SCRIPT_DIR"

exec > >(tee "$LOG_FILE") 2>&1

echo "stress test log: $LOG_FILE"

make clean
make
spike ./obj/riscv-pke /bin/app_stress
