// ChimpFlow - OmniNet voice service: API-key auth + TTS proxy to audio.cpp
#include "OmniNet.h"
#include "http/HttpClient.h"
#include "token/ApiKeyStore.h"
#include "db/Database.h"
#include "http/StaticHandler.h"
#include "token/ApiKey.h"
#include <nlohmann/json.hpp>
#include <filesystem>

#include <iostream>
#include <string>
#include <string_view>

using namespace OmniNet;
using namespace OmniNet::HTTP;

namespace {

// Engine endpoint (audio.cpp server)
constexpr std::string_view kEngineBase = "http://127.0.0.1:8080";


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
    Server::Config serverConfig;
    serverConfig.address = "127.0.0.1";   // localhost service; set 0.0.0.0 only if exposing to LAN
    serverConfig.port = 8090;
    Server server(serverConfig);

    // Health (open)
    server.get("/healthz", [](const Request&, Response& resp) {
        sendJson(resp, StatusCode::OK, R"({"status":"ok","service":"chimpflow"})");
    });

    // TTS: POST /v1/audio/speech  (OpenAI-compatible, proxied to audio.cpp)
    server.post("/v1/audio/speech", [&keys](const Request& req, Response& resp) {
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
        fwd.method("POST").url(std::string(kEngineBase) + "/v1/audio/speech")
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
    server.get("/admin/engine/models", [&requireAdmin](const Request& req, Response& resp) {
        Token::ApiKeyStore::ValidationResult v;
        if (!requireAdmin(req, v))
            return sendJson(resp, StatusCode::FORBIDDEN, R"({"error":"admin scope required"})");
        ClientRequest fwd;
        fwd.method("GET").url(std::string(kEngineBase) + "/v1/models");
        auto r = Client::get_instance().send_blocking(fwd);
        if (!r) return sendJson(resp, StatusCode::BAD_GATEWAY, R"({"error":"engine unreachable"})");
        resp.setStatus(r->status());
        resp.setContentType("application/json");
        resp.setBody(r->bodyAsStringView());
    });

    // --- WebUI (static, open) ---
    StaticHandler staticHandler(webRoot());
    staticHandler.setDefaultFile("index.html");
    auto serveIndex = [&staticHandler](const Request& req, Response& resp) {
        staticHandler.handle(req, resp);
    };
    server.router().get("/", serveIndex);
    server.router().get("/index.html", serveIndex);

    std::cout << "ChimpFlow listening on http://127.0.0.1:8090\n"
              << "  /healthz            (open)\n"
              << "  /v1/audio/speech    (Bearer key, 'tts' scope)\n"
              << "  /admin/keys         (Bearer key, 'admin' scope)\n";
    server.start();
    OmniNet::cleanup();
    return 0;
}
