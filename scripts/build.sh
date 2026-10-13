#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$DIR"

OS_NAME="$(uname -s)"
echo "==> Building Amuse Native Engine for $OS_NAME..."

mkdir -p bin

if [ "$OS_NAME" = "Darwin" ]; then
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
elif [ "$OS_NAME" = "Linux" ]; then
  g++ -O2 -std=c++17 \
    src/native/linux/main_linux.cpp \
    $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1 javascriptcoregtk-4.1 libpulse-simple sqlite3 x11 xi) \
    -lpthread \
    -o bin/amuse-bridge
else
  echo "Error: Unsupported OS '$OS_NAME'"
  exit 1
fi

echo "==> Amuse Native Engine built successfully: bin/amuse-bridge"

chmod +x bin/amuse-bridge
chmod +x bin/amuse 2>/dev/null || true

echo "==> Build complete!"
