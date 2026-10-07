# ChimpFlow — deployment

User-level, no root. Three processes make the service work; know all three
before touching anything.

## What runs where

| Process | Endpoint | How it runs |
|---|---|---|
| `chimpflow` (this repo) | 127.0.0.1:8090 | systemd user unit `chimpflow.service` — created and managed by `install.sh` |
| whisper.cpp `whisper-server` | 127.0.0.1:8081 | systemd user unit `chimpflow-whisper.service` — created and managed by `install.sh` |
| audio.cpp `audiocpp_server` | 127.0.0.1:8080 | systemd user unit `chimpflow-tts.service` — created and managed by `install.sh` |

All three are `WantedBy=default.target` and survive reboot/logout via
`loginctl enable-linger` (done by `install.sh`). The gateway unit also has
`Wants=`/`After=` on both engine units, so it starts after them at boot.

## Filesystem layout

- `~/.local/lib/chimpflow/` — binary + `web/` (the WebUI served at `/`)
- `~/.local/share/chimpflow/` — service `WorkingDirectory`
  - `chimpflow.env` — configuration: `CHIMPFLOW_*` env vars read by BOTH services (see below)
  - `data/chimpflow.db` — API key store (sha-hashed keys, usage counts)
  - `ADMIN_KEY` — bootstrap admin key, mode 600 (created by `install.sh` on a fresh db)
- `~/.config/systemd/user/` — `chimpflow.service`, `chimpflow-tts.service`, `chimpflow-whisper.service`
- `/projects/audio-cpp-test/` — audio.cpp engine checkout (`server.json`, `models/` incl. `models/embeddings`)

## Prerequisites

- OmniNet: no manual checkout needed — CMake FetchContent clones
  `git@gitlab.com:leestripp/omninet.git` at tag `main` and every configure
  fast-forwards the checkout to the origin/main tip, so any build always
  compiles the latest upstream code. Never pin a SHA, never patch the
  fetched copy locally.
- audio.cpp: built `audiocpp_server` with `server.json`, TTS model
  (`models/pocket-tts-english-q8_0.gguf`), and the speaker-embedding voice dir.
- whisper.cpp: built `whisper-server` at `~/Apps/whisper.cpp/build/bin/` and the
  model at `~/Apps/whisper.cpp/models/ggml-tiny.en.bin`.

## First install

```bash
bash install.sh        # build → cmake --install → 3 units → enable --now → linger
```

`install.sh` installs **all three** units (`chimpflow`, `chimpflow-tts`,
`chimpflow-whisper`), enables them under `default.target`, and enables
linger — after a reboot everything comes up without a login.

On a **fresh database**, the binary itself generates a bootstrap admin key
(`chimpflow-bootstrap-admin`, scopes `admin`+`tts`+`stt`) and prints it **once**
to stdout (i.e. the journal); restarts with an existing db never reprint it.
`install.sh` scrapes the journal and saves the key to
`~/.local/share/chimpflow/ADMIN_KEY` (mode 600), displaying it at the end of
the install run. There is no recovery for a lost key — without any admin key
the only fix is deleting `data/chimpflow.db` (destructive). The WebUI at
`http://127.0.0.1:8090` manages keys interactively.

The engine units are installed by `install.sh`; for reference:

- `chimpflow-tts.service` — `WorkingDirectory=/projects/audio-cpp-test`,
  `ExecStart=/projects/audio-cpp-test/audiocpp_server --config .../server.json`
- `chimpflow-whisper.service` — `ExecStart=~/Apps/whisper.cpp/build/bin/whisper-server
  --host 127.0.0.1 --port 8081 --model .../${CHIMPFLOW_STT_MODEL}.bin`

Both have `Restart=on-failure` and `WantedBy=default.target`. The engine
checkouts themselves (`/projects/audio-cpp-test`, `~/Apps/whisper.cpp`) must
of course exist — see Prerequisites.

## Updating

```bash
git pull && bash install.sh   # rebuilds, reinstalls, restarts chimpflow
```

`install.sh` rewrites all three units (daemon-reload included).
WebUI-only changes still go through `install.sh` + restart — **and bump the
header build badge in `web/index.html`** so users can tell stale pages from
current ones (see `docs/design.md` invariants).

## Configuration

One env file: `~/.local/share/chimpflow/chimpflow.env`, created by `install.sh`
with defaults. Both `chimpflow.service` and `chimpflow-whisper.service` load it
via `EnvironmentFile=-` (the leading `-` means the file may be absent — the
compiled-in defaults in `src/main.cpp` then apply). Edit it, then
`systemctl --user restart chimpflow chimpflow-whisper`.

| Variable | Default | Meaning |
|---|---|---|
| `CHIMPFLOW_LISTEN_HOST` | `127.0.0.1` | chimpflow bind address |
| `CHIMPFLOW_LISTEN_PORT` | `8090` | chimpflow port |
| `CHIMPFLOW_ENGINE_URL` | `http://127.0.0.1:8080` | audio.cpp TTS engine |
| `CHIMPFLOW_STT_URL` | `http://127.0.0.1:8081` | whisper.cpp STT engine |
| `CHIMPFLOW_STT_MODEL` | `ggml-tiny.en` | whisper model basename; the whisper unit appends `.bin`, so this one value drives both the status page and the service's `--model` |

The effective values are logged at startup
(`journalctl --user -u chimpflow`) as
`[chimpflow] listen=… engine=… stt=… sttModel=…`. audio.cpp itself keeps its
own `server.json` + CLI flags.

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
| TTS returns 502 / audio.cpp card red | `chimpflow-tts.service` down | `systemctl --user restart chimpflow-tts` |
| STT card red | `chimpflow-whisper.service` down | `systemctl --user restart chimpflow-whisper` |
| WebUI shows old layout after deploy | browser cache | badge tells the truth — hard refresh or `?v=N` |
| Status shows wrong STT model | `kSttModel` ≠ whisper `--model` | align, rebuild, restart |
| Logs | `journalctl --user -u chimpflow -f` (same for `-u chimpflow-whisper`) | |

## Uninstall

```bash
systemctl --user disable --now chimpflow chimpflow-tts chimpflow-whisper
rm -rf ~/.local/lib/chimpflow ~/.local/share/chimpflow
rm ~/.config/systemd/user/chimpflow.service ~/.config/systemd/user/chimpflow-tts.service ~/.config/systemd/user/chimpflow-whisper.service
systemctl --user daemon-reload
```

The sqlite db (and every API key in it) is gone with `~/.local/share/chimpflow`.
