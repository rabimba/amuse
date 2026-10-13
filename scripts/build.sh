#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

echo "==> Building Amuse Native Engine..."

mkdir -p bin

clang -O2 \
  -fobjc-arc \
  -I src/native \
  -I src/native/connectors \
  -framework Cocoa \
  -framework WebKit \
  -framework AVFoundation \
  -framework Contacts \
  -framework EventKit \
  -lsqlite3 \
  src/native/connectors/MessagesConnector.m \
  src/native/connectors/ContactsConnector.m \
  src/native/connectors/CalendarConnector.m \
  src/native/connectors/NotesConnector.m \
  src/native/connectors/LocalConnectorsManager.m \
  src/native/AudioEngine.m \
  src/native/HotkeyMonitor.m \
  src/native/AmuseHUD.m \
  src/native/main.m \
  -o bin/amuse-bridge

echo "==> Amuse Native Engine built successfully: bin/amuse-bridge"

chmod +x bin/amuse-bridge
chmod +x bin/amuse 2>/dev/null || true

echo "==> Build complete!"
