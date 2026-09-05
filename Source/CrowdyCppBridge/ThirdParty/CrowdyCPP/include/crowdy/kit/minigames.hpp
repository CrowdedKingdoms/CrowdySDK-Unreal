#pragma once

#include <string>

#include "crowdy/kit/engine.hpp"

/// A thin invoke wrapper for invoke-loop minigames (the Wave 2 `minigame`
/// scaffold pattern). Denials resolve as `{success=false, reason}` — no
/// exceptions. Mirrors CrowdyJS's kit.minigames.
namespace crowdy::kit {

class MinigamesKit {
 public:
  MinigamesKit(std::string appId, EngineDetector& engines, std::string defaultModuleName = {})
      : appId_(std::move(appId)),
        engines_(engines),
        defaultModuleName_(std::move(defaultModuleName)) {}

  /// Is a minigame module deployed + enabled (cached per client)?
  bool engineAvailable(std::string_view moduleName = {}) {
    const std::string name = resolve(moduleName);
    return !name.empty() && engines_.has(name);
  }

  /// One round: the game's `play` export with your move.
  EngineInvokeResult play(const graphql::JVal& params, std::string_view moduleName = {}) {
    return invoke("play", params, moduleName);
  }

  /// Your per-caller record.
  EngineInvokeResult record(std::string_view moduleName = {}) {
    return invoke("record", graphql::JVal(), moduleName);
  }

  /// Any other export the game defines.
  EngineInvokeResult invoke(std::string_view exportName, const graphql::JVal& params,
                            std::string_view moduleName = {}) {
    const std::string name = resolve(moduleName);
    if (name.empty()) {
      EngineInvokeResult out;
      out.reason = "no minigame module configured";
      return out;
    }
    return engines_.invoke(name, exportName, params);
  }

 private:
  std::string resolve(std::string_view moduleName) const {
    return moduleName.empty() ? defaultModuleName_ : std::string(moduleName);
  }

  std::string appId_;
  EngineDetector& engines_;
  std::string defaultModuleName_;
};

}  // namespace crowdy::kit
