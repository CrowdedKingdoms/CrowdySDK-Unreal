#include "CrowdyCppBridge.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/client.hpp"
#include "crowdy/core/base64.hpp"
#include "crowdy/core/clock.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/core/uuid.hpp"
#include "crowdy/generated/operations.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"
THIRD_PARTY_INCLUDES_END

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
	// Never actually invoked by the self test (no send() is issued); it only
	// stands in for the injected Unreal transport so client construction has no
	// null-transport edge to hit.
	class FInertTransport final : public crowdy::graphql::IHttpTransport
	{
	public:
		crowdy::graphql::HttpResponse send(const crowdy::graphql::HttpRequest&) override
		{
			return crowdy::graphql::HttpResponse{200, std::string()};
		}
	};

	// Every Game Model operation the plugin sends. Each must resolve to a
	// non-empty document; an empty one means the vendored library no longer
	// carries that operation, which would otherwise surface as a server-side
	// syntax error at runtime rather than at check time.
	const char* const GameModelOperations[] = {
		"GameModelContainerState",
		"GameModelContainerStates",
		"GameModelContainers",
		"GameModelEnsureContainer",
		"GameModelCreateContainer",
		"GameModelInvoke",
		"GameModelCreateSession",
		"GameModelJoinSession",
		"GameModelSetSessionTurn",
		"GameModelSessions",
		"GameModelSession",
		"GameModelLeaveSession",
		"GameModelSetSessionAdmission",
		"GameModelTransferSessionHost",
		"GameModelEndSession",
		"GameModelSessionSnapshot",
		"GameModelSessionEvents",
		"GameModelSetProperty",
		"GameModelAddEdge",
		"GameModelDeleteEdge",
		"GameModelDeleteContainer",
		"GameModelTraverse",
		"GameModelSeed",
		"GameModelUpsertAutomation",
		"GameModelUpsertAutomationTrigger"
	};
}

bool FCrowdyCppBridge::SelfTest()
{
	try
	{
		// Core: base64 round trip, compared by content so a provider that returns
		// the right length but the wrong bytes still fails.
		const std::array<std::uint8_t, 5> payload{0x43, 0x72, 0x6f, 0x77, 0x64};
		const std::string encoded = crowdy::core::base64Encode(crowdy::Bytes(payload.data(), payload.size()));
		const auto decoded = crowdy::core::base64Decode(encoded);
		const bool bBase64Ok = decoded.has_value() && decoded->size() == payload.size()
			&& std::equal(payload.begin(), payload.end(), decoded->begin());

		// Crypto hook. Ask the provider whether it is available rather than
		// inferring it from a result: the convenience UUID helper returns an
		// all-zero value of the correct length when crypto is dead, so a length
		// check alone reports success on a build whose signing is broken.
		const bool bCryptoAvailable = crowdy::core::defaultCrypto().availability().ok();
		const crowdy::Result<crowdy::core::ActorUuid> uuid = crowdy::core::tryGenerateActorUuid();
		const bool bUuidOk = uuid.ok() && crowdy::core::toString(uuid.value()).size() == 32;

		// RTTI. The library recovers typed GraphQL errors by downcasting from
		// std::exception; without RTTI that cast compiles (with a warning this
		// module does not promote) and then always fails at runtime, silently
		// degrading every error message. This mirrors the library's own cast.
		const crowdy::graphql::CrowdyNetworkError probeError("self test");
		const std::exception& probeAsBase = probeError;
		const bool bRttiOk =
			dynamic_cast<const crowdy::graphql::CrowdyError*>(&probeAsBase) != nullptr;

		// Clock hook: ISO-8601 parse.
		const char kIso[] = "2026-07-21T00:00:00.000Z";
		const std::int64_t millis = crowdy::core::parseIso8601Millis(kIso, sizeof(kIso) - 1);
		const bool bClockOk = millis > 0 && crowdy::core::systemClock().monotonicMillis() >= 0;

		// Every operation the plugin sends still resolves to a document.
		bool bDocumentsOk = true;
		for (const char* const OperationName : GameModelOperations)
		{
			if (crowdy::gen::gameModel::documentFor(OperationName).empty())
			{
				UE_LOG(LogCrowdyCpp, Error,
					TEXT("CrowdyCPP self test: no document for operation '%hs'"), OperationName);
				bDocumentsOk = false;
			}
		}

		// Client + domains + GraphQL: construct and dispose with an inert
		// injected transport (no network I/O occurs).
		crowdy::ClientConfig config;
		config.httpUrl = "https://example.invalid";
		config.transport = std::make_shared<FInertTransport>();
		config.crypto = &crowdy::core::opensslCrypto();
		crowdy::CrowdyClient client(std::move(config));
		client.close();

		const bool bOk = bBase64Ok && bCryptoAvailable && bUuidOk && bRttiOk && bClockOk && bDocumentsOk;
		UE_LOG(LogCrowdyCpp, Log,
			TEXT("CrowdyCPP self test: base64=%d crypto=%d uuid=%d rtti=%d clock=%d documents=%d client=constructed"),
			bBase64Ok ? 1 : 0, bCryptoAvailable ? 1 : 0, bUuidOk ? 1 : 0, bRttiOk ? 1 : 0,
			bClockOk ? 1 : 0, bDocumentsOk ? 1 : 0);
		return bOk;
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("CrowdyCPP self test threw: %hs"), Ex.what());
		return false;
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("CrowdyCPP self test threw a non-standard exception"));
		return false;
	}
}
