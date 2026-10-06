# ChimpEcho — parrot demo

Hold-to-talk echo app for ChimpFlow: press the button, say something, and the
service transcribes it (whisper.cpp) then speaks it back (PocketTTS).
gtkmm-4 UI, PipeWire mic/speaker, OmniNet HTTP client.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/chimp-echo
```

Config via env: `CHIMPFLOW_URL` (default `http://127.0.0.1:8090`),
`CHIMPFLOW_KEY` (default: reads `~/.local/share/chimpflow/ADMIN_KEY`),
`CHIMPFLOW_VOICE` (default `alba`). The key needs the `stt` + `tts` scopes.
