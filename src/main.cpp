// ChimpFlow - OmniNet voice service: API-key auth + TTS proxy to audio.cpp
#include "OmniNet.h"
#include "http/HttpClient.h"
#include "token/ApiKeyStore.h"
#include "db/Database.h"
#include "http/StaticHandler.h"
#include "token/ApiKey.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#include <iostream>
#include <string>
#include <string_view>

using namespace OmniNet;
using namespace OmniNet::HTTP;

namespace {

// Defaults for the CHIMPFLOW_* environment variables (see chimpflow.env,
// loaded via systemd EnvironmentFile= — install.sh creates the file).
// Engine endpoint (audio.cpp TTS server)
constexpr std::string_view kEngineBase = "http://127.0.0.1:8080";
// Transcription endpoint (whisper.cpp server; see chimpflow-whisper.service)
constexpr std::string_view kSttBase = "http://127.0.0.1:8081";
// Model the companion whisper service serves. whisper-server has no
// model-listing endpoint, so the status route reports this value — the
// whisper unit reads the same CHIMPFLOW_STT_MODEL var (single source).
constexpr std::string_view kSttModel = "ggml-tiny.en";

// Environment override: empty/unset falls back to the compiled default.
std::string envOr(const char* name, std::string_view fallback) {
    if (const char* v = std::getenv(name); v && *v != '\0') return v;
    return std::string(fallback);
}


// Locate the web/ dir next to the executable (independent of CWD)
std::string webRoot() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return (exe.parent_path() / "web").string();
    return "web";
}

// Extract Bearer token from Authorization header
std::string bearerToken(const Request& req) {
    auto auth = req.authHeader();
    if (auth.substr(0, 7) == "Bearer ") return std::string(auth.substr(7));
    return {};
}

void sendJson(Response& resp, StatusCode code, std::string_view body) {
    resp.setStatus(code);
    resp.setContentType("application/json");
    resp.setBody(body);
}

// Agent-facing guide to this API, served at /skill/ (see README "Skill endpoint").
// Written for an AI agent reader: task-oriented, exact request shapes, honest errors.
constexpr std::string_view kSkillMd = R"CHIMP(---
name: chimpflow-tts
description: Local text-to-speech AND speech-to-text for AI agents on this machine. Use whenever a task needs spoken audio (WAV narration, voice output, read-aloud) or audio transcription. Runs on localhost, no cloud, OpenAI-compatible endpoints.
version: 0.1.2
service: chimpflow
base_url: http://127.0.0.1:8090
---

# ChimpFlow — local TTS + STT gateway for agents

ChimpFlow is a localhost-only voice service. It checks API keys and proxies
text-to-speech to the audio.cpp engine (TTS) and transcription to whisper.cpp
(STT). It never leaves this machine.

## Authentication

Every route except `/healthz` and `/skill/` requires a bearer key.
Any valid key works for `/v1/models`; the rest additionally check scopes:

    Authorization: Bearer omn_sk_...

Keys carry scopes: `admin` (key management), `tts` (speech),
`stt` (transcription). If you do not have a key, ask the human operator —
or create one with an admin key (below). The WebUI at the base URL manages
keys interactively.

## Endpoints

### GET /healthz — no auth

    curl -s http://127.0.0.1:8090/healthz
    {"status":"ok","service":"chimpflow"}

### POST /v1/audio/speech — scope: tts

OpenAI-compatible speech endpoint. Request body:

    {
      "model": "pocket-tts",      // as listed by GET /v1/models
      "input": "Text to speak.",
      "voice": "alba",            // voice id from GET /v1/voices
      "response_format": "wav"    // format the engine supports
    }

Example:

    curl -s -X POST http://127.0.0.1:8090/v1/audio/speech \
      -H "Authorization: Bearer $KEY" \
      -H "Content-Type: application/json" \
      -d '{"model":"pocket-tts","input":"Hello world","voice":"alba","response_format":"wav"}' \
      -o speech.wav

Returns the audio bytes (Content-Type audio/wav; PCM 16-bit mono 24 kHz for
pocket-tts) on success, JSON error otherwise. The body is forwarded to the
engine as-is, so engine-specific extra fields pass through.

### GET /admin/keys — scope: admin

    curl -s -H "Authorization: Bearer $ADMIN_KEY" http://127.0.0.1:8090/admin/keys

Returns an array of keys: id, name, scopes, useCount, revoked, expired, createdAt.

### POST /admin/keys — scope: admin

    curl -s -X POST http://127.0.0.1:8090/admin/keys \
      -H "Authorization: Bearer $ADMIN_KEY" -H "Content-Type: application/json" \
      -d '{"name":"my-agent","scopes":["tts"]}'
    {"id":"...","rawKey":"omn_sk_..."}

`rawKey` is returned ONCE. Store it in the environment or a secrets store,
never in a committed file. If lost, revoke and recreate.

### DELETE /admin/keys/<id> — scope: admin

    curl -s -X DELETE -H "Authorization: Bearer $ADMIN_KEY" \
      http://127.0.0.1:8090/admin/keys/<id>
    {"ok":true}

### POST /v1/audio/transcriptions — scope: stt

OpenAI-compatible transcription. Multipart upload, forwarded to whisper.cpp:

    curl -s -X POST http://127.0.0.1:8090/v1/audio/transcriptions \
      -H "Authorization: Bearer $KEY" \
      -F file=@speech.wav \
      -F model=whisper

Returns JSON {"text":"..."} (verbose_json / other formats as the engine supports).
Any audio format whisper.cpp accepts (wav, mp3, ogg, ...); 16 kHz mono WAV is the
safest. Keep clips modest — the gateway rejects bodies over 10 MB.

### GET /v1/voices — any valid key

    curl -s -H "Authorization: Bearer $KEY" http://127.0.0.1:8090/v1/voices
    {"object":"list","data":[{"id":"alba","object":"voice"}, ...]}

Lists the voices the engine can speak — use these ids as the `voice`
value in the speech route. The engine itself has no voice-listing
endpoint, so ChimpFlow scans the voice-samples directory configured via
`CHIMPFLOW_VOICES_DIR` (see chimpflow.env). Returns 503 if unset/unreadable.

### GET /v1/models — any valid key

    curl -s -H "Authorization: Bearer $KEY" http://127.0.0.1:8090/v1/models

Models currently loaded in the engine — use these ids as the `model`
value in the speech route. (The same list is also at
`/admin/engine/models` for admin keys.) Voice ids come from
`/v1/voices`, not from this list.

## Adding ChimpFlow to your app

ChimpFlow mirrors the OpenAI audio API, so client libraries work by pointing
them at the gateway (base_url http://127.0.0.1:8090/v1, api_key = your
chimpflow key). Integration checklist:

1. **Obtain a key from the human operator** — the gateway never
   self-provisions. With an admin key, mint a dedicated least-privilege key
   for your app (scopes ["tts"], ["stt"], or both); rawKey shows once — store
   it in the app's environment or a secrets store, never in source control.
   The demo's convention: CHIMPFLOW_URL / CHIMPFLOW_KEY / CHIMPFLOW_VOICE.

2. **Discover at runtime; don't hardcode.** On startup GET /v1/models for the
   `model` id and GET /v1/voices for the `voice` id — the engine's loaded
   model and the voice catalog can both change.

3. **Handle the error contract deliberately:** 401 = bad key, stop and tell
   the operator (retrying cannot help); 403 = key lacks scope, needs a
   differently-scoped key; 502 = an engine is down (audio.cpp :8080 or
   whisper :8081), surface it instead of retry-looping.

4. **Preflight batches:** check /healthz first — one bad key 401s every call
   in the job.

5. **Stay localhost:** bind nothing to a LAN address; never forward keys or
   text off this machine.

## Error shape

All errors are JSON: {"error":"human-readable message"}

    401  missing or invalid api key
    403  valid key, missing required scope
    502  engine unreachable (the audio.cpp server on :8080 is down)

## Agent notes

- localhost only: 127.0.0.1:8090. Never send these keys or text anywhere else.
- If speech returns 502, the engine server is down — tell the human operator
  instead of retrying in a loop.
- Check /healthz before a batch job; one bad key will 401 every call in it.
- Voices are engine-loaded speaker embeddings; the right values come from
  GET /v1/voices, not from memory.
)CHIMP";

// Same guide as structured data, for agent frameworks that prefer JSON.
constexpr std::string_view kSkillManifest = R"CHIMP({
  "name": "chimpflow-tts",
  "description": "Local text-to-speech AND speech-to-text for AI agents on this machine. Use whenever a task needs spoken audio (WAV narration, voice output, read-aloud) or audio transcription. Runs on localhost, no cloud, OpenAI-compatible endpoints.",
  "version": "0.1.2",
  "base_url": "http://127.0.0.1:8090",
  "auth": {"type": "bearer", "header": "Authorization", "prefix": "Bearer ", "scopes": ["admin", "tts", "stt"]},
  "endpoints": [
    {"method": "GET",  "path": "/healthz",              "auth": "none"},
    {"method": "GET",  "path": "/skill/",               "auth": "none", "notes": "this document, markdown"},
    {"method": "GET",  "path": "/skill/manifest.json",  "auth": "none", "notes": "this document, structured"},
    {"method": "GET",  "path": "/v1/models",            "auth": "bearer", "notes": "no scope required; discover model ids"},
    {"method": "GET",  "path": "/v1/voices",            "auth": "bearer", "notes": "no scope required; discover voice ids for the speech route"},
    {"method": "POST", "path": "/v1/audio/transcriptions", "auth": "bearer", "scope": "stt", "content_type": "multipart/form-data", "body": {"file": "@audio.wav", "model": "whisper"}, "returns": "JSON {\"text\":\"...\"}"},
    {"method": "POST", "path": "/v1/audio/speech",      "auth": "bearer", "scope": "tts", "content_type": "application/json", "body": {"model": "pocket-tts", "input": "text", "voice": "alba", "response_format": "wav"}, "returns": "audio bytes (WAV)"},
    {"method": "GET",  "path": "/admin/keys",           "auth": "bearer", "scope": "admin"},
    {"method": "POST", "path": "/admin/keys",           "auth": "bearer", "scope": "admin", "body": {"name": "key name", "scopes": ["tts"]}, "notes": "rawKey returned once"},
    {"method": "DELETE", "path": "/admin/keys/{id}",    "auth": "bearer", "scope": "admin"},
    {"method": "GET",  "path": "/admin/engine/models",  "auth": "bearer", "scope": "admin", "notes": "engine model list (proxied; no voices)"},
    {"method": "GET",  "path": "/admin/engine/stt",     "auth": "bearer", "scope": "admin", "notes": "whisper.cpp reachability + served model"}
  ],
  "errors": {"shape": {"error": "message"}, "401": "missing/invalid key", "403": "missing scope", "502": "engine down"}
})CHIMP";

}  // namespace

int main() {
    OmniNet::init();
    Logger::instance().setLevel(LogLevel::INFO);

    // --- Database + API key store ---
    // Data dir lives next to the working directory (created if missing)
    std::filesystem::create_directories("data");
    DB::Database db;
    db.open("data/chimpflow.db");
    Token::ApiKeyStore keys(db);   // namespace TBD by real header
    keys.initialize();

    // Bootstrap: create admin key if none exist for "admin"
    auto existing = keys.listForUser("admin");
    if (existing.empty()) {
        auto res = keys.create("admin", "chimpflow-bootstrap-admin",
                               {"admin", "tts", "stt"});
        std::cout << "\n=== CHIMPFLOW BOOTSTRAP ADMIN KEY (shown once) ===\n"
                  << res.rawKey << "\n===================================================\n\n";
    }

    // --- HTTP server ---
    // --- Config: CHIMPFLOW_* env overrides with localhost defaults ---
    const std::string cfgListenHost = envOr("CHIMPFLOW_LISTEN_HOST", "127.0.0.1");
    const unsigned short cfgListenPort = static_cast<unsigned short>(
        std::stoul(envOr("CHIMPFLOW_LISTEN_PORT", "8090")));
    const std::string cfgEngineBase = envOr("CHIMPFLOW_ENGINE_URL", kEngineBase);
    const std::string cfgSttBase    = envOr("CHIMPFLOW_STT_URL", kSttBase);
    const std::string cfgSttModel   = envOr("CHIMPFLOW_STT_MODEL", kSttModel);
    // Voice catalog: dir of engine voice samples (*.wav). The engine has no
    // voice-listing endpoint, so GET /v1/voices derives ids from these files.
    // Empty/unset -> /v1/voices answers 503 with a configuration hint.
    const std::string cfgVoicesDir  = envOr("CHIMPFLOW_VOICES_DIR", "");
    std::cout << "[chimpflow] listen=" << cfgListenHost << ":" << cfgListenPort
              << " engine=" << cfgEngineBase
              << " stt=" << cfgSttBase << " sttModel=" << cfgSttModel
              << " voicesDir=" << (cfgVoicesDir.empty() ? "<unset>" : cfgVoicesDir) << "\n";

    Server::Config serverConfig;
    serverConfig.address = cfgListenHost;  // localhost service; set 0.0.0.0 only if exposing to LAN
    serverConfig.port = cfgListenPort;
    // Clients upload up to ~120 s of audio for STT. OmniNet's default
    // 1 MB request cap drops those mid-upload; raise it to match the
    // hardcoded 10 MB SAFETY_BODY_LIMIT in OmniNet's HttpConnection
    // (effective ceiling until that lands upstream in leestrip/OmniNet).
    serverConfig.maxRequestSize = 10 * 1024 * 1024;
    Server server(serverConfig);

    // Health (open)
    server.get("/healthz", [](const Request&, Response& resp) {
        sendJson(resp, StatusCode::OK, R"({"status":"ok","service":"chimpflow"})");
    });

    // Agent skill doc: how an AI agent should use this API (open, like /healthz).
    // Human-readable markdown at /skill/ and /skill; machine-readable twin at
    // /skill/manifest.json for agent frameworks that prefer structured data.
    auto serveSkill = [](const Request&, Response& resp) {
        resp.setStatus(StatusCode::OK);
        resp.setContentType("text/markdown; charset=utf-8");
        resp.setBody(kSkillMd);
    };
    server.get("/skill", serveSkill);
    server.get("/skill/", serveSkill);
    server.get("/skill/manifest.json", [](const Request&, Response& resp) {
        sendJson(resp, StatusCode::OK, kSkillManifest);
    });

    // TTS: POST /v1/audio/speech  (OpenAI-compatible, proxied to audio.cpp)
    server.post("/v1/audio/speech", [&keys, &cfgEngineBase](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }
        if (!v.key->hasScope("tts"))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"key lacks 'tts' scope"})");

        // Forward the request body as-is to the engine
        ClientRequest fwd;
        fwd.method("POST").url(cfgEngineBase + "/v1/audio/speech")
           .header("Content-Type", "application/json")
           .body(req.bodyAsString());
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) {
            return sendJson(resp, StatusCode::BAD_GATEWAY,
                R"({"error":"engine unreachable"})");
        }
        auto& er = r.value();
        resp.setStatus(er.status());
        resp.setContentType(er.header("Content-Type").empty()
                            ? "audio/wav" : er.header("Content-Type"));
        resp.setBody(er.body());
    });

    // STT: POST /v1/audio/transcriptions (OpenAI-compatible, proxied to whisper.cpp)
    // The multipart body is forwarded byte-for-byte — ChimpFlow never parses it,
    // so it stays a pure auth gateway (10MB OmniNet body limit applies).
    server.post("/v1/audio/transcriptions", [&keys, &cfgSttBase](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }
        if (!v.key->hasScope("stt"))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"key lacks 'stt' scope"})");

        ClientRequest fwd;
        fwd.method("POST").url(cfgSttBase + "/inference")
           .header("Content-Type", req.headerOr("Content-Type", "multipart/form-data"))
           .body(req.bodyAsString());
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) {
            return sendJson(resp, StatusCode::BAD_GATEWAY,
                R"({"error":"transcription engine unreachable"})");
        }
        resp.setStatus(r->status());
        resp.setContentType(r->header("Content-Type").empty()
                            ? "application/json" : r->header("Content-Type"));
        resp.setBody(r->body());
    });

    // Model discovery: any valid key lists engine models (model/voice values
    // for the speech route). Deliberately not admin-gated.
    server.get("/v1/models", [&keys, &cfgEngineBase](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }
        ClientRequest fwd;
        fwd.method("GET").url(cfgEngineBase + "/v1/models");
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable"})");
        resp.setStatus(r->status());
        resp.setContentType("application/json");
        resp.setBody(r->bodyAsStringView());
    });

    // Voice discovery: any valid key lists the voices the engine can speak.
    // The engine (audio.cpp) has no voice-listing endpoint, so ids are the
    // *.wav basenames in the operator-configured CHIMPFLOW_VOICES_DIR — the
    // same names the engine resolves `voice` against.
    server.get("/v1/voices", [&keys, &cfgVoicesDir](const Request& req, Response& resp) {
        auto tok = bearerToken(req);
        if (tok.empty()) return sendJson(resp, StatusCode::UNAUTHORIZED,
            R"({"error":"missing Authorization: Bearer <api_key>"})");
        auto v = keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        if (!v.valid) {
            nlohmann::json err = {{"error", "invalid api key: " + v.error}};
            return sendJson(resp, StatusCode::UNAUTHORIZED, err.dump());
        }
        if (cfgVoicesDir.empty())
            return sendJson(resp, StatusCode::SERVICE_UNAVAILABLE,
                R"({"error":"voices not configured: set CHIMPFLOW_VOICES_DIR in chimpflow.env"})");

        std::error_code ec;
        std::vector<std::string> ids;
        for (const auto& entry : std::filesystem::directory_iterator(cfgVoicesDir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".wav") continue;
            ids.push_back(entry.path().stem().string());
        }
        if (ec)
            return sendJson(resp, StatusCode::SERVICE_UNAVAILABLE,
                nlohmann::json{{"error", "voices dir unreadable: " + ec.message()}}.dump());
        std::ranges::sort(ids);

        nlohmann::json data = nlohmann::json::array();
        for (const auto& id : ids)
            data.push_back({{"id", id}, {"object", "voice"}, {"owned_by", "engine"}});
        nlohmann::json out = {{"object", "list"}, {"data", std::move(data)}};
        sendJson(resp, StatusCode::OK, out.dump());
    });

    // --- Admin API (admin scope required) ---
    auto requireAdmin = [&keys](const Request& req,
                                Token::ApiKeyStore::ValidationResult& out) {
        auto tok = bearerToken(req);
        out = tok.empty() ? Token::ApiKeyStore::ValidationResult::failure("none")
                          : keys.validateAndUse(tok, req.headerOr("X-Forwarded-For", "127.0.0.1"));
        return out.valid && out.key->hasScope("admin");
    };

    // List keys (full detail)
    server.get("/admin/keys", [&keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        nlohmann::json arr = nlohmann::json::array();
        for (auto& k : keys.listForUser("admin")) {
            arr.push_back({
                {"id", k.id}, {"name", k.name}, {"scopes", k.scopes},
                {"useCount", k.useCount}, {"revoked", k.revoked},
                {"expired", k.isExpired()},
                {"createdAt", std::chrono::system_clock::to_time_t(k.createdAt)},
            });
        }
        sendJson(resp, StatusCode::OK, arr.dump());
    });

    // Create key: {"name":"...","scopes":["tts",...]}
    server.post("/admin/keys", [&keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        try {
            auto j = nlohmann::json::parse(req.bodyAsString());
            std::string name = j.value("name", "unnamed");
            std::vector<std::string> scopes = j.value("scopes", std::vector<std::string>{"tts"});
            auto res = keys.create("admin", name, scopes);
            nlohmann::json out = {{"id", res.id}, {"rawKey", res.rawKey}};
            sendJson(resp, StatusCode::CREATED, out.dump());
        } catch (const std::exception& e) {
            nlohmann::json err = {{"error", std::string("bad JSON: ") + e.what()}};
            sendJson(resp, StatusCode::BAD_REQUEST, err.dump());
        }
    });

    // Revoke key by id: DELETE /admin/keys/<id>
    server.delete_(R"(/admin/keys/[a-zA-Z0-9_]+)", [&keys, &requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        std::string path = std::string(req.path());
        auto id = path.substr(path.find_last_of('/') + 1);
        if (keys.revokeById(id))
            sendJson(resp, StatusCode::OK, R"({"ok":true})");
        else
            sendJson(resp, StatusCode::NOT_FOUND, R"({"error":"key not found"})");
    });

    // Engine models (proxied from audio.cpp, admin only)
    server.get("/admin/engine/models", [&requireAdmin, &cfgEngineBase](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        ClientRequest fwd;
        fwd.method("GET").url(cfgEngineBase + "/v1/models");
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable"})");
        resp.setStatus(r->status());
        resp.setContentType("application/json");
        resp.setBody(r->bodyAsStringView());
    });

    // STT engine status (whisper.cpp reachability + served model, admin only).
    // Never 502s: reports {"reachable":false} so the WebUI can show it.
    server.get("/admin/engine/stt", [&](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        ClientRequest fwd;
        fwd.method("GET").url(cfgSttBase + "/health");
        OmniNet::HTTP::ClientConfig cfg;
        cfg.connect_timeout = std::chrono::seconds(2);
        cfg.request_timeout = std::chrono::seconds(4);
        const bool up = Client::get_instance().send_blocking(fwd, cfg).has_value();
        nlohmann::json out = {
            {"engine", "whisper.cpp"},
            {"model", cfgSttModel},
            {"url", cfgSttBase},
            {"reachable", up},
        };
        sendJson(resp, StatusCode::OK, out.dump());
    });

    // --- WebUI (static, open) ---
    StaticHandler staticHandler(webRoot());
    staticHandler.setDefaultFile("index.html");
    auto serveIndex = [&staticHandler](const Request& req, Response& resp) {
        staticHandler.handle(req, resp);
        // Single-file SPA: never let the browser cache index.html, or UI
        // updates look like they "didn't deploy" until a hard refresh.
        resp.setHeader("Cache-Control", "no-cache");
    };
    server.router().get("/", serveIndex);
    server.router().get("/index.html", serveIndex);

    std::cout << "ChimpFlow listening on http://127.0.0.1:8090\n"
              << "  /healthz            (open)\n"
              << "  /skill/             (open — agent guide, markdown + manifest.json)\n"
              << "  /v1/models         (Bearer key, any valid)\n"
              << "  /v1/voices         (Bearer key, any valid)\n"
              << "  /v1/audio/speech    (Bearer key, 'tts' scope)\n"
              << "  /v1/audio/transcriptions (Bearer key, 'stt' scope)\n"
              << "  /admin/engine/stt   (Bearer key, 'admin' scope)\n"
              << "  /admin/keys         (Bearer key, 'admin' scope)\n";
    server.start();
    OmniNet::cleanup();
    return 0;
}
