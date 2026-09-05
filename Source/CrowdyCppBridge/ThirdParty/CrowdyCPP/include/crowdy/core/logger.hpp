#pragma once

#include <string_view>

/// Pluggable logging. The default logger writes WARN and ERROR to stderr and
/// drops the rest; engine wrappers adapt this onto their own log sink.
/// The replication hot path never formats log strings unless the level is
/// enabled.
namespace crowdy::core {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

class ILogger {
 public:
  virtual ~ILogger() = default;
  virtual bool enabled(LogLevel level) const = 0;
  virtual void log(LogLevel level, std::string_view message) const = 0;
};

const ILogger& defaultLogger();

}  // namespace crowdy::core
