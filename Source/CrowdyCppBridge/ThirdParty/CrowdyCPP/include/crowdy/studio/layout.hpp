#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "crowdy/graphql/json.hpp"

namespace crowdy::studio {

enum class StudioPaneId : std::uint8_t {
  Explorer,
  Settings,
  Agent,
  Bottom,
};

struct StudioPaneSizeRange {
  int min = 0;
  int max = 0;
  bool operator==(const StudioPaneSizeRange&) const = default;
};

inline constexpr std::string_view STUDIO_LAYOUT_STORAGE_KEY =
    "ck:crowdy-studio:layout:v1";

inline constexpr std::array<StudioPaneId, 4> STUDIO_PANE_IDS = {
    StudioPaneId::Explorer,
    StudioPaneId::Settings,
    StudioPaneId::Agent,
    StudioPaneId::Bottom,
};

inline constexpr std::size_t studioPaneIndex(StudioPaneId pane) noexcept {
  return static_cast<std::size_t>(pane);
}

inline constexpr std::string_view toString(StudioPaneId pane) noexcept {
  switch (pane) {
    case StudioPaneId::Explorer: return "explorer";
    case StudioPaneId::Settings: return "settings";
    case StudioPaneId::Agent: return "agent";
    case StudioPaneId::Bottom: return "bottom";
  }
  return "";
}

inline constexpr std::optional<StudioPaneId> studioPaneIdFromString(
    std::string_view pane) noexcept {
  if (pane == "explorer") return StudioPaneId::Explorer;
  if (pane == "settings") return StudioPaneId::Settings;
  if (pane == "agent") return StudioPaneId::Agent;
  if (pane == "bottom") return StudioPaneId::Bottom;
  return std::nullopt;
}

inline constexpr StudioPaneSizeRange studioPaneSizeRange(
    StudioPaneId pane) noexcept {
  switch (pane) {
    case StudioPaneId::Explorer: return {160, 480};
    case StudioPaneId::Settings: return {220, 480};
    case StudioPaneId::Agent: return {280, 620};
    case StudioPaneId::Bottom: return {96, 480};
  }
  return {};
}

inline constexpr bool defaultStudioPaneVisible(StudioPaneId pane) noexcept {
  return pane == StudioPaneId::Explorer;
}

inline constexpr int defaultStudioPaneSize(StudioPaneId pane) noexcept {
  switch (pane) {
    case StudioPaneId::Explorer: return 230;
    case StudioPaneId::Settings: return 280;
    case StudioPaneId::Agent: return 340;
    case StudioPaneId::Bottom: return 180;
  }
  return 0;
}

/**
 * Clamp and round exactly like CrowdyJS's clampStudioPaneSize().
 *
 * The range is positive, so std::round and JavaScript Math.round agree for
 * every value after clamping. Non-finite values restore the pane default.
 */
inline int clampStudioPaneSize(StudioPaneId pane, double size) noexcept {
  if (!std::isfinite(size)) return defaultStudioPaneSize(pane);
  const auto range = studioPaneSizeRange(pane);
  const double bounded =
      std::min(static_cast<double>(range.max),
               std::max(static_cast<double>(range.min), size));
  return static_cast<int>(std::round(bounded));
}

class StudioLayoutController;

class StudioLayoutState {
 public:
  using Visibility = std::array<bool, STUDIO_PANE_IDS.size()>;
  using Sizes = std::array<int, STUDIO_PANE_IDS.size()>;

  StudioLayoutState() = default;
  StudioLayoutState(Visibility visible, Sizes sizes)
      : visible_(std::move(visible)), sizes_(std::move(sizes)) {}

  const Visibility& visible() const noexcept { return visible_; }
  const Sizes& sizes() const noexcept { return sizes_; }

  bool isVisible(StudioPaneId pane) const noexcept {
    return visible_[studioPaneIndex(pane)];
  }

  int paneSize(StudioPaneId pane) const noexcept {
    return sizes_[studioPaneIndex(pane)];
  }

  bool operator==(const StudioLayoutState&) const = default;

 private:
  friend class StudioLayoutController;

  Visibility visible_ = {
      true,
      false,
      false,
      false,
  };
  Sizes sizes_ = {
      230,
      280,
      340,
      180,
  };
};

struct StudioLayoutDefaults {
  std::array<std::optional<bool>, STUDIO_PANE_IDS.size()> visible;
  std::array<std::optional<double>, STUDIO_PANE_IDS.size()> sizes;
};

/**
 * Injected persistence boundary. Native hosts choose where settings live;
 * CrowdyCPP never assumes a filesystem, registry, or process-global store.
 *
 * nullopt means absent or unavailable. setItem() returns false when the write
 * was unavailable. In exception-enabled builds the controller also catches
 * failures from a throwing host implementation so state remains session-usable.
 * Reduced no-exception packages require injected implementations not to throw,
 * like every other CrowdyCPP platform boundary.
 */
class ICrowdyStudioLayoutStorage {
 public:
  virtual ~ICrowdyStudioLayoutStorage() = default;
  virtual std::optional<std::string> getItem(std::string_view key) = 0;
  virtual bool setItem(std::string_view key, std::string_view value) = 0;
};

class InMemoryCrowdyStudioLayoutStorage final
    : public ICrowdyStudioLayoutStorage {
 public:
  std::optional<std::string> getItem(std::string_view key) override {
    const auto found = values_.find(key);
    if (found == values_.end()) return std::nullopt;
    return found->second;
  }

  bool setItem(std::string_view key, std::string_view value) override {
    values_.insert_or_assign(std::string(key), std::string(value));
    return true;
  }

  void erase(std::string_view key) {
    const auto found = values_.find(key);
    if (found != values_.end()) values_.erase(found);
  }
  void clear() noexcept { values_.clear(); }

 private:
  std::map<std::string, std::string, std::less<>> values_;
};

struct StudioLayoutControllerOptions {
  ICrowdyStudioLayoutStorage* storage = nullptr;
  std::string storageKey{STUDIO_LAYOUT_STORAGE_KEY};
  StudioLayoutDefaults defaults;
};

/**
 * Headless Crowdy Studio pane layout state machine.
 *
 * Returned state is always a value snapshot, and listeners receive a const
 * snapshot detached from controller storage. Mutations persist before
 * notifying, matching CrowdyJS.
 */
class StudioLayoutController {
 public:
  using ListenerId = std::uint64_t;
  using StudioLayoutListener =
      std::function<void(const StudioLayoutState& state)>;

  explicit StudioLayoutController(
      StudioLayoutControllerOptions options = {})
      : storage_(options.storage),
        storageKey_(std::move(options.storageKey)) {
    for (const StudioPaneId pane : STUDIO_PANE_IDS) {
      const auto index = studioPaneIndex(pane);
      if (options.defaults.visible[index]) {
        state_.visible_[index] = *options.defaults.visible[index];
      }
      if (options.defaults.sizes[index]) {
        state_.sizes_[index] =
            clampStudioPaneSize(pane, *options.defaults.sizes[index]);
      }
    }
    loadPersisted();
  }

  StudioLayoutState getState() const { return state_; }

  bool isVisible(StudioPaneId pane) const noexcept {
    return state_.isVisible(pane);
  }

  int paneSize(StudioPaneId pane) const noexcept {
    return state_.paneSize(pane);
  }

  ListenerId subscribe(StudioLayoutListener listener) {
    const ListenerId id = nextListenerId_++;
    listeners_.emplace(id, std::move(listener));
    return id;
  }

  void unsubscribe(ListenerId id) { listeners_.erase(id); }

  void setVisible(StudioPaneId pane, bool visible) {
    const auto index = studioPaneIndex(pane);
    if (state_.visible_[index] == visible) return;
    state_.visible_[index] = visible;
    persist();
    emit();
  }

  void toggle(StudioPaneId pane) {
    setVisible(pane, !isVisible(pane));
  }

  void setSize(StudioPaneId pane, double size, bool persistValue = true) {
    const auto index = studioPaneIndex(pane);
    const int next = clampStudioPaneSize(pane, size);
    if (state_.sizes_[index] == next) {
      if (persistValue) persist();
      return;
    }
    state_.sizes_[index] = next;
    if (persistValue) persist();
    emit();
  }

 private:
  void loadPersisted() {
    if (!storage_) return;
    std::optional<std::string> raw;
#ifndef CROWDY_NO_EXCEPTIONS
    try {
      raw = storage_->getItem(storageKey_);
    } catch (...) {
      return;
    }
#else
    raw = storage_->getItem(storageKey_);
#endif
    if (!raw || raw->empty()) return;

    const graphql::Json parsed = graphql::Json::parse(*raw);
    if (!parsed.isObject()) return;

    StudioLayoutState next = state_;
    const graphql::Json visible = parsed["visible"];
    const graphql::Json sizes = parsed["sizes"];
    for (const StudioPaneId pane : STUDIO_PANE_IDS) {
      const auto index = studioPaneIndex(pane);
      const auto key = toString(pane);
      const graphql::Json persistedVisible = visible[key];
      if (persistedVisible.isBool()) {
        next.visible_[index] = persistedVisible.asBool();
      }
      const graphql::Json persistedSize = sizes[key];
      if (persistedSize.isNumber()) {
        const double value = persistedSize.asDouble();
        if (std::isfinite(value)) {
          next.sizes_[index] = clampStudioPaneSize(pane, value);
        }
      }
    }
    state_ = next;
  }

  static std::string serialize(const StudioLayoutState& state) {
    std::string json;
    json.reserve(160);
    json += R"({"visible":{)";
    appendRecord(
        json, [&state](StudioPaneId pane) {
          return state.isVisible(pane) ? std::string_view("true")
                                       : std::string_view("false");
        });
    json += R"(},"sizes":{)";
    appendRecord(json, [&state](StudioPaneId pane) {
      return std::to_string(state.paneSize(pane));
    });
    json += "}}";
    return json;
  }

  template <typename Value>
  static void appendRecord(std::string& json, Value value) {
    bool first = true;
    for (const StudioPaneId pane : STUDIO_PANE_IDS) {
      if (!first) json.push_back(',');
      first = false;
      json.push_back('"');
      json += toString(pane);
      json += "\":";
      json += value(pane);
    }
  }

  void persist() {
    if (!storage_) return;
    const std::string json = serialize(state_);
#ifndef CROWDY_NO_EXCEPTIONS
    try {
      (void)storage_->setItem(storageKey_, json);
    } catch (...) {
      // A throwing platform store degrades to session-only layout state.
    }
#else
    (void)storage_->setItem(storageKey_, json);
#endif
  }

  void emit() {
    const StudioLayoutState snapshot = state_;
    const auto listeners = listeners_;
    for (const auto& [id, listener] : listeners) {
      (void)id;
      if (listener) listener(snapshot);
    }
  }

  ICrowdyStudioLayoutStorage* storage_ = nullptr;
  std::string storageKey_;
  StudioLayoutState state_;
  std::map<ListenerId, StudioLayoutListener> listeners_;
  ListenerId nextListenerId_ = 1;
};

}  // namespace crowdy::studio
