#pragma once

// This header names CrowdyCPP types, so it includes the CrowdyCPP header
// directly (no UE THIRD_PARTY guard, which would not be defined when this
// private header is the first include in a bridge translation unit). http.hpp
// pulls no UE or winsock headers, so plain inclusion is safe.
#include "crowdy/graphql/http.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Templates/SharedPointer.h"

class IHttpRequest;

// The bridge's HTTP transports for the asynchronous API path. The GraphQL client drives an injected
// IAsyncHttpTransport, so the only thing it needs from the engine is an HTTP stack behind this interface.
// This header is private to the bridge because it names third-party types; dependent modules never see it.
namespace CrowdyCppTransport
{
	// Diagnostic totals. Atomic because a completion is not guaranteed to run on the thread that reads them.
	struct FTransportCounters
	{
		std::atomic<int64_t> RequestBytes{0};
		std::atomic<int64_t> ResponseBytes{0};
		std::atomic<int32_t> Responses{0};
		std::atomic<int32_t> InFlight{0};
		std::atomic<int32_t> PeakInFlight{0};

		void BeginFlight()
		{
			const int32_t Now = InFlight.fetch_add(1, std::memory_order_relaxed) + 1;
			int32_t Peak = PeakInFlight.load(std::memory_order_relaxed);
			while (Now > Peak && !PeakInFlight.compare_exchange_weak(Peak, Now, std::memory_order_relaxed))
			{
			}
		}

		void EndFlight() { InFlight.fetch_sub(1, std::memory_order_relaxed); }
	};

	// Marks the request the calling thread is issuing, for as long as the scope lives, so the transport can record
	// into InStampSeconds the moment it hands that request to the HTTP module. Scopes nest.
	class FHandOffScope
	{
	public:
		explicit FHandOffScope(double& InStampSeconds);
		~FHandOffScope();
		FHandOffScope(const FHandOffScope&) = delete;
		FHandOffScope& operator=(const FHandOffScope&) = delete;

	private:
		double* Previous = nullptr;
	};

	// Writes the current time into the calling thread's marked request, if one is marked.
	void StampHandOff();

	// The engine request the async transport sends for Request, configured but not started; asks for HTTP/2 while
	// crowdy.net.http2 is on. Game thread only. The caller rejects a body larger than MAX_int32 first.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> BuildRequest(const crowdy::graphql::HttpRequest& Request);

	// Async transport over Unreal's FHttpModule. sendAsync starts a request and
	// invokes the callback from OnProcessRequestComplete (already on the game
	// thread); the GraphQL client then routes it through the client's Dispatcher
	// so the callback lands wherever poll() is pumped. Counters, when given, receive every body sent and received.
	std::shared_ptr<crowdy::graphql::IAsyncHttpTransport> MakeFHttpTransport(
		std::shared_ptr<FTransportCounters> Counters = nullptr);

	// What a canned transport was last asked to send. Lets a test assert the
	// endpoint a call was routed to and the bearer it would have carried, which
	// are otherwise only observable against a live server.
	struct FCannedRequestCapture
	{
		bool bHasRequest = false;
		std::string Url;
		std::string Authorization;

		// When non-empty, each request consumes the next entry as its (status, body) instead of the
		// transport's fixed response; requests past the end of the script get the fixed response. Lets
		// one test drive a sequence such as a redirect followed by the retried call's answer.
		std::vector<std::pair<int, std::string>> ScriptedResponses;
		std::size_t NextScriptedResponse = 0;

		// Runs while a request is in flight, after it was captured and before its response is
		// delivered. A test uses it to act as a concurrent caller - for example moving the client's
		// endpoint mid-request, the way a parallel request's datacenter redirect would.
		std::function<void(const std::string& Url)> OnRequest;

		// The request body last sent, and whether each request stamps its hand-off the way the real transport does.
		std::string Body;
		bool bStampHandOff = false;
	};

	// Test transport: every request resolves to a fixed canned response with no
	// network I/O, running the real interpret() response path. Used to prove
	// parity against FCrowdyGameApiCodec headlessly. When Capture is set, each
	// request overwrites it before the canned response is returned. Counters, when given, count requests in flight
	// and nothing else.
	std::shared_ptr<crowdy::graphql::IAsyncHttpTransport> MakeCannedTransport(std::string Body, int Status,
		std::shared_ptr<FCannedRequestCapture> Capture = nullptr, std::shared_ptr<FTransportCounters> Counters = nullptr);
}
