#include "Platform/CrowdyCppHttpTransport.h"

#include "CrowdyCppBridge.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HttpConstants.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Templates/SharedPointer.h"

#include <functional>
#include <utility>

THIRD_PARTY_INCLUDES_START
#include "crowdy/core/result.hpp"
THIRD_PARTY_INCLUDES_END

namespace
{
	using crowdy::graphql::HttpOutcome;
	using crowdy::graphql::HttpRequest;
	using crowdy::graphql::HttpResponse;
	using crowdy::graphql::IAsyncHttpTransport;
	using crowdy::graphql::IHttpTransport;

	// A ceiling on the response body the transport will materialize. A malicious
	// or man-in-the-middle server can otherwise return an arbitrarily large body
	// and drive several coexisting multi-gigabyte copies (raw bytes, the GraphQL
	// parse, the decoded FString) to an out-of-memory crash. Generous for any
	// real GraphQL response; availability-only, never a truth-plane concern.
	constexpr int32 MaxResponseBytes = 64 * 1024 * 1024;

	TAutoConsoleVariable<int32> CVarHttp2(
		TEXT("crowdy.net.http2"), 1,
		TEXT("Asks for HTTP/2 on the SDK's own requests, falling back to HTTP/1.1 when the server does not offer it; 0 leaves the engine's HTTP version. With HTTP/2 the curl diagnostics the engine logs for a failed request can include the request headers, bearer token included, so set 0 if you ship logs from builds with logging enabled."),
		ECVF_Default);

	void ApplyRequestOptions(IHttpRequest& Request)
	{
		const int32 Http2 = CVarHttp2.GetValueOnGameThread();
		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			UE_LOG(LogCrowdyCpp, Log, TEXT("SDK requests %s (crowdy.net.http2 %d)."),
				Http2 != 0 ? TEXT("ask for HTTP/2") : TEXT("keep the engine's HTTP version"), Http2);
		}
		if (Http2 == 0)
		{
			return;
		}
		Request.SetOption(HttpRequestOptions::HttpVersion, FHttpConstants::VERSION_2TLS);
	}

	thread_local double* MarkedHandOff = nullptr;

	// FHttpModule-backed async transport. Requests are issued on the calling
	// thread (the game thread, where requestAsync runs) and FHttpModule delivers
	// OnProcessRequestComplete on the game thread too, so no cross-thread hop is
	// introduced here; the Dispatcher poll() in the client is the marshalling seam.
	class FUnrealAsyncHttpTransport final : public IAsyncHttpTransport
	{
	public:
		explicit FUnrealAsyncHttpTransport(std::shared_ptr<CrowdyCppTransport::FTransportCounters> InCounters)
			: Counters(std::move(InCounters)) {}

		void sendAsync(const HttpRequest& Request, std::function<void(HttpOutcome)> Cb) override
		{
			// The completion callback owns the std::function; a thread-safe shared
			// pointer keeps it alive until the request finishes and lets the
			// copyable UE delegate carry the move-only capture. Every exit path
			// invokes it exactly once so the GraphQL client always completes.
			const TSharedRef<std::function<void(HttpOutcome)>, ESPMode::ThreadSafe> Callback =
				MakeShared<std::function<void(HttpOutcome)>, ESPMode::ThreadSafe>(std::move(Cb));

			if (Request.body.size() > static_cast<size_t>(MAX_int32))
			{
				HttpOutcome Outcome;
				Outcome.status = crowdy::Errc::SocketError;
				Outcome.errorMessage = "request body exceeds the size limit";
				(*Callback)(std::move(Outcome));
				return;
			}

			const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = CrowdyCppTransport::BuildRequest(Request);
			if (Counters)
			{
				Counters->RequestBytes.fetch_add(static_cast<int64_t>(Request.body.size()), std::memory_order_relaxed);
			}

			// A start that fails can also fire the completion, so the flight is ended by whichever runs first.
			const std::shared_ptr<std::atomic<bool>> bLanded = std::make_shared<std::atomic<bool>>(false);
			auto Land = [SharedCounters = Counters, bLanded]()
			{
				if (SharedCounters && !bLanded->exchange(true))
				{
					SharedCounters->EndFlight();
				}
			};

			HttpRequest->OnProcessRequestComplete().BindLambda(
				[Callback, SharedCounters = Counters, Land](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
				{
					Land();
					HttpOutcome Outcome;
					if (bConnectedSuccessfully && Response.IsValid())
					{
						const TArray<uint8>& Payload = Response->GetContent();
						if (SharedCounters)
						{
							SharedCounters->ResponseBytes.fetch_add(Payload.Num(), std::memory_order_relaxed);
							SharedCounters->Responses.fetch_add(1, std::memory_order_relaxed);
						}
						if (Payload.Num() > MaxResponseBytes)
						{
							Outcome.status = crowdy::Errc::SocketError;
							Outcome.errorMessage = "HTTP response exceeds the size limit";
						}
						else
						{
							Outcome.status = crowdy::Errc::Ok;
							Outcome.response.status = Response->GetResponseCode();
							if (Payload.Num() > 0)
							{
								Outcome.response.body.assign(
									reinterpret_cast<const char*>(Payload.GetData()),
									static_cast<size_t>(Payload.Num()));
							}
						}
					}
					else
					{
						// FHttpModule does not surface a distinct timeout status here,
						// so every connection-level failure maps to SocketError; the
						// GraphQL client turns that into a Network-kind outcome.
						Outcome.status = crowdy::Errc::SocketError;
						Outcome.errorMessage = "HTTP request failed";
					}
					(*Callback)(std::move(Outcome));
				});

			if (Counters)
			{
				Counters->BeginFlight();
			}

			// A synchronous start failure does not always fire the completion delegate, so it is completed here too;
			// the GraphQL client delivers only the first of the two.
			if (!HttpRequest->ProcessRequest())
			{
				Land();
				HttpOutcome Outcome;
				Outcome.status = crowdy::Errc::SocketError;
				Outcome.errorMessage = "failed to start HTTP request";
				(*Callback)(std::move(Outcome));
				return;
			}
			// Only a request the HTTP module accepted is handed off; a failed start records no split.
			CrowdyCppTransport::StampHandOff();
		}

	private:
		std::shared_ptr<CrowdyCppTransport::FTransportCounters> Counters;
	};

	// Synchronous transport that returns a fixed response. Wrapped by the inline
	// async adapter so the canned path runs the identical interpret() logic as a
	// real round trip.
	class FCannedSyncTransport final : public IHttpTransport
	{
	public:
		FCannedSyncTransport(std::string InBody, int InStatus,
			std::shared_ptr<CrowdyCppTransport::FCannedRequestCapture> InCapture,
			std::shared_ptr<CrowdyCppTransport::FTransportCounters> InCounters)
			: Body(std::move(InBody)), Status(InStatus), Capture(std::move(InCapture)), Counters(std::move(InCounters)) {}

		HttpResponse send(const HttpRequest& Request) override
		{
			if (Counters)
			{
				Counters->BeginFlight();
			}
			HttpResponse Response = Respond(Request);
			if (Counters)
			{
				Counters->EndFlight();
			}
			return Response;
		}

	private:
		HttpResponse Respond(const HttpRequest& Request)
		{
			if (Capture)
			{
				if (Capture->bStampHandOff)
				{
					CrowdyCppTransport::StampHandOff();
				}
				Capture->bHasRequest = true;
				Capture->Url = Request.url;
				Capture->Body = Request.body;
				Capture->Authorization.clear();
				for (const auto& Header : Request.headers)
				{
					if (Header.first == "Authorization")
					{
						Capture->Authorization = Header.second;
						break;
					}
				}
				if (Capture->OnRequest)
				{
					Capture->OnRequest(Request.url);
				}
				if (Capture->NextScriptedResponse < Capture->ScriptedResponses.size())
				{
					const std::pair<int, std::string>& Scripted =
						Capture->ScriptedResponses[Capture->NextScriptedResponse++];
					return HttpResponse{Scripted.first, Scripted.second};
				}
			}
			return HttpResponse{Status, Body};
		}

		std::string Body;
		int Status;
		std::shared_ptr<CrowdyCppTransport::FCannedRequestCapture> Capture;
		std::shared_ptr<CrowdyCppTransport::FTransportCounters> Counters;
	};
}

namespace CrowdyCppTransport
{
	FHandOffScope::FHandOffScope(double& InStampSeconds)
		: Previous(MarkedHandOff)
	{
		MarkedHandOff = &InStampSeconds;
	}

	FHandOffScope::~FHandOffScope()
	{
		MarkedHandOff = Previous;
	}

	void StampHandOff()
	{
		if (MarkedHandOff)
		{
			*MarkedHandOff = FPlatformTime::Seconds();
		}
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> BuildRequest(const crowdy::graphql::HttpRequest& Request)
	{
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Built = FHttpModule::Get().CreateRequest();
		ApplyRequestOptions(*Built);
		Built->SetURL(FString(UTF8_TO_TCHAR(Request.url.c_str())));
		Built->SetVerb(FString(UTF8_TO_TCHAR(Request.method.c_str())));
		for (const std::pair<std::string, std::string>& Header : Request.headers)
		{
			Built->SetHeader(FString(UTF8_TO_TCHAR(Header.first.c_str())), FString(UTF8_TO_TCHAR(Header.second.c_str())));
		}

		// Send the body as raw bytes so the UTF-8 request payload is never
		// re-encoded through an intermediate FString.
		TArray<uint8> Body;
		Body.Append(reinterpret_cast<const uint8*>(Request.body.data()), static_cast<int32>(Request.body.size()));
		Built->SetContent(MoveTemp(Body));

		// A non-positive timeout leaves FHttpModule's own default in place
		// rather than disabling the timeout (SetTimeout(0) would).
		if (Request.timeoutMs > 0)
		{
			Built->SetTimeout(static_cast<float>(Request.timeoutMs) / 1000.0f);
		}
		return Built;
	}

	std::shared_ptr<IAsyncHttpTransport> MakeFHttpTransport(std::shared_ptr<FTransportCounters> Counters)
	{
		return std::make_shared<FUnrealAsyncHttpTransport>(std::move(Counters));
	}

	std::shared_ptr<IAsyncHttpTransport> MakeCannedTransport(std::string Body, int Status,
		std::shared_ptr<FCannedRequestCapture> Capture, std::shared_ptr<FTransportCounters> Counters)
	{
		return crowdy::graphql::makeInlineAsyncTransport(std::make_shared<FCannedSyncTransport>(
			std::move(Body), Status, std::move(Capture), std::move(Counters)));
	}
}
