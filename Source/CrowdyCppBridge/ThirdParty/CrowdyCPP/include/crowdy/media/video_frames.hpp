#pragma once
// Webcam video fragments: the SDK-side half of ClientVideoPacket (143).
//
// A Buddy datagram carries at most 1232 bytes, and a long-spatial payload at
// most 1123 of them (1117 once the HMAC tail is counted). Even a 64x64 JPEG is
// 2-6 KB, so a frame crosses as several packets. The server never looks inside
// a video payload; THIS is the contract both SDKs implement byte for byte
// (published under "Wire formats" on docs.crowdedkingdoms.com; CrowdyJS
// src/media/video-frames.ts is the reference, and the seven fixture cases in
// tests/video_frames_test.cpp mirror its unit test):
//
//   offset  size  field       meaning
//   0       1     version     0x01. Anything else: drop the fragment.
//   1       1     codec       0 = JPEG, 1 = WebP. Others reserved; drop.
//   2       2     frameId     uint16 big-endian, per sender, +1 per frame, wraps.
//   4       1     fragIndex   0-based index of this fragment within the frame.
//   5       1     fragCount   total fragments in the frame, 1..16.
//   6       ...   body        this fragment's slice of the encoded frame.
//
// Reassembly is per (sender uuid, frameId): a frame is delivered once when every
// index has arrived; an incomplete frame is dropped when a NEWER frameId from the
// same sender arrives, or after kVideoFrameTimeoutMs without progress. No
// retransmit, no NACK -- the next frame is the recovery. Header-only, no
// networking, so it is unit-testable and usable from any engine thread.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "crowdy/core/uuid.hpp"

namespace crowdy::media {

/// Bytes of fragment header in front of every body slice.
constexpr std::size_t kVideoFragmentHeaderBytes = 6;
/// Largest body a fragment may carry with the HMAC tail present (1123 - 6).
constexpr std::size_t kMaxVideoFragmentBodyBytes = 1117;
/// A frame is refused above this many fragments (~17.8 KB).
constexpr std::size_t kMaxVideoFragments = 16;
/// An incomplete frame is abandoned after this long without a new fragment.
constexpr std::int64_t kVideoFrameTimeoutMs = 500;
/// The one header version this SDK writes and accepts.
constexpr std::uint8_t kVideoFragmentVersion = 1;

enum class VideoCodec : std::uint8_t { Jpeg = 0, WebP = 1 };

/// FNV-1a over the 32 uuid bytes (the same shape session::ActorUuidHash uses;
/// duplicated so this header depends on core only).
struct ActorUuidHash {
  std::size_t operator()(const core::ActorUuid& u) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (char c : u) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
  }
};

struct VideoFragmentHeader {
  std::uint8_t version;
  std::uint8_t codec;
  std::uint16_t frameId;
  std::uint8_t fragIndex;
  std::uint8_t fragCount;
};

/// A frame the assembler completed. `bytes` is owned by the caller after return.
struct AssembledVideoFrame {
  core::ActorUuid uuid{};
  std::uint16_t frameId = 0;
  VideoCodec codec = VideoCodec::Jpeg;
  std::vector<std::uint8_t> bytes;
  /// When the LAST fragment arrived (the caller's clock).
  std::int64_t completedAtMs = 0;
};

/// Parse a fragment header. Returns nullopt for anything this SDK must drop: too
/// short, wrong version, reserved codec, fragCount outside 1..16, or an index at
/// or past the count.
inline std::optional<VideoFragmentHeader> parseVideoFragmentHeader(
    std::span<const std::uint8_t> packet) noexcept {
  if (packet.size() < kVideoFragmentHeaderBytes) return std::nullopt;
  VideoFragmentHeader h;
  h.version = packet[0];
  h.codec = packet[1];
  h.frameId = static_cast<std::uint16_t>((static_cast<std::uint16_t>(packet[2]) << 8) | packet[3]);
  h.fragIndex = packet[4];
  h.fragCount = packet[5];
  if (h.version != kVideoFragmentVersion) return std::nullopt;
  if (h.codec != static_cast<std::uint8_t>(VideoCodec::Jpeg) &&
      h.codec != static_cast<std::uint8_t>(VideoCodec::WebP))
    return std::nullopt;
  if (h.fragCount < 1 || h.fragCount > kMaxVideoFragments) return std::nullopt;
  if (h.fragIndex >= h.fragCount) return std::nullopt;
  return h;
}

/// True when `a` is newer than `b` on the wrapping uint16 frameId counter.
inline constexpr bool isNewerFrameId(std::uint16_t a, std::uint16_t b) noexcept {
  const std::uint16_t d = static_cast<std::uint16_t>(a - b);
  return d != 0 && d < 0x8000;
}

/// Split one encoded frame into fragments, each `header || slice`. Returns an
/// empty vector when the frame is empty or would need more than
/// kMaxVideoFragments fragments (send a smaller frame; a partial frame is never
/// sent) -- callers check `.empty()` rather than catching.
inline std::vector<std::vector<std::uint8_t>> fragmentFrame(
    std::span<const std::uint8_t> frame, std::uint16_t frameId,
    VideoCodec codec = VideoCodec::Jpeg, std::size_t maxBody = kMaxVideoFragmentBodyBytes) {
  std::vector<std::vector<std::uint8_t>> out;
  if (frame.empty() || maxBody < 1 || maxBody > kMaxVideoFragmentBodyBytes) return out;
  const std::size_t fragCount = (frame.size() + maxBody - 1) / maxBody;
  if (fragCount > kMaxVideoFragments) return out;
  out.reserve(fragCount);
  for (std::size_t i = 0; i < fragCount; ++i) {
    const std::size_t begin = i * maxBody;
    const std::size_t len = std::min(maxBody, frame.size() - begin);
    std::vector<std::uint8_t> packet(kVideoFragmentHeaderBytes + len);
    packet[0] = kVideoFragmentVersion;
    packet[1] = static_cast<std::uint8_t>(codec);
    packet[2] = static_cast<std::uint8_t>(frameId >> 8);
    packet[3] = static_cast<std::uint8_t>(frameId & 0xff);
    packet[4] = static_cast<std::uint8_t>(i);
    packet[5] = static_cast<std::uint8_t>(fragCount);
    std::memcpy(packet.data() + kVideoFragmentHeaderBytes, frame.data() + begin, len);
    out.push_back(std::move(packet));
  }
  return out;
}

/// Reassembles fragments per sender. Feed every ClientVideoNotification's payload
/// to ingest(); call prune() on a timer (or rely on the newer-frame rule alone)
/// and forget() when a sender leaves (an actorLeft is the natural trigger).
class VideoFrameAssembler {
 public:
  explicit VideoFrameAssembler(std::int64_t timeoutMs = kVideoFrameTimeoutMs)
      : timeoutMs_(timeoutMs) {}

  /// Add one fragment. Returns the completed frame when this fragment finished it.
  std::optional<AssembledVideoFrame> ingest(const core::ActorUuid& uuid,
                                            std::span<const std::uint8_t> packet,
                                            std::int64_t nowMs) {
    const auto header = parseVideoFragmentHeader(packet);
    if (!header) {
      ++dropped;
      return std::nullopt;
    }
    auto it = pending_.find(uuid);
    if (it == pending_.end()) {
      const auto done = lastDone_.find(uuid);
      if (done != lastDone_.end() && !isNewerFrameId(header->frameId, done->second)) {
        ++dropped;  // a straggler from a frame already completed or abandoned
        return std::nullopt;
      }
    } else if (it->second.frameId != header->frameId) {
      if (isNewerFrameId(header->frameId, it->second.frameId)) {
        ++abandoned;  // the sender moved on; the old frame will never complete
        lastDone_[uuid] = it->second.frameId;
        pending_.erase(it);
        it = pending_.end();
      } else {
        ++dropped;
        return std::nullopt;
      }
    }
    if (it != pending_.end() &&
        (it->second.fragCount != header->fragCount || it->second.codec != header->codec)) {
      ++abandoned;  // same frameId, different shape: corrupt or a very fast wrap
      pending_.erase(it);
      it = pending_.end();
    }
    if (it == pending_.end()) {
      Pending p;
      p.frameId = header->frameId;
      p.codec = header->codec;
      p.fragCount = header->fragCount;
      p.parts.resize(header->fragCount);
      p.have.assign(header->fragCount, false);
      it = pending_.emplace(uuid, std::move(p)).first;
    }
    Pending& frame = it->second;
    if (!frame.have[header->fragIndex]) {
      frame.parts[header->fragIndex].assign(packet.begin() + kVideoFragmentHeaderBytes,
                                            packet.end());
      frame.have[header->fragIndex] = true;
      ++frame.received;
    }
    frame.lastAtMs = nowMs;
    if (frame.received < frame.fragCount) return std::nullopt;

    AssembledVideoFrame out;
    out.uuid = uuid;
    out.frameId = frame.frameId;
    out.codec = static_cast<VideoCodec>(frame.codec);
    out.completedAtMs = nowMs;
    std::size_t total = 0;
    for (const auto& part : frame.parts) total += part.size();
    out.bytes.reserve(total);
    for (const auto& part : frame.parts) out.bytes.insert(out.bytes.end(), part.begin(), part.end());
    lastDone_[uuid] = frame.frameId;
    pending_.erase(it);
    return out;
  }

  /// Abandon frames that have not progressed within the timeout. Returns how many.
  std::size_t prune(std::int64_t nowMs) {
    std::size_t n = 0;
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (nowMs - it->second.lastAtMs > timeoutMs_) {
        lastDone_[it->first] = it->second.frameId;
        it = pending_.erase(it);
        ++abandoned;
        ++n;
      } else {
        ++it;
      }
    }
    return n;
  }

  /// Drop any partial frame for a sender that left, and forget its counter.
  void forget(const core::ActorUuid& uuid) {
    if (pending_.erase(uuid) > 0) ++abandoned;
    lastDone_.erase(uuid);
  }

  /// Senders with a frame in progress.
  std::size_t pendingCount() const noexcept { return pending_.size(); }

  /// Fragments dropped for a malformed header or a stale frameId.
  std::uint64_t dropped = 0;
  /// Frames abandoned incomplete (newer frame arrived, timeout, or forget).
  std::uint64_t abandoned = 0;

 private:
  struct Pending {
    std::uint16_t frameId = 0;
    std::uint8_t codec = 0;
    std::uint8_t fragCount = 0;
    std::uint8_t received = 0;
    std::int64_t lastAtMs = 0;
    std::vector<std::vector<std::uint8_t>> parts;
    std::vector<bool> have;
  };
  std::int64_t timeoutMs_;
  std::unordered_map<core::ActorUuid, Pending, ActorUuidHash> pending_;
  std::unordered_map<core::ActorUuid, std::uint16_t, ActorUuidHash> lastDone_;
};

}  // namespace crowdy::media
