<!-- monkeybyte:type: cpp -->

# ChimpFlow — agent notes

Local voice service: API-key auth + WebUI (OmniNet/C++23) proxying an
audio.cpp server (PocketTTS today; whisper.cpp STT planned). Listens on
127.0.0.1:8090; engine at 127.0.0.1:8080.

## Commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # configure (first time)
cmake --build build -j$(nproc)                    # build → build/chimpflow
bash install.sh                                   # user-level install (systemd --user, no root)
systemctl --user restart chimpflow                # restart the service
journalctl --user -u chimpflow -f                 # service logs
pgrep -x chimpflow | xargs -r kill                # stop a dev instance
setsid ./build/chimpflow > chimpflow.out 2>&1 < /dev/null &   # dev start detached
curl -s http://127.0.0.1:8090/healthz             # smoke test
```

Installed layout (do not confuse with the repo): binary + `web/` in
`~/.local/lib/chimpflow/`, `data/` + `ADMIN_KEY` in `~/.local/share/chimpflow/`
(the service's WorkingDirectory). Boot-time start needs
`loginctl enable-linger $USER` (install.sh does it).

Run from the project root — `data/chimpflow.db` is created in the CWD.
`build/web` must point at `../web` (symlink) so the binary finds the WebUI
next to itself (web root resolves via /proc/self/exe).

## Layout

- `src/main.cpp` — all service code: auth, routes, engine proxy, bootstrap admin key
- `web/index.html` — self-contained admin SPA (vanilla JS, dark theme; see the
  global `webui-design` skill — select/option/button CSS is mandatory)
- `resources/` — static assets (chimp banner)
- `data/` — runtime sqlite db (gitignored)
- `Plan.md` — research history + DECISION (audio.cpp is the engine; all others rejected)

## Hard-won facts (don't relearn)

- OmniNet API: `StatusCode::SCREAMING_SNAKE_CASE`, `validateAndUse(key, ip)` →
  `.valid` / `.key` / `.error`, `Server` is `.start()` not `.run()`. The bundled
  helper is renamed `sendJson` (nlohmann `json` collides with OmniNet's).
- OmniNet bug FIXED here: `TokenUtils.cpp` validated keys by splitting at the
  FIRST `_`, but keys are `omn_sk_<hex>_<secret>` — every key failed its own
  validator. Fix = split at `rfind('_')`. **Push upstream to GitLab (leestrip/OmniNet).**
- WebUI keeps the admin key in localStorage; keys are sha-hashed in sqlite.
- Frontend-format workflow gotcha (ComfyUI side): prompt text lives in BOTH
  `widgets_values[0]` and `widgets_values_named.value` — patch both.

## Conventions

- C++23, warning-clean under the project's flags; RAII, no raw new/delete.
- No new dependencies without asking; OmniNet path via `-DOMNINET_DIR=` (default
  `/projects/Websites/OmniNet`, prebuilt `libOmniNet.a` if present, else builds from source).
- Never commit: `build/`, `data/`, `*.wav`, `.daedalus/`, logs.
- Bootstrap admin key prints ONCE on fresh db (in chimpflow.out) — there is no recovery;
  without keys the only fix is deleting data/chimpflow.db (destructive).
