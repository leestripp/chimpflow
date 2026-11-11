#ifndef CHIMPFLOW_AUDIO_FORMAT_H
#define CHIMPFLOW_AUDIO_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace chimpflow {

// Sequentially join PCM16 WAV bodies without re-encoding. Every input must
// have the same sample rate and channel count. Throws std::runtime_error on
// malformed input, inconsistent formats, or excessive output size.
class WavJoiner {
public:
    void append(std::span<const std::uint8_t> wav);
    std::vector<std::uint8_t> finish() const;
    std::size_t pcmBytes() const { return m_pcm.size(); }

private:
    std::uint16_t m_channels = 0;
    std::uint32_t m_sampleRate = 0;
    std::vector<std::uint8_t> m_pcm;
};

// Encode a complete WAV body as MP3 using the locally installed ffmpeg.
// Throws std::runtime_error when the encoder is unavailable, fails, or times out.
std::vector<std::uint8_t> encodeMp3(std::span<const std::uint8_t> wav,
                                    std::string_view ffmpegPath);

}  // namespace chimpflow

#endif  // CHIMPFLOW_AUDIO_FORMAT_H
