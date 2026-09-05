#include "CrowdyCppBridge.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/core/logger.hpp"
THIRD_PARTY_INCLUDES_END

// Routes CrowdyCPP log output onto the LogCrowdyCpp category. Replaces the
// vendored stderr logger (src/core/logger.cpp is not compiled into the bridge).
namespace crowdy::core
{
	namespace
	{
		class FUnrealLogger final : public ILogger
		{
		public:
			bool enabled(LogLevel Level) const override
			{
				switch (Level)
				{
				case LogLevel::Trace:
				case LogLevel::Debug: return UE_LOG_ACTIVE(LogCrowdyCpp, Verbose);
				case LogLevel::Info:  return UE_LOG_ACTIVE(LogCrowdyCpp, Log);
				case LogLevel::Warn:  return UE_LOG_ACTIVE(LogCrowdyCpp, Warning);
				case LogLevel::Error: return UE_LOG_ACTIVE(LogCrowdyCpp, Error);
				case LogLevel::Off:
				default: return false;
				}
			}

			void log(LogLevel Level, std::string_view Message) const override
			{
				if (!enabled(Level))
				{
					return;
				}

				// Message is not guaranteed null-terminated; convert length-bounded.
				const auto Converted = StringCast<TCHAR>(Message.data(), static_cast<int32>(Message.size()));
				const TCHAR* Text = Converted.Get();

				switch (Level)
				{
				case LogLevel::Trace:
				case LogLevel::Debug: UE_LOG(LogCrowdyCpp, Verbose, TEXT("%s"), Text); break;
				case LogLevel::Info:  UE_LOG(LogCrowdyCpp, Log, TEXT("%s"), Text); break;
				case LogLevel::Warn:  UE_LOG(LogCrowdyCpp, Warning, TEXT("%s"), Text); break;
				case LogLevel::Error: UE_LOG(LogCrowdyCpp, Error, TEXT("%s"), Text); break;
				case LogLevel::Off:
				default: break;
				}
			}
		};
	}

	const ILogger& defaultLogger()
	{
		static FUnrealLogger Logger;
		return Logger;
	}
}
