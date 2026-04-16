#!/usr/bin/env bash
# Build systemd-manager
# Requires: libgtk-3-dev
#   sudo apt install libgtk-3-dev

set -e
gcc $(pkg-config --cflags gtk+-3.0) \
    -o systemd-manager systemd-manager.c \
    $(pkg-config --libs gtk+-3.0) \
    -lpthread -std=gnu11 -O2

echo "Built: ./systemd-manager"
echo ""
echo "Run with:  sudo ./systemd-manager   (full control)"
echo "       or  ./systemd-manager        (read-only / journal only)"
