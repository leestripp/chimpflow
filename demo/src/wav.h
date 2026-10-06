// ChimpEcho WAV/multipart helpers — header-only so the demo and unit tests
// share one implementation. Pure functions, no I/O.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace chimpecho {

// floats (-1..1), 16 kHz mono -> 16-bit PCM WAV in memory (whisper's favourite)
inline std::vector<std::uint8_t> makeWav16(const std::vector<float>& samples) {
    const auto n = static_cast<std::uint32_t>(samples.size());
    const std::uint32_t dataBytes = n * 2, rate = 16000;
    std::vector<std::uint8_t> w(44 + dataBytes);
    auto put = [&w](std::size_t o, auto v) { std::memcpy(w.data() + o, &v, sizeof v); };
    std::memcpy(w.data(), "RIFF", 4);          put(4,  36u + dataBytes);
    std::memcpy(w.data() + 8, "WAVEfmt ", 8);  put(16, 16u);
    put(20, std::uint16_t{1});                 // PCM
    put(22, std::uint16_t{1});                 // mono
    put(24, rate);  put(28, rate * 2u);  put(32, std::uint16_t{2});  put(34, std::uint16_t{16});
    std::memcpy(w.data() + 36, "data", 4);     put(40, dataBytes);
    for (std::size_t i = 0; i < n; ++i) {
        float s = std::clamp(samples[i], -1.0f, 1.0f);
        auto v = static_cast<std::int16_t>(std::lrint(s * 32767.0f));
        put(44 + i * 2, v);
    }
    return w;
}

// Parse whatever PCM WAV the engine returns (24-bit/24 kHz today) -> floats at
// the stream rate. Returns false on anything we don't understand.
inline bool parseWav(const std::string& bytes, std::uint32_t& rate, std::vector<float>& out) {
    if (bytes.size() < 44 || bytes.compare(0, 4, "RIFF") || bytes.compare(8, 4, "WAVE"))
        return false;
    std::uint16_t fmt = 0, ch = 0, bits = 0;
    rate = 0;
    std::size_t pos = 12, dataPos = 0, dataLen = 0;
    while (pos + 8 <= bytes.size()) {
        std::uint32_t sz;
        std::memcpy(&sz, bytes.data() + pos + 4, 4);
        const char* id = bytes.data() + pos;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::memcpy(&fmt,  bytes.data() + pos + 8, 2);
            std::memcpy(&ch,   bytes.data() + pos + 10, 2);
            std::memcpy(&rate, bytes.data() + pos + 12, 4);
            std::memcpy(&bits, bytes.data() + pos + 22, 2);
        } else if (!std::memcmp(id, "data", 4)) {
            dataPos = pos + 8; dataLen = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || ch != 1 || rate == 0 || dataPos == 0) return false;
    const std::uint8_t* d = reinterpret_cast<const std::uint8_t*>(bytes.data() + dataPos);
    out.clear();
    if (bits == 16) {
        out.resize(dataLen / 2);
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::int16_t v; std::memcpy(&v, d + i * 2, 2); out[i] = v / 32768.0f;
        }
    } else if (bits == 24) {
        out.resize(dataLen / 3);
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::int32_t v = d[i*3] | (d[i*3+1] << 8) | (d[i*3+2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;
            out[i] = v / 8388608.0f;
        }
    } else if (bits == 32) {
        out.resize(dataLen / 4);
        for (std::size_t i = 0; i < out.size(); ++i) {
            std::int32_t v; std::memcpy(&v, d + i * 4, 4); out[i] = v / 2147483648.0f;
        }
    } else return false;
    return true;
}

// multipart/form-data body with a single "file" part (OpenAI STT shape)
inline std::string multipartBody(const std::string& boundary, const std::vector<std::uint8_t>& wav) {
    std::string b;
    b += "--" + boundary + "\r\n";
    b += "Content-Disposition: form-data; name=\"file\"; filename=\"echo.wav\"\r\n";
    b += "Content-Type: audio/wav\r\n\r\n";
    b.append(reinterpret_cast<const char*>(wav.data()), wav.size());
    b += "\r\n--" + boundary + "--\r\n";
    return b;
}

}  // namespace chimpecho
