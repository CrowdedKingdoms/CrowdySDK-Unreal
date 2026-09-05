#include "Platform/CrowdyCppHttpTransport.h"

#include "CrowdyCppBridge.h"
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

	// FHttpModule-backed async transport. Requests are issued on the calling
	// thread (the game thread, where requestAsync runs) and FHttpModule delivers
	// OnProcessRequestComplete on the game thread too, so no cross-thread hop is
	// introduced here; the Dispatcher poll() in the client is the marshalling seam.
	class FUnrealAsyncHttpTransport final : public IAsyncHttpTransport
	{
	public:
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

			const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest =
				FHttpModule::Get().CreateRequest();
			HttpRequest->SetURL(FString(UTF8_TO_TCHAR(Request.url.c_str())));
			HttpRequest->SetVerb(FString(UTF8_TO_TCHAR(Request.method.c_str())));
			for (const std::pair<std::string, std::string>& Header : Request.headers)
			{
				HttpRequest->SetHeader(FString(UTF8_TO_TCHAR(Header.first.c_str())),
					FString(UTF8_TO_TCHAR(Header.second.c_str())));
			}

			// Send the body as raw bytes so the UTF-8 request payload is never
			// re-encoded through an intermediate FString.
			TArray<uint8> Body;
			Body.Append(reinterpret_cast<const uint8*>(Request.body.data()),
				static_cast<int32>(Request.body.size()));
			HttpRequest->SetContent(MoveTemp(Body));

			// A non-positive timeout leaves FHttpModule's own default in place
			// rather than disabling the timeout (SetTimeout(0) would).
			if (Request.timeoutMs > 0)
			{
				HttpRequest->SetTimeout(static_cast<float>(Request.timeoutMs) / 1000.0f);
			}

			HttpRequest->OnProcessRequestComplete().BindLambda(
				[Callback](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
				{
					HttpOutcome Outcome;
					if (bConnectedSuccessfully && Response.IsValid())
					{
						const TArray<uint8>& Payload = Response->GetContent();
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

			// A synchronous start failure (invalid URL, HTTP module shutting down)
			// does not fire the completion delegate, so complete it here to keep
			// the exactly-once contract the GraphQL client depends on.
			if (!HttpRequest->ProcessRequest())
			{
				HttpOutcome Outcome;
				Outcome.status = crowdy::Errc::SocketError;
				Outcome.errorMessage = "failed to start HTTP request";
				(*Callback)(std::move(Outcome));
			}
		}
	};

	// Synchronous transport that returns a fixed response. Wrapped by the inline
	// async adapter so the canned path runs the identical interpret() logic as a
	// real round trip.
	class FCannedSyncTransport final : public IHttpTransport
	{
	public:
		FCannedSyncTransport(std::string InBody, int InStatus,
			std::shared_ptr<CrowdyCppTransport::FCannedRequestCapture> InCapture)
			: Body(std::move(InBody)), Status(InStatus), Capture(std::move(InCapture)) {}

		HttpResponse send(const HttpRequest& Request) override
		{
			if (Capture)
			{
				Capture->bHasRequest = true;
				Capture->Url = Request.url;
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

	private:
		std::string Body;
		int Status;
		std::shared_ptr<CrowdyCppTransport::FCannedRequestCapture> Capture;
	};
}

namespace CrowdyCppTransport
{
	std::shared_ptr<IAsyncHttpTransport> MakeFHttpTransport()
	{
		return std::make_shared<FUnrealAsyncHttpTransport>();
	}

	std::shared_ptr<IAsyncHttpTransport> MakeCannedTransport(std::string Body, int Status,
		std::shared_ptr<FCannedRequestCapture> Capture)
	{
		return crowdy::graphql::makeInlineAsyncTransport(
			std::make_shared<FCannedSyncTransport>(std::move(Body), Status, std::move(Capture)));
	}
}
