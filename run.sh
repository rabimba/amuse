#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

# Ensure Node environment from NVM if present
if [ -s "$HOME/.nvm/nvm.sh" ]; then
  # shellcheck disable=SC1090
  source "$HOME/.nvm/nvm.sh"
fi

MODE="${1:-all}"

echo "=================================================================="
echo "          AMUSE — COMPLETE END-TO-END RUNNER                      "
echo "=================================================================="
echo "Platform : $(uname -s) $(uname -m)"
echo "Node.js  : $(node -v)"
echo "Directory: $DIR"
echo "=================================================================="
echo ""

# ------------------------------------------------------------------
# STEP 1: Dependencies
# ------------------------------------------------------------------
echo "==> [1/6] Checking Node.js dependencies..."
if [ ! -d "$DIR/node_modules/ws" ]; then
  npm install --no-fund --no-audit
else
  echo "✓ Dependencies already installed (node_modules/ws)."
fi
echo ""

# ------------------------------------------------------------------
# STEP 2: Native Bridge Build
# ------------------------------------------------------------------
echo "==> [2/6] Compiling Native Engine (bin/amuse-bridge)..."
bash "$DIR/scripts/build.sh"
echo ""

# ------------------------------------------------------------------
# STEP 3: Configuration Check
# ------------------------------------------------------------------
echo "==> [3/6] Verifying Amuse Configuration..."
./bin/amuse config
echo ""

# ------------------------------------------------------------------
# STEP 4: Local & Cloud Muse Connectors & Desktop Diagnostics
# ------------------------------------------------------------------
echo "==> [4/6] Running Muse Connectors, Clock, Notes & Desktop Diagnostics..."
./bin/amuse test
if [ "$(uname -s)" = "Linux" ]; then
  echo "-> Testing Real-Time Clock & Multi-Timezone Grounding (get_current_time_and_environment):"
  ./bin/amuse call get_current_time_and_environment '{}'
  echo "-> Testing Active Muse Connectors Registry (list_connectors):"
  ./bin/amuse call list_connectors '{"category": "all"}' | head -c 400 && echo "..."
  echo "-> Testing Linux Markdown Note Sync (open_note 'About Amuse'):"
  ./bin/amuse call open_note '{"query": "About Amuse", "open_on_desktop": false}'
  echo "-> Testing LinkedIn Social Connector Scheduling (social_media_action):"
  ./bin/amuse call social_media_action '{"platform": "linkedin", "action": "schedule", "content": "Excited to share our Amuse Linux multimodal assistant demo!", "schedule_time": "09:00 CST", "open_in_browser": false}'
  echo "-> Testing Full-Disk File Assistant (manage_files list ~/Documents/AmuseNotes):"
  ./bin/amuse call manage_files '{"action": "list", "path": "~/Documents/AmuseNotes"}'
  echo "-> Testing Muse Active Memory (save_memory & recall_memories):"
  ./bin/amuse call save_memory '{"key": "preferred_timezone", "value": "America/Los_Angeles (PDT) & America/Chicago (CST/CDT)", "category": "preference"}'
  ./bin/amuse call recall_memories '{"query": "timezone"}'
  echo "-> Testing Muse Action Plans (manage_action_plan):"
  ./bin/amuse call manage_action_plan '{"action": "list"}'
  echo "-> Testing Muse Dynamic Custom Tool Builder (build_and_run_custom_tool):"
  ./bin/amuse call build_and_run_custom_tool '{"tool_name": "system_summary", "language": "python", "code": "import platform\nprint(f\"Linux Host: {platform.node()} ({platform.release()})\")", "description": "Prints Linux host summary"}'
  echo "-> Testing Live Screen Vision Capture (take_screenshot):"
  ./bin/amuse call take_screenshot '{"title": "E2E_Screen_Check"}'
  echo "-> Testing Reminders Connector (get_reminders):"
  ./bin/amuse call get_reminders '{"include_completed": false}'
else
  echo "-> macOS: skipping Linux-only connector diagnostics (social/files/scheduler/custom-tool)"
  echo "-> Testing Real-Time Clock (get_current_time_and_environment):"
  ./bin/amuse call get_current_time_and_environment '{}'
  echo "-> Testing Connectors Registry (list_connectors):"
  ./bin/amuse call list_connectors '{"category": "all"}' | head -c 400 && echo "..."
fi
echo ""

# ------------------------------------------------------------------
# STEP 5: Mock Gemini Multimodal Live WebSocket E2E Test
# ------------------------------------------------------------------
echo "==> [5/6] Running Mock Gemini Multimodal Live WebSocket E2E Suite..."
node "$DIR/scripts/test_e2e.mjs"
echo ""

# ------------------------------------------------------------------
# STEP 6: Live Google Gemini API + Native GUI HUD End-to-End Run
# ------------------------------------------------------------------
echo "==> [6/6] Running Live Google Gemini API & Native GUI HUD E2E Suite..."
node "$DIR/scripts/test_live_e2e.mjs"

if [ "$MODE" = "--gui" ] || [ "$MODE" = "--interactive" ]; then
  echo "==> Launching Interactive Amuse Ambient HUD (Press Double-Tap Option/Alt to toggle)..."
  exec ./bin/amuse start
elif [ "$(uname -s)" = "Linux" ]; then
  echo "==> Running Live Native GUI HUD + Gemini Live BidiGenerateContent Duplex Turn..."
  AMUSE_STARTUP_PROMPT="What is my first scheduled calendar event? Reply in one short sentence." \
  AMUSE_EXIT_ON_READY=1 \
  AMUSE_AUTO_EXIT_MS=18000 \
  ./bin/amuse start

  echo ""
  echo "=================================================================="
  echo "✓ ALL 6 END-TO-END STAGES COMPLETED SUCCESSFULLY!"
  echo "  To keep the interactive HUD running continuously, run:"
  echo "    ./run.sh --gui   (or ./bin/amuse)"
  echo "=================================================================="
else
  echo ""
  echo "==> macOS: headless HUD hooks (AMUSE_STARTUP_PROMPT/EXIT_ON_READY) are Linux-only."
  echo "    Launch the HUD manually: ./bin/amuse start"
  echo "=================================================================="
fi
