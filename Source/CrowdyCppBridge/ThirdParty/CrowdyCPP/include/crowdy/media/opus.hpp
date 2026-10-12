#pragma once
// Opus for the voice convention's codec 1 (VoiceCodec::Opus: 48 kHz, one channel), over
// libopus. Optional: built only when CrowdyCPP is configured with -DCROWDY_WITH_OPUS=ON and
// libopus is found (its CMake package, pkg-config, or a plain header and library search), as
// the target CrowdyCPP::crowdy_opus, which CrowdyCPP::crowdy links in such a build and which
// defines CROWDY_HAS_OPUS for whatever links it. Without it this header declares only
// kOpusAvailable = false, so the default build, install and tests need no Opus. The frames go
// in a voice packet with VoicePacketizer (crowdy/media/voice_frames.hpp).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "crowdy/core/result.hpp"

namespace crowdy::media {

#if defined(CROWDY_HAS_OPUS)

/// This build wraps libopus.
inline constexpr bool kOpusAvailable = true;

/// Settings for OpusVoiceEncoder.
struct OpusVoiceEncoderOptions {
  /// Frame duration: 10, 20, 40 or 60 ms.
  std::uint8_t frameMs = 20;
  /// Target bitrate in bits per second (6000-510000).
  int bitrate = 24000;
  /// Expected packet loss in percent (0-100); above 0 libopus adds in-band FEC.
  int packetLossPercent = 0;
};

/// Encodes 48 kHz mono 16-bit PCM into Opus frames (OPUS_APPLICATION_VOIP). Move-only;
/// one per sender.
class OpusVoiceEncoder {
 public:
  /// InvalidArgument for options outside their ranges or that libopus refuses.
  static Result<OpusVoiceEncoder> create(const OpusVoiceEncoderOptions& options = {});

  OpusVoiceEncoder(OpusVoiceEncoder&&) noexcept;
  OpusVoiceEncoder& operator=(OpusVoiceEncoder&&) noexcept;
  ~OpusVoiceEncoder();

  std::uint8_t frameMs() const noexcept;
  /// Samples one frame holds (48 a millisecond).
  std::size_t frameSamples() const noexcept;

  /// Encode one frame of exactly frameSamples() samples (InvalidArgument for another
  /// count; Rejected when libopus fails). At most kMaxVoiceFrameBytes bytes.
  Result<std::vector<std::uint8_t>> encode(std::span<const std::int16_t> pcm);

 private:
  struct State;
  explicit OpusVoiceEncoder(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

/// Decodes Opus frames to 48 kHz mono 16-bit PCM and conceals the lost ones. Move-only;
/// one per sender (it keeps that sender's decoder state).
class OpusVoiceDecoder {
 public:
  static Result<OpusVoiceDecoder> create();

  OpusVoiceDecoder(OpusVoiceDecoder&&) noexcept;
  OpusVoiceDecoder& operator=(OpusVoiceDecoder&&) noexcept;
  ~OpusVoiceDecoder();

  /// Decode one frame (Malformed for bytes libopus cannot decode).
  Result<std::vector<std::int16_t>> decode(std::span<const std::uint8_t> frame);
  /// Packet-loss concealment for one missing frame of `frameMs` (a VoicePlayout gap):
  /// libopus extrapolates from what it decoded last. InvalidArgument for a duration
  /// that is not a multiple of 2.5 ms up to 120 ms.
  Result<std::vector<std::int16_t>> conceal(std::uint8_t frameMs);

 private:
  struct State;
  explicit OpusVoiceDecoder(std::unique_ptr<State> state) noexcept;
  std::unique_ptr<State> state_;
};

#else

/// This build does not wrap libopus (configure with -DCROWDY_WITH_OPUS=ON).
inline constexpr bool kOpusAvailable = false;

#endif

}  // namespace crowdy::media
