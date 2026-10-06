# ChimpFlow — deployment

User-level, no root. Three processes make the service work; know all three
before touching anything.

## What runs where

| Process | Endpoint | How it runs |
|---|---|---|
| `chimpflow` (this repo) | 127.0.0.1:8090 | systemd user unit `chimpflow.service` — created and managed by `install.sh` |
| whisper.cpp `whisper-server` | 127.0.0.1:8081 | systemd user unit `chimpflow-whisper.service` — **created by hand, not by install.sh** |
| audio.cpp `audiocpp_server` | 127.0.0.1:8080 | **manual process — no service.** Started from `/projects/audio-cpp-test`. Dies on reboot/logout, and TTS 502s until restarted. |

## Filesystem layout

- `~/.local/lib/chimpflow/` — binary + `web/` (the WebUI served at `/`)
- `~/.local/share/chimpflow/` — service `WorkingDirectory`
  - `data/chimpflow.db` — API key store (sha-hashed keys, usage counts)
  - `ADMIN_KEY` — bootstrap admin key, mode 600 (created by `install.sh` on a fresh db)
- `~/.config/systemd/user/` — `chimpflow.service`, `chimpflow-whisper.service`
- `/projects/audio-cpp-test/` — audio.cpp engine checkout (`server.json`, `models/` incl. `models/embeddings`)

## Prerequisites

- OmniNet: sibling checkout at `/projects/Websites/OmniNet` (default), or pass
  `-DOMNINET_DIR=/path` at cmake time. A prebuilt `libOmniNet.a` is used if present.
- audio.cpp: built `audiocpp_server` with `server.json`, TTS model
  (`models/pocket-tts-english-q8_0.gguf`), and the speaker-embedding voice dir.
- whisper.cpp: built `whisper-server` at `~/Apps/whisper.cpp/build/bin/` and the
  model at `~/Apps/whisper.cpp/models/ggml-tiny.en.bin`.

## First install

```bash
bash install.sh        # build → cmake --install → unit → enable --now → linger
```

On a **fresh database** the bootstrap admin key prints once and is saved to
`~/.local/share/chimpflow/ADMIN_KEY` (mode 600). There is no recovery for a lost
key — without any admin key the only fix is deleting `data/chimpflow.db`
(destructive). The WebUI at `http://127.0.0.1:8090` manages keys interactively.

Then the two engines (install.sh does **not** do these):

**1. whisper.cpp unit** (`~/.config/systemd/user/chimpflow-whisper.service`):

```ini
[Unit]
Description=whisper.cpp server for ChimpFlow STT (local model, localhost only)

[Service]
Type=simple
ExecStart=%h/Apps/whisper.cpp/build/bin/whisper-server --host 127.0.0.1 --port 8081 --model %h/Apps/whisper.cpp/models/ggml-tiny.en.bin
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=default.target
```

```bash
systemctl --user daemon-reload && systemctl --user enable --now chimpflow-whisper
```

**2. audio.cpp engine** — currently a manual process:

```bash
cd /projects/audio-cpp-test
./audiocpp_server --config server.json --ui --voice-dir models/embeddings --log-file server.log
```

Strongly recommended: wrap it in a `~/.config/systemd/user/audio-cpp.service`
unit with `WorkingDirectory=/projects/audio-cpp-test`, the same `ExecStart`,
and `Restart=on-failure` — otherwise TTS is down after every reboot until
someone starts it by hand.

## Updating

```bash
git pull && bash install.sh   # rebuilds, reinstalls, restarts chimpflow
```

`install.sh` rewrites the `chimpflow.service` unit (daemon-reload included).
WebUI-only changes still go through `install.sh` + restart — **and bump the
header build badge in `web/index.html`** so users can tell stale pages from
current ones (see `docs/design.md` invariants).

## Configuration

There is no config file. Two surfaces:

- **chimpflow itself** — compile-time constants in `src/main.cpp`: listen
  address (`serverConfig` in `main()`, 127.0.0.1:8090), `kEngineBase`
  (audio.cpp URL), `kSttModel`, and the whisper URL constant. Change →
  `cmake --build build` → `bash install.sh` → restart.
- **engines** — flags: whisper `--port`/`--model` in its unit; audio.cpp
  `server.json` + CLI. **Invariant:** `kSttModel` in `src/main.cpp` must match
  the whisper unit's `--model`, or the status page lies about the served model.

## Verification

```bash
curl -s http://127.0.0.1:8090/healthz
curl -s -H "Authorization: Bearer $(cat ~/.local/share/chimpflow/ADMIN_KEY)" \
     http://127.0.0.1:8090/admin/engine/stt
```

WebUI Status tab: Service card + one card per engine (audio.cpp TTS,
whisper.cpp STT), each with endpoint, reachability, and loaded model. Header
badge shows the UI build version.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| TTS returns 502 / audio.cpp card red | engine process not running (it is **not** a service) | restart `audiocpp_server` (see above) |
| STT card red | `chimpflow-whisper.service` down | `systemctl --user restart chimpflow-whisper` |
| WebUI shows old layout after deploy | browser cache | badge tells the truth — hard refresh or `?v=N` |
| Status shows wrong STT model | `kSttModel` ≠ whisper `--model` | align, rebuild, restart |
| Logs | `journalctl --user -u chimpflow -f` (same for `-u chimpflow-whisper`) | |

## Uninstall

```bash
systemctl --user disable --now chimpflow chimpflow-whisper
rm -rf ~/.local/lib/chimpflow ~/.local/share/chimpflow
rm ~/.config/systemd/user/chimpflow.service ~/.config/systemd/user/chimpflow-whisper.service
systemctl --user daemon-reload
```

The sqlite db (and every API key in it) is gone with `~/.local/share/chimpflow`.
