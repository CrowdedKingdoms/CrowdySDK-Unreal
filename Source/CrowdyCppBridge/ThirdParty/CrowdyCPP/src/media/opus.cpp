// libopus behind crowdy/media/opus.hpp; compiled only with CROWDY_WITH_OPUS=ON.
#include "crowdy/media/opus.hpp"

#include <utility>

#if __has_include(<opus.h>)
#include <opus.h>
#else
#include <opus/opus.h>
#endif

#include "crowdy/media/voice_frames.hpp"

namespace crowdy::media {

namespace {

constexpr opus_int32 kSampleRate = 48000;
constexpr int kSamplesPerMs = 48;
/// The longest frame libopus decodes (120 ms).
constexpr int kMaxFrameSamples = 120 * kSamplesPerMs;

Errc errcOf(int opusError) {
  if (opusError == OPUS_BAD_ARG || opusError == OPUS_BUFFER_TOO_SMALL) return Errc::InvalidArgument;
  if (opusError == OPUS_INVALID_PACKET) return Errc::Malformed;
  return Errc::Rejected;
}

}  // namespace

struct OpusVoiceEncoder::State {
  OpusEncoder* encoder = nullptr;
  std::uint8_t frameMs = 20;
  ~State() {
    if (encoder) opus_encoder_destroy(encoder);
  }
};

OpusVoiceEncoder::OpusVoiceEncoder(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
OpusVoiceEncoder::OpusVoiceEncoder(OpusVoiceEncoder&&) noexcept = default;
OpusVoiceEncoder& OpusVoiceEncoder::operator=(OpusVoiceEncoder&&) noexcept = default;
OpusVoiceEncoder::~OpusVoiceEncoder() = default;

Result<OpusVoiceEncoder> OpusVoiceEncoder::create(const OpusVoiceEncoderOptions& options) {
  const std::uint8_t ms = options.frameMs;
  if ((ms != 10 && ms != 20 && ms != 40 && ms != 60) || options.bitrate < 6000 ||
      options.bitrate > 510000 || options.packetLossPercent < 0 || options.packetLossPercent > 100)
    return Errc::InvalidArgument;
  int error = OPUS_OK;
  auto state = std::make_unique<State>();
  state->frameMs = ms;
  state->encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &error);
  if (error != OPUS_OK || !state->encoder) return errcOf(error);
  if (opus_encoder_ctl(state->encoder, OPUS_SET_BITRATE(options.bitrate)) != OPUS_OK ||
      opus_encoder_ctl(state->encoder, OPUS_SET_PACKET_LOSS_PERC(options.packetLossPercent)) !=
          OPUS_OK ||
      opus_encoder_ctl(state->encoder, OPUS_SET_INBAND_FEC(options.packetLossPercent > 0 ? 1 : 0)) !=
          OPUS_OK)
    return Errc::InvalidArgument;
  return OpusVoiceEncoder(std::move(state));
}

std::uint8_t OpusVoiceEncoder::frameMs() const noexcept { return state_->frameMs; }

std::size_t OpusVoiceEncoder::frameSamples() const noexcept {
  return static_cast<std::size_t>(state_->frameMs) * kSamplesPerMs;
}

Result<std::vector<std::uint8_t>> OpusVoiceEncoder::encode(std::span<const std::int16_t> pcm) {
  if (pcm.size() != frameSamples()) return Errc::InvalidArgument;
  std::vector<std::uint8_t> out(kMaxVoiceFrameBytes);
  const opus_int32 n = opus_encode(state_->encoder, pcm.data(), static_cast<int>(pcm.size()),
                                   out.data(), static_cast<opus_int32>(out.size()));
  if (n < 0) return errcOf(n);
  out.resize(static_cast<std::size_t>(n));
  return out;
}

struct OpusVoiceDecoder::State {
  OpusDecoder* decoder = nullptr;
  ~State() {
    if (decoder) opus_decoder_destroy(decoder);
  }
};

OpusVoiceDecoder::OpusVoiceDecoder(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
OpusVoiceDecoder::OpusVoiceDecoder(OpusVoiceDecoder&&) noexcept = default;
OpusVoiceDecoder& OpusVoiceDecoder::operator=(OpusVoiceDecoder&&) noexcept = default;
OpusVoiceDecoder::~OpusVoiceDecoder() = default;

Result<OpusVoiceDecoder> OpusVoiceDecoder::create() {
  int error = OPUS_OK;
  auto state = std::make_unique<State>();
  state->decoder = opus_decoder_create(kSampleRate, 1, &error);
  if (error != OPUS_OK || !state->decoder) return errcOf(error);
  return OpusVoiceDecoder(std::move(state));
}

Result<std::vector<std::int16_t>> OpusVoiceDecoder::decode(std::span<const std::uint8_t> frame) {
  if (frame.empty()) return Errc::Malformed;
  std::vector<std::int16_t> pcm(kMaxFrameSamples);
  const int n = opus_decode(state_->decoder, frame.data(), static_cast<opus_int32>(frame.size()),
                            pcm.data(), kMaxFrameSamples, 0);
  if (n < 0) return errcOf(n);
  pcm.resize(static_cast<std::size_t>(n));
  return pcm;
}

Result<std::vector<std::int16_t>> OpusVoiceDecoder::conceal(std::uint8_t frameMs) {
  const int samples = frameMs * kSamplesPerMs;
  if (samples == 0 || samples > kMaxFrameSamples || samples % 120 != 0) return Errc::InvalidArgument;
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(samples));
  const int n = opus_decode(state_->decoder, nullptr, 0, pcm.data(), samples, 0);
  if (n < 0) return errcOf(n);
  pcm.resize(static_cast<std::size_t>(n));
  return pcm;
}

}  // namespace crowdy::media
