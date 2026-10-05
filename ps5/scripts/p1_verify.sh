#!/usr/bin/env bash
# P1 verification: build the upstream SDK samples from scratch.
set -e
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
WORK="$HOME/ps5build"
rm -rf "$WORK" && mkdir -p "$WORK"
for s in hello_world notify; do
  cp -r "$PS5_PAYLOAD_SDK/samples/$s" "$WORK/"
  echo "== building $s"
  make -C "$WORK/$s" 2>&1 | tail -5
done
ls -la "$WORK"/*/*.elf
file "$WORK/hello_world/hello_world.elf"
