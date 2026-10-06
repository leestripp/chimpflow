#!/usr/bin/env bash
# ChimpFlow installer — user-level, no root. Run as: bash install.sh
set -euo pipefail

APP=chimpflow
LIB="$HOME/.local/lib/$APP"            # binary + web/
DATA="$HOME/.local/share/$APP"         # WorkingDirectory; holds data/ + ADMIN_KEY
UNIT_DIR="$HOME/.config/systemd/user"
ENV_FILE="$DATA/chimpflow.env"          # systemd EnvironmentFile for both services
# Respect an existing env file when installing (it overrides the defaults below)
if [[ -f "$ENV_FILE" ]]; then
  source "$ENV_FILE"
fi
PORT="${CHIMPFLOW_LISTEN_PORT:-8090}"
ENGINE_URL="${CHIMPFLOW_ENGINE_URL:-http://127.0.0.1:8080}"

echo "==> Building ($APP)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo "==> Installing to ~/.local (cmake --install)"
mkdir -p "$DATA/data" "$UNIT_DIR"
cmake --install build --prefix "$HOME/.local"

if [[ ! -f "$ENV_FILE" ]]; then
cat > "$ENV_FILE" << 'ENV'
# ChimpFlow configuration — systemd EnvironmentFile, read by chimpflow.service
# and chimpflow-whisper.service. Edit, then restart both:
#   systemctl --user restart chimpflow chimpflow-whisper
CHIMPFLOW_LISTEN_HOST=127.0.0.1
CHIMPFLOW_LISTEN_PORT=8090
CHIMPFLOW_ENGINE_URL=http://127.0.0.1:8080
CHIMPFLOW_STT_URL=http://127.0.0.1:8081
# Whisper model basename (the whisper unit appends .bin). Single source for
# both the status page and the whisper service's --model.
CHIMPFLOW_STT_MODEL=ggml-tiny.en
ENV
  echo "==> Created $ENV_FILE (defaults)"
fi

echo "==> Stopping any dev instance on :$PORT"
pkill -x "$APP" 2>/dev/null && sleep 1 || true

echo "==> Installing systemd user unit"
cat > "$UNIT_DIR/$APP.service" << 'UNIT'
[Unit]
Description=ChimpFlow voice service (OmniNet TTS proxy)

[Service]
Type=simple
WorkingDirectory=%h/.local/share/chimpflow
EnvironmentFile=-%h/.local/share/chimpflow/chimpflow.env
ExecStart=%h/.local/lib/chimpflow/chimpflow
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now "$APP"

echo "==> Enabling boot-time start without login (linger)"
loginctl enable-linger "$USER" 2>/dev/null \
  || echo "    WARN: could not enable linger — service starts on login only"

sleep 2
echo "==> Status"
systemctl --user --no-pager --full status "$APP" | head -n 6 || true

# --- bootstrap admin key (only exists on a FRESH db) ---
KEY=$(journalctl --user -u "$APP" --since "-2min" -o cat 2>/dev/null \
        | grep -A1 "BOOTSTRAP ADMIN KEY" | tail -n 1 | tr -d ' ' || true)
if [[ -n "$KEY" ]]; then
  printf '%s\n' "$KEY" > "$DATA/ADMIN_KEY"
  chmod 600 "$DATA/ADMIN_KEY"
  echo
  echo "==> FRESH DATABASE — bootstrap admin key (saved to $DATA/ADMIN_KEY, mode 600):"
  echo "    $KEY"
else
  echo "    Existing database kept — keys preserved (see WebUI to manage them)."
fi

echo
if curl -sf -m 2 "$ENGINE_URL/v1/models" > /dev/null; then
  echo "==> Engine reachable at $ENGINE_URL — TTS ready."
else
  echo "==> WARN: audio.cpp engine not reachable at $ENGINE_URL."
  echo "    TTS requests will fail until the engine server is running."
fi

echo
echo "==> Done. WebUI: http://127.0.0.1:$PORT  ·  logs: journalctl --user -u $APP -f"
