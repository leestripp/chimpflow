// Unit tests for the ChimpEcho WAV/multipart helpers (demo/src/wav.h).
#include "test_util.h"
#include "wav.h"

#include <cstring>

using namespace chimpecho;

namespace {

// Build a PCM WAV by hand: fmt chunk + optional junk chunk + data chunk.
std::string makePcmWav(std::uint16_t bits, std::uint16_t channels, std::uint32_t rate,
                       const std::vector<std::uint8_t>& data, bool addOddJunk) {
    std::string w{"RIFF\0\0\0\0WAVE", 12};  // explicit length: literal has embedded NULs
    w.append("fmt ", 4);
    auto append = [&w](const void* p, std::size_t n) { w.append(static_cast<const char*>(p), n); };
    auto u16 = [&](std::uint16_t v) { append(&v, 2); };
    auto u32 = [&](std::uint32_t v) { append(&v, 4); };
    u32(16);                       // fmt chunk size
    u16(1); u16(channels); u32(rate);
    u32(rate * channels * bits / 8); u16(channels * bits / 8u); u16(bits);
    if (addOddJunk) {              // 5-byte chunk -> exercises pad skip
        const char junk[8] = {'L','I','S','T', 5,0,0,0};
        append(junk, 8); append("abcde", 5); append("\0", 1);
    }
    const char id[4] = {'d','a','t','a'};
    append(id, 4); u32(static_cast<std::uint32_t>(data.size())); append(data.data(), data.size());
    std::uint32_t riffSize = static_cast<std::uint32_t>(w.size()) - 8;
    std::memcpy(w.data() + 4, &riffSize, 4);
    return w;
}

int run_tests() {
    // ---- makeWav16 round trip ----
    {
        const std::vector<float> in{0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.001f};
        auto wav = makeWav16(in);
        CHECK_EQ(wav.size(), 44 + in.size() * 2);
        CHECK(!std::memcmp(wav.data(), "RIFF", 4));
        CHECK(!std::memcmp(wav.data() + 8, "WAVE", 4));
        std::uint32_t rate = 0;
        std::vector<float> out;
        CHECK(parseWav(std::string(reinterpret_cast<const char*>(wav.data()), wav.size()), rate, out));
        CHECK_EQ(rate, 16000);
        CHECK_EQ(out.size(), in.size());
        const auto n = std::min(in.size(), out.size());  // size mismatch already flagged above
        for (std::size_t i = 0; i < n; ++i) CHECK_NEAR(out[i], in[i], 1.0f / 32767.0f + 1e-6f);
    }
    // ---- makeWav16 clamps out-of-range ----
    {
        auto wav = makeWav16({2.0f, -3.0f});
        std::int16_t hi, lo;
        std::memcpy(&hi, wav.data() + 44, 2);
        std::memcpy(&lo, wav.data() + 46, 2);
        CHECK_EQ(hi, 32767);
        CHECK_EQ(lo, -32767);  // -3.0f clamps to -1.0f -> lrint(-32767)
    }
    // ---- parseWav: 24-bit with sign extension (engine's current format) ----
    {
        std::vector<std::uint8_t> raw = {0xFF, 0xFF, 0x7F,   // +8388607 -> ~1.0
                                         0x00, 0x00, 0x80,   // -8388608 -> -1.0
                                         0x00, 0x00, 0x00};  // 0
        auto wav = makePcmWav(24, 1, 24000, raw, /*addOddJunk=*/true);
        std::uint32_t rate = 0;
        std::vector<float> out;
        CHECK(parseWav(wav, rate, out));
        CHECK_EQ(rate, 24000);
        CHECK_EQ(out.size(), 3);
        if (out.size() == 3) {
            CHECK_NEAR(out[0], 8388607.0f / 8388608.0f, 1e-7f);
            CHECK_NEAR(out[1], -1.0f, 1e-7f);
            CHECK_NEAR(out[2], 0.0f, 1e-7f);
        }
    }
    // ---- parseWav: 32-bit path ----
    {
        std::vector<std::uint8_t> raw(8);
        std::int32_t v = 1073741824;  // 0.5 in Q31
        std::memcpy(raw.data(), &v, 4);
        std::memcpy(raw.data() + 4, &v, 4);
        auto wav = makePcmWav(32, 1, 48000, raw, false);
        std::uint32_t rate = 0;
        std::vector<float> out;
        CHECK(parseWav(wav, rate, out));
        CHECK_EQ(rate, 48000);
        CHECK_EQ(out.size(), 2);
        if (!out.empty()) CHECK_NEAR(out[0], 0.5f, 1e-7f);
    }
    // ---- parseWav: rejections ----
    {
        std::uint32_t rate = 0;
        std::vector<float> out;
        CHECK(!parseWav("", rate, out));
        CHECK(!parseWav("not a wav at all, just some text......", rate, out));
        CHECK(!parseWav(std::string(20, '\0'), rate, out));  // truncated header
        // non-PCM (IEEE float, fmt=3)
        auto f = makePcmWav(32, 1, 16000, std::vector<std::uint8_t>(8, 0), false);
        f[20] = 3;
        CHECK(!parseWav(f, rate, out));
        // stereo
        CHECK(!parseWav(makePcmWav(16, 2, 16000, std::vector<std::uint8_t>(8, 0), false), rate, out));
        // 8-bit depth unsupported
        CHECK(!parseWav(makePcmWav(8, 1, 16000, std::vector<std::uint8_t>(4, 0), false), rate, out));
        // fmt present but no data chunk
        std::string noData{"RIFF\0\0\0\0WAVEfmt ", 16};
        std::uint32_t cs = 16; noData.append(reinterpret_cast<const char*>(&cs), 4);
        noData.append(16, '\0');
        CHECK(!parseWav(noData, rate, out));
    }
    // ---- multipartBody framing ----
    {
        const std::vector<std::uint8_t> payload{0x52, 0x49, 0x46, 0x46, 0x00, 0xFF};
        auto body = multipartBody("BOUND", payload);
        CHECK(body.starts_with("--BOUND\r\n"));
        CHECK(body.find("Content-Disposition: form-data; name=\"file\"; filename=\"echo.wav\"") != std::string::npos);
        CHECK(body.find("Content-Type: audio/wav\r\n\r\n") != std::string::npos);
        CHECK(body.ends_with("\r\n--BOUND--\r\n"));
        auto pos = body.find(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        CHECK(pos != std::string::npos);
        // payload bytes survive framing exactly (binary-safe)
        CHECK_EQ(body.substr(pos, payload.size()).size(), payload.size());
        CHECK_EQ(static_cast<std::uint8_t>(body[pos + 5]), 0xFF);
        // empty payload still frames correctly
        auto empty = multipartBody("B", {});
        CHECK(empty == "--B\r\nContent-Disposition: form-data; name=\"file\"; filename=\"echo.wav\"\r\n"
                        "Content-Type: audio/wav\r\n\r\n\r\n--B--\r\n");
    }
    return 0;
}

}  // namespace

int main() {
    run_tests();
    return test::summary("unit_wav");
}
