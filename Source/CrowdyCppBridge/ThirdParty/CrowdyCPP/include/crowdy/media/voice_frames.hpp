#pragma once
// Voice payloads: an optional convention for what an app carries inside an audio
// payload (ClientAudioPacket 134, delivered as ClientAudioNotification 135).
//
// The server never looks inside an audio payload, and an app with a format of its
// own keeps it. This is a shared one, implemented byte for byte by both SDKs
// (CrowdyJS src/media/voice-frames.ts is the reference; tests/voice_frames_test.cpp
// replays its cases, tools/parity/fixtures/voice-frames.json, a copy of CrowdyJS's
// test/unit/fixtures/voice-frames.json): a 10-byte header in front of each codec
// frame, integers little-endian.
//
//   offset  size  field      meaning
//   0       1     version    1. A reader refuses any other version.
//   1       1     codec      0 = unspecified / raw, 1 = Opus 48 kHz mono,
//                            2 = G.711 mu-law 8 kHz.
//   2       2     seq        uint16, +1 per packet sent, wraps.
//   4       4     timestamp  uint32 in codec samples (48 kHz for Opus, 8 kHz for
//                            mu-law; milliseconds for codec 0 and any codec not
//                            listed), wraps.
//   8       1     frameMs    the frame's duration in milliseconds.
//   9       1     flags      bit 0: the first packet after silence (a talk spurt
//                            starts); bit 1: the last packet before silence.
//                            Other bits are reserved.
//   10      ...   frame      one codec frame.
//
// A reader refuses a packet shorter than 10 bytes or of another version
// (decodeVoicePacket returns nullopt), and nothing here throws on a packet from the
// network. VoicePacketizer numbers one sender's frames and sets the talk-spurt
// flags; VoiceJitterBuffer puts each sender's packets back in order, plays them out
// after a fixed delay and reports the frames that never came as gaps. Header-only,
// no networking and no codec: a build configured with CROWDY_WITH_OPUS=ON adds
// crowdy/media/opus.hpp's libopus wrapper. Where a voice sits in the world (panning,
// distance attenuation) is the game's to decide.

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crowdy::media {

/// Bytes of voice header in front of every codec frame.
constexpr std::size_t kVoiceHeaderBytes = 10;
/// The one header version this SDK writes and accepts.
constexpr std::uint8_t kVoiceHeaderVersion = 1;
/// Largest codec frame one audio payload carries with the HMAC tail present (1123 - 10).
constexpr std::size_t kMaxVoiceFrameBytes = 1113;
/// Default VoiceJitterBufferOptions::targetDelayMs.
constexpr std::int64_t kVoiceTargetDelayMs = 60;
/// Default VoiceJitterBufferOptions::maxFrames.
constexpr int kVoiceJitterMaxFrames = 64;
/// Default VoiceJitterBufferOptions::resetAfterMs.
constexpr std::int64_t kVoiceResetAfterMs = 200;

/// Codecs the header names. Any other value is carried through as it is.
enum class VoiceCodec : std::uint8_t {
  Raw = 0,    ///< unspecified, or raw samples in a format the app defines; timestamps count ms
  Opus = 1,   ///< Opus, 48 kHz, one channel
  Mulaw = 2,  ///< G.711 mu-law, 8 kHz, one byte per sample
};

/// Header flag bits.
struct VoiceFlag {
  static constexpr std::uint8_t kSpurtStart = 0x01;  ///< the first packet after silence
  static constexpr std::uint8_t kSpurtEnd = 0x02;    ///< the last packet before silence
};

/// A parsed or to-be-written voice header.
struct VoiceHeader {
  std::uint8_t version = kVoiceHeaderVersion;
  std::uint8_t codec = 0;
  std::uint16_t seq = 0;
  std::uint32_t timestamp = 0;
  std::uint8_t frameMs = 0;
  std::uint8_t flags = 0;
};

/// A parsed voice packet: its header and the codec frame after it (a view into the
/// packet, valid as long as the packet is).
struct VoicePacket {
  VoiceHeader header;
  std::span<const std::uint8_t> frame;
};

/// The sample clock a codec's timestamps count: 48000 (Opus), 8000 (mu-law), else 1000.
inline constexpr std::uint32_t voiceClockRate(std::uint8_t codec) noexcept {
  if (codec == static_cast<std::uint8_t>(VoiceCodec::Opus)) return 48000;
  if (codec == static_cast<std::uint8_t>(VoiceCodec::Mulaw)) return 8000;
  return 1000;
}

/// How far the timestamp moves for one frame of `frameMs` milliseconds.
inline constexpr std::uint32_t voiceSamplesPerFrame(std::uint8_t codec, std::uint8_t frameMs) noexcept {
  return voiceClockRate(codec) / 1000 * frameMs;
}

/// The distance from seq `b` to seq `a` on the wrapping uint16 counter, from -32768
/// to 32767: positive when `a` is newer.
inline constexpr int voiceSeqDiff(std::uint16_t a, std::uint16_t b) noexcept {
  const int d = static_cast<std::uint16_t>(a - b);
  return d >= 0x8000 ? d - 0x10000 : d;
}

/// Write a voice header (always version 1, whatever `header.version` says).
inline std::array<std::uint8_t, kVoiceHeaderBytes> encodeVoiceHeader(const VoiceHeader& header) noexcept {
  std::array<std::uint8_t, kVoiceHeaderBytes> out{};
  out[0] = kVoiceHeaderVersion;
  out[1] = header.codec;
  out[2] = static_cast<std::uint8_t>(header.seq & 0xff);
  out[3] = static_cast<std::uint8_t>(header.seq >> 8);
  for (int i = 0; i < 4; ++i) out[static_cast<std::size_t>(4 + i)] = static_cast<std::uint8_t>(header.timestamp >> (8 * i));
  out[8] = header.frameMs;
  out[9] = header.flags;
  return out;
}

/// `header || frame`, ready for Connection::sendAudio. Empty when the frame is longer
/// than kMaxVoiceFrameBytes (callers check `.empty()` rather than catching).
inline std::vector<std::uint8_t> encodeVoicePacket(const VoiceHeader& header,
                                                   std::span<const std::uint8_t> frame) {
  std::vector<std::uint8_t> packet;
  if (frame.size() > kMaxVoiceFrameBytes) return packet;
  const auto head = encodeVoiceHeader(header);
  packet.reserve(kVoiceHeaderBytes + frame.size());
  packet.insert(packet.end(), head.begin(), head.end());
  packet.insert(packet.end(), frame.begin(), frame.end());
  return packet;
}

/// Parse a voice packet. nullopt for a packet shorter than kVoiceHeaderBytes or of a
/// version other than kVoiceHeaderVersion; codecs and flag bits this SDK does not
/// know are returned as they are.
inline std::optional<VoicePacket> decodeVoicePacket(std::span<const std::uint8_t> packet) noexcept {
  if (packet.size() < kVoiceHeaderBytes || packet[0] != kVoiceHeaderVersion) return std::nullopt;
  VoicePacket out;
  out.header.version = packet[0];
  out.header.codec = packet[1];
  out.header.seq = static_cast<std::uint16_t>(packet[2] | (packet[3] << 8));
  out.header.timestamp = static_cast<std::uint32_t>(packet[4]) |
                         (static_cast<std::uint32_t>(packet[5]) << 8) |
                         (static_cast<std::uint32_t>(packet[6]) << 16) |
                         (static_cast<std::uint32_t>(packet[7]) << 24);
  out.header.frameMs = packet[8];
  out.header.flags = packet[9];
  out.frame = packet.subspan(kVoiceHeaderBytes);
  return out;
}

/// Options for VoicePacketizer.
struct VoicePacketizerOptions {
  /// VoiceCodec, or an app's own value.
  std::uint8_t codec = static_cast<std::uint8_t>(VoiceCodec::Opus);
  /// The duration of every frame, 1-255 ms (0 makes packetize() refuse every frame).
  std::uint8_t frameMs = 20;
  /// The first packet's seq.
  std::uint16_t seq = 0;
  /// The first packet's timestamp.
  std::uint32_t timestamp = 0;
};

/// Numbers one sender's frames: each packet gets the next seq (uint16, wraps) and a
/// timestamp one frame later than the last (uint32, wraps). The first packet, and the
/// first after a `last` one or a skip(), carries VoiceFlag::kSpurtStart. Keep one
/// packetizer for as long as the sender speaks with the same codec and frame
/// duration, muted stretches included: a fresh one starts its seq over, and a
/// receiver that still holds the old stream drops packets that look old to it until
/// the seq catches up or the stream is reset.
class VoicePacketizer {
 public:
  explicit VoicePacketizer(VoicePacketizerOptions options = {}) noexcept
      : codec_(options.codec),
        frameMs_(options.frameMs),
        seq_(options.seq),
        timestamp_(options.timestamp) {}

  std::uint8_t codec() const noexcept { return codec_; }
  std::uint8_t frameMs() const noexcept { return frameMs_; }
  /// The seq the next packet will carry.
  std::uint16_t nextSeq() const noexcept { return seq_; }
  /// The timestamp the next packet will carry.
  std::uint32_t nextTimestamp() const noexcept { return timestamp_; }

  /// The next packet: the header, then `frame`. `last`: this is the last packet
  /// before silence (VoiceFlag::kSpurtEnd); the next one starts a talk spurt. Empty,
  /// and nothing numbered, when the frame is longer than kMaxVoiceFrameBytes or the
  /// frame duration is 0.
  std::vector<std::uint8_t> packetize(std::span<const std::uint8_t> frame, bool last = false) {
    if (frameMs_ == 0 || frame.size() > kMaxVoiceFrameBytes) return {};
    VoiceHeader header;
    header.codec = codec_;
    header.seq = seq_;
    header.timestamp = timestamp_;
    header.frameMs = frameMs_;
    header.flags = static_cast<std::uint8_t>((spurtStart_ ? VoiceFlag::kSpurtStart : 0) |
                                             (last ? VoiceFlag::kSpurtEnd : 0));
    auto packet = encodeVoicePacket(header, frame);
    seq_ = static_cast<std::uint16_t>(seq_ + 1);
    timestamp_ += voiceSamplesPerFrame(codec_, frameMs_);
    spurtStart_ = last;
    return packet;
  }

  /// Silence: move the timestamp past `frames` frames that are not sent (the seq does
  /// not move), and start a talk spurt with the next packet. skip(0) only does the
  /// latter.
  void skip(std::uint32_t frames = 1) noexcept {
    timestamp_ += frames * voiceSamplesPerFrame(codec_, frameMs_);
    spurtStart_ = true;
  }

 private:
  std::uint8_t codec_;
  std::uint8_t frameMs_;
  std::uint16_t seq_;
  std::uint32_t timestamp_;
  bool spurtStart_ = true;
};

/// Options for VoiceJitterBuffer. Values outside their ranges are clamped:
/// targetDelayMs to 0 or more, maxFrames to 1-4096, resetAfterMs to above
/// targetDelayMs (CrowdyJS refuses them with a RangeError).
struct VoiceJitterBufferOptions {
  /// How long after the first packet of a talk spurt arrives it plays.
  std::int64_t targetDelayMs = kVoiceTargetDelayMs;
  /// The window of seqs one sender may have buffered, from the next one to play; a
  /// packet this far ahead of it or farther starts the stream over.
  int maxFrames = kVoiceJitterMaxFrames;
  /// A sender with no packet accepted for this long is silent: its stream stops
  /// reporting gaps, and the next packet starts it over.
  std::int64_t resetAfterMs = kVoiceResetAfterMs;
};

/// What VoiceJitterBuffer::push did with a packet.
enum class VoicePushResult : std::uint8_t { Buffered, Late, Duplicate, Malformed };

/// One played slot: a frame, or a gap where one never came.
struct VoicePlayout {
  /// The sender's key, as pushed.
  std::string key;
  std::uint16_t seq = 0;
  /// The frame's timestamp; for a gap, the one its frame would have had.
  std::uint32_t timestamp = 0;
  std::uint8_t codec = 0;
  std::uint8_t frameMs = 0;
  /// The frame's flags; 0 for a gap.
  std::uint8_t flags = 0;
  /// True when the frame is missing: conceal `frameMs` of audio (or play silence).
  bool gap = false;
  /// The codec frame (empty for a gap).
  std::vector<std::uint8_t> frame;
};

/// Puts each sender's voice packets back in order and plays them out after a fixed
/// delay. Feed every audio notification's payload to push() with the sender's key
/// (its actor uuid as a string), call pull() (or poll()) at least once a frame, and
/// forget() a sender that left; pass both the same monotonic clock. Pure and
/// clock-injected, so both SDKs replay the same fixtures. Single-threaded.
///
/// - A sender's stream starts with the first packet it accepts, which plays
///   targetDelayMs after it arrived; each later seq plays frameMs after the one
///   before it.
/// - pull() returns the slots that are due, in seq order: the frame, or a gap when it
///   is missing. After the last packet before silence (VoiceFlag::kSpurtEnd) the
///   stream reports nothing more. It reports slots only until resetAfterMs after the
///   last packet it accepted, and is then reset.
/// - A packet is late, and dropped, when its slot has passed or has been played. One
///   older than the first slot of a spurt that has not started playing is fitted in
///   front of it while its slot is still ahead.
/// - The stream starts over, dropping what it holds, on a packet that starts a talk
///   spurt and is newer than every packet it accepted; on a packet newer than the
///   spurt's last packet before silence; on a change of codec or frame duration; on a
///   packet maxFrames or more seqs ahead of the next one to play; and on any packet
///   after resetAfterMs without one accepted. A sender that restarts its seq lower is
///   dropped as late until then.
/// - A sender holds at most maxFrames frames.
///
/// The frame duration is fixed within a talk spurt, and so is the delay once the
/// spurt starts: a sender whose clock runs fast fills the window, one that runs slow
/// underruns, and both recover at the next spurt.
class VoiceJitterBuffer {
 public:
  explicit VoiceJitterBuffer(VoiceJitterBufferOptions options = {}) noexcept
      : targetDelayMs_(options.targetDelayMs < 0 ? 0 : options.targetDelayMs),
        maxFrames_(options.maxFrames < 1 ? 1 : (options.maxFrames > 4096 ? 4096 : options.maxFrames)),
        resetAfterMs_(options.resetAfterMs > targetDelayMs_ ? options.resetAfterMs
                                                            : targetDelayMs_ + 1) {}

  /// Add one packet from the sender `key`.
  VoicePushResult push(std::string_view key, std::span<const std::uint8_t> packet,
                       std::int64_t nowMs) {
    const auto parsed = decodeVoicePacket(packet);
    if (!parsed || parsed->header.frameMs == 0) {
      ++malformed;
      return VoicePushResult::Malformed;
    }
    const VoiceHeader& header = parsed->header;
    const std::string id(key);
    auto it = streams_.find(id);
    if (it != streams_.end() && startsOver(it->second, header, nowMs)) {
      discarded += it->second.frames.size();
      streams_.erase(it);
      it = streams_.end();
    }
    if (it == streams_.end()) {
      Stream stream;
      stream.codec = header.codec;
      stream.frameMs = header.frameMs;
      stream.nextSeq = header.seq;
      stream.nextAtMs = nowMs + targetDelayMs_;
      stream.nextTimestamp = header.timestamp;
      stream.highestSeq = header.seq;
      if ((header.flags & VoiceFlag::kSpurtEnd) != 0) stream.endSeq = header.seq;
      stream.lastArrivalMs = nowMs;
      stream.frames.emplace(header.seq, Held{header, {parsed->frame.begin(), parsed->frame.end()}});
      streams_.emplace(id, std::move(stream));
      return VoicePushResult::Buffered;
    }
    Stream& stream = it->second;
    const int ahead = voiceSeqDiff(header.seq, stream.nextSeq);
    const std::int64_t slotAtMs = stream.nextAtMs + static_cast<std::int64_t>(ahead) * stream.frameMs;
    const bool behind =
        ahead < 0 && (stream.started || voiceSeqDiff(stream.highestSeq, header.seq) >= maxFrames_);
    if (behind || slotAtMs < nowMs) {
      ++late;
      return VoicePushResult::Late;
    }
    if (stream.frames.count(header.seq) != 0) {
      ++duplicates;
      return VoicePushResult::Duplicate;
    }
    if (ahead < 0) {
      stream.nextSeq = header.seq;
      stream.nextAtMs = slotAtMs;
      stream.nextTimestamp = header.timestamp;
    }
    stream.frames.emplace(header.seq, Held{header, {parsed->frame.begin(), parsed->frame.end()}});
    stream.lastArrivalMs = nowMs;
    if (voiceSeqDiff(header.seq, stream.highestSeq) > 0) stream.highestSeq = header.seq;
    if ((header.flags & VoiceFlag::kSpurtEnd) != 0 && !stream.endSeq) stream.endSeq = header.seq;
    return VoicePushResult::Buffered;
  }

  /// The sender's slots due by `nowMs`, in seq order.
  std::vector<VoicePlayout> pull(std::string_view key, std::int64_t nowMs) {
    std::vector<VoicePlayout> out;
    auto it = streams_.find(std::string(key));
    if (it == streams_.end()) return out;
    Stream& stream = it->second;
    const std::int64_t silentAtMs = stream.lastArrivalMs + resetAfterMs_;
    while (!stream.ended && stream.nextAtMs <= nowMs && stream.nextAtMs < silentAtMs) {
      const std::uint16_t seq = stream.nextSeq;
      VoicePlayout slot;
      slot.key = it->first;
      slot.seq = seq;
      auto held = stream.frames.find(seq);
      if (held != stream.frames.end()) {
        slot.timestamp = held->second.header.timestamp;
        slot.codec = held->second.header.codec;
        slot.frameMs = held->second.header.frameMs;
        slot.flags = held->second.header.flags;
        slot.frame = std::move(held->second.frame);
        stream.nextTimestamp = held->second.header.timestamp;
        stream.frames.erase(held);
      } else {
        ++gaps;
        slot.timestamp = stream.nextTimestamp;
        slot.codec = stream.codec;
        slot.frameMs = stream.frameMs;
        slot.gap = true;
      }
      out.push_back(std::move(slot));
      stream.started = true;
      if (stream.endSeq && *stream.endSeq == seq) stream.ended = true;
      stream.nextSeq = static_cast<std::uint16_t>(seq + 1);
      stream.nextAtMs += stream.frameMs;
      stream.nextTimestamp += voiceSamplesPerFrame(stream.codec, stream.frameMs);
    }
    if (nowMs >= silentAtMs) {
      discarded += stream.frames.size();
      streams_.erase(it);
    }
    return out;
  }

  /// Every sender's due slots (pull() for each); senders in no particular order.
  std::vector<VoicePlayout> poll(std::int64_t nowMs) {
    std::vector<std::string> keys;
    keys.reserve(streams_.size());
    for (const auto& entry : streams_) keys.push_back(entry.first);
    std::vector<VoicePlayout> out;
    for (const std::string& key : keys) {
      auto slots = pull(key, nowMs);
      out.insert(out.end(), std::make_move_iterator(slots.begin()),
                 std::make_move_iterator(slots.end()));
    }
    return out;
  }

  /// Drop a sender's stream (it left).
  void forget(std::string_view key) {
    auto it = streams_.find(std::string(key));
    if (it == streams_.end()) return;
    discarded += it->second.frames.size();
    streams_.erase(it);
  }

  /// Senders with a stream.
  std::size_t senderCount() const noexcept { return streams_.size(); }

  /// Frames the sender has buffered.
  std::size_t bufferedCount(std::string_view key) const {
    auto it = streams_.find(std::string(key));
    return it == streams_.end() ? 0 : it->second.frames.size();
  }

  /// Packets dropped because their slot had passed.
  std::uint64_t late = 0;
  /// Packets dropped because their seq was already buffered.
  std::uint64_t duplicates = 0;
  /// Packets refused as not voice v1 (or with a frame duration of 0).
  std::uint64_t malformed = 0;
  /// Buffered frames thrown away unplayed (a stream started over, went silent, or
  /// was forgotten).
  std::uint64_t discarded = 0;
  /// Slots reported as gaps.
  std::uint64_t gaps = 0;

 private:
  struct Held {
    VoiceHeader header;
    std::vector<std::uint8_t> frame;
  };
  struct Stream {
    std::uint8_t codec = 0;
    std::uint8_t frameMs = 0;
    std::uint16_t nextSeq = 0;
    std::int64_t nextAtMs = 0;           ///< when nextSeq plays
    std::uint32_t nextTimestamp = 0;     ///< the timestamp nextSeq is expected to carry
    bool started = false;                ///< a slot has played in this spurt
    std::uint16_t highestSeq = 0;
    std::optional<std::uint16_t> endSeq; ///< the accepted last-before-silence packet
    bool ended = false;                  ///< endSeq has played
    std::int64_t lastArrivalMs = 0;
    std::unordered_map<std::uint16_t, Held> frames;
  };

  bool startsOver(const Stream& stream, const VoiceHeader& header, std::int64_t nowMs) const {
    if (nowMs - stream.lastArrivalMs >= resetAfterMs_) return true;
    if (header.codec != stream.codec || header.frameMs != stream.frameMs) return true;
    if ((header.flags & VoiceFlag::kSpurtStart) != 0 &&
        voiceSeqDiff(header.seq, stream.highestSeq) > 0)
      return true;
    if (stream.endSeq && voiceSeqDiff(header.seq, *stream.endSeq) > 0) return true;
    return voiceSeqDiff(header.seq, stream.nextSeq) >= maxFrames_;
  }

  std::int64_t targetDelayMs_;
  int maxFrames_;
  std::int64_t resetAfterMs_;
  std::unordered_map<std::string, Stream> streams_;
};

}  // namespace crowdy::media
