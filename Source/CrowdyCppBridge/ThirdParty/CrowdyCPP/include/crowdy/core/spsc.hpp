#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

/// Bounded single-producer / single-consumer lock-free ring.
///
/// Used to hand parsed notifications from the network thread to the game
/// thread without locks or steady-state allocation. Capacity is fixed at
/// construction and rounded up to a power of two.
namespace crowdy::core {

template <typename T>
class SpscRing {
 public:
  explicit SpscRing(std::size_t capacity) {
    std::size_t cap = 1;
    while (cap < capacity) cap <<= 1;
    mask_ = cap - 1;
    slots_.resize(cap);
  }

  /// Producer side. Returns false when the ring is full (item not moved).
  bool tryPush(T& item) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail > mask_) return false;
    slots_[head & mask_] = std::move(item);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Consumer side.
  std::optional<T> tryPop() {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return std::nullopt;
    T item = std::move(slots_[tail & mask_]);
    tail_.store(tail + 1, std::memory_order_release);
    return item;
  }

  std::size_t sizeApprox() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }

 private:
  std::vector<T> slots_;
  std::size_t mask_ = 0;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace crowdy::core
