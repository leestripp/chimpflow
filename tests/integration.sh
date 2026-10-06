#!/usr/bin/env bash
# Integration smoke suite — needs a RUNNING ChimpFlow service.
# Behavioral failures exit 1; unreachable service or missing key SKIP (exit 0).
BASE="${CHIMPFLOW_URL:-http://127.0.0.1:8090}"
KEY="${CHIMPFLOW_KEY:-}"
[ -z "$KEY" ] && [ -f "$HOME/.local/share/chimpflow/ADMIN_KEY" ] && KEY=$(cat "$HOME/.local/share/chimpflow/ADMIN_KEY")

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'PASS  %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }
expect() { # name expected actual
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected $2, got $3)"; fi
}

if ! curl -fsS -m 3 "$BASE/healthz" >/dev/null 2>&1; then
  echo "SKIP: $BASE unreachable — start the service to run integration tests"
  exit 0
fi
if [ -z "$KEY" ]; then echo "SKIP: no API key (set CHIMPFLOW_KEY)"; exit 0; fi
AUTH=(-H "Authorization: Bearer $KEY")

expect "GET /healthz" 200 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/healthz")"
expect "no key -> 401" 401 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/v1/audio/speech" -H 'Content-Type: application/json' -d '{}')"
expect "bad key -> 401" 401 "$(curl -s -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer omn_sk_invalid_000000_deadbeef' "$BASE/v1/models")"
expect "GET /v1/models" 200 "$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" "$BASE/v1/models")"

wav=$(mktemp /tmp/chimpflow-it-XXXX.wav)
code=$(curl -s -o "$wav" -w '%{http_code}' -X POST "$BASE/v1/audio/speech" "${AUTH[@]}" \
  -H 'Content-Type: application/json' \
  -d '{"model":"pocket-tts","input":"integration test one two three","voice":"alba","response_format":"wav"}')
expect "TTS -> 200" 200 "$code"
if head -c 4 "$wav" | grep -q RIFF; then ok "TTS body is WAV"; else bad "TTS body is not RIFF"; fi

resp=$(curl -s -X POST "$BASE/v1/audio/transcriptions" "${AUTH[@]}" -F file=@"$wav" -F model=whisper)
if echo "$resp" | grep -qi "integration test"; then ok "STT round trip: $resp"
else bad "STT round trip got: $resp"; fi

expect "GET /skill/manifest.json" 200 "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/skill/manifest.json")"
if curl -fsS "$BASE/skill/manifest.json" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert isinstance(d.get("endpoints"), list) and len(d["endpoints"]) >= 8' 2>/dev/null
then ok "skill manifest lists all endpoints"; else bad "skill manifest malformed"; fi
if curl -fsS "$BASE/skill/" | grep -q "ChimpFlow"; then ok "skill markdown served"; else bad "skill markdown missing"; fi

raw=$(curl -s -X POST "$BASE/admin/keys" "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"name":"it-scope-test","scopes":["tts"]}')
id=$(echo "$raw"  | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
tkey=$(echo "$raw" | python3 -c 'import json,sys; print(json.load(sys.stdin)["rawKey"])')
expect "tts-only key on STT -> 403" 403 "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/v1/audio/transcriptions" -H "Authorization: Bearer $tkey" -F file=@"$wav" -F model=whisper)"
if curl -s -X DELETE "${AUTH[@]}" "$BASE/admin/keys/$id" | grep -q '"ok":true'; then ok "scope-test key revoked"
else bad "scope-test key NOT revoked"; fi

rm -f "$wav"
echo "---"
echo "integration: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
