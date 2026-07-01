// Fill out your copyright notice in the Description page of Project Settings.

#include "Auth/FCrowdyLoopbackAuthServer.h"

#include "CrowdyNetLog.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpServerConstants.h"
#include "HttpPath.h"
#include "IHttpRouter.h"
#include "IPAddress.h"

namespace
{
	// Fixed loopback ports tried in order. The chosen one becomes the session's sticky redirect
	// origin, so this whole set is what an app's allowed redirect URIs should cover server-side.
	static const uint32 GLoopbackCandidatePorts[] = { 53117, 53118, 53119, 53120, 53121 };

	static const FString GCloseTabHtml = TEXT(
		"<!doctype html><html><head><meta charset=\"utf-8\"><title>Signed in</title></head>"
		"<body style=\"font-family:sans-serif;text-align:center;padding-top:3rem\">"
		"<h2>You're signed in</h2><p>You can close this tab and return to the game.</p>"
		"</body></html>");
}

FCrowdyLoopbackAuthServer::~FCrowdyLoopbackAuthServer()
{
	Stop();
	Router.Reset();
	// Intentionally NOT StopAllListeners() it is process-global (see header).
}

bool FCrowdyLoopbackAuthServer::EnsureListening()
{
	if (Router.IsValid() && BoundPort != 0)
	{
		return true;
	}

	FHttpServerModule& Module = FHttpServerModule::Get();

	for (uint32 Port : GLoopbackCandidatePorts)
	{
		if (!Module.GetHttpRouter(Port, /*bFailOnBindFailure=*/ true).IsValid())
		{
			continue;
		}

		Module.StartAllListeners();

		// The socket bind happens in StartAllListeners. The first GetHttpRouter above can return a
		// not-yet-bound router (when listeners were still disabled), so it is NOT proof of binding.
		// Once listeners are enabled, GetHttpRouter(Port, /*bFailOnBindFailure=*/true) returns null
		// for a port that did not bind (and drops the dead listener). So this re-query is the
		// reliable "did it actually bind?" check. Do not remove it.
		TSharedPtr<IHttpRouter> Verified = Module.GetHttpRouter(Port, /*bFailOnBindFailure=*/ true);
		if (Verified.IsValid())
		{
			Router = Verified;
			BoundPort = Port;
			return true;
		}
	}

	UE_LOG(LogCrowdyNet, Error,
		TEXT("[Loopback] Loopback listener: no free port in the candidate range."));
	return false;
}

FString FCrowdyLoopbackAuthServer::ReserveRedirectUri()
{
	if (!EnsureListening())
	{
		return FString();
	}
	return FString::Printf(TEXT("http://127.0.0.1:%u/callback"), BoundPort);
}

FString FCrowdyLoopbackAuthServer::Start(const FString& InExpectedState, double TimeoutSeconds,
                                         FOnLoopbackToken InOnToken, FOnLoopbackError InOnError)
{
	// Clear any prior flow so a re-entrant Starts re-arms cleanly.
	Stop();

	if (!EnsureListening())
	{
		InOnError.ExecuteIfBound(TEXT("Could not open a local sign-in port."));
		return FString();
	}

	ExpectedState = InExpectedState;
	OnToken       = InOnToken;
	OnError       = InOnError;
	bConsumed     = false;

	// Keep this a STATIC path. UE 5.8's router never actually unbinds a *parameterized* route
	// (e.g. "/callback/:provider"), so social must read the provider from a query param, not the
	// path.
	RouteHandle = Router->BindRoute(
		FHttpPath(TEXT("/callback")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FCrowdyLoopbackAuthServer::HandleCallback));

	if (!RouteHandle.IsValid())
	{
		InOnError.ExecuteIfBound(TEXT("Could not register the sign-in callback route."));
		return FString();
	}
	bRouteBound = true;

	if (TimeoutSeconds > 0.0)
	{
		TimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FCrowdyLoopbackAuthServer::OnTimeout),
			static_cast<float>(TimeoutSeconds));
	}

	UE_CLOG(CrowdyNetTrace::Query(), LogCrowdyNet, Log,
		TEXT("[Loopback] Loopback listener armed on 127.0.0.1:%u/callback."), BoundPort);

	return FString::Printf(TEXT("http://127.0.0.1:%u/callback"), BoundPort);
}

void FCrowdyLoopbackAuthServer::Stop()
{
	if (TimeoutHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TimeoutHandle);
		TimeoutHandle.Reset();
	}
	if (DeferredStopHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DeferredStopHandle);
		DeferredStopHandle.Reset();
	}

	if (bRouteBound && Router.IsValid() && RouteHandle.IsValid())
	{
		Router->UnbindRoute(RouteHandle);
	}
	RouteHandle.Reset();
	bRouteBound = false;
	bConsumed   = false;

	ExpectedState.Reset();
	OnToken.Unbind();
	OnError.Unbind();
	// Router + BoundPort kept: the loopback port is sticky and reused on the next Start.
}

bool FCrowdyLoopbackAuthServer::HandleCallback(const FHttpServerRequest& Request,
                                               const FHttpResultCallback& OnComplete)
{
	if (bConsumed)
	{
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::TooManyRequests,
			TEXT(""), TEXT("Sign-in already handled.")));
		return true;
	}

	if (!IsLoopbackPeer(Request))
	{
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::Forbidden,
			TEXT(""), TEXT("Loopback only.")));
		return true;
	}

	const FString* State = Request.QueryParams.Find(TEXT("state"));
	const FString* Token = Request.QueryParams.Find(TEXT("token"));
	if (Token == nullptr)
	{
		// Param name is not pinned in the docs (token vs code) accept either.
		Token = Request.QueryParams.Find(TEXT("code"));
	}

	const bool bStateOk = ExpectedState.IsEmpty() || (State != nullptr && *State == ExpectedState);
	if (!bStateOk || Token == nullptr || Token->IsEmpty())
	{
		// Do NOT consume a CSRF/garbage hit must not burn the single shot or the user's attempt.
		OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::BadRequest,
			TEXT(""), TEXT("Invalid sign-in callback.")));
		return true;
	}

	bConsumed = true;
	OnComplete(FHttpServerResponse::Create(GCloseTabHtml, TEXT("text/html")));

	UE_CLOG(CrowdyNetTrace::Query(), LogCrowdyNet, Log,
		TEXT("[Loopback] Loopback callback captured (state checked: %s)."),
		ExpectedState.IsEmpty() ? TEXT("no") : TEXT("yes"));

	// Tear the route down on the next tick; AFTER the response has flushed, the unbinding inline can
	// drop the connection before the "you can close this tab" body is written. Scheduled BEFORE
	// firing OnToken so a handler that re-enters Start() stays safe: Start()'s Stop() clears this
	// handle, so the ticker never tears down the route that re-entrant Start() just re-bound.
	DeferredStopHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FCrowdyLoopbackAuthServer::OnDeferredStop), 0.0f);

	OnToken.ExecuteIfBound(*Token);

	return true;
}

bool FCrowdyLoopbackAuthServer::OnTimeout(float)
{
	// About to return false (which auto-removes this ticker); clear the handle so Stop() does not
	// also try to remove it.
	TimeoutHandle.Reset();

	if (!bConsumed)
	{
		OnError.ExecuteIfBound(TEXT("Sign-in timed out. Please try again."));
	}
	Stop();
	return false;
}

bool FCrowdyLoopbackAuthServer::OnDeferredStop(float)
{
	DeferredStopHandle.Reset();
	Stop();
	return false;
}

bool FCrowdyLoopbackAuthServer::IsLoopbackPeer(const FHttpServerRequest& Request)
{
	if (!Request.PeerAddress.IsValid())
	{
		// Fail closed: a peer we cannot identify is treated as non-loopback. This is the sole
		// barrier the moment the engine's process-global bind address is ever set to "any".
		return false;
	}

	FString Peer = Request.PeerAddress->ToString(/*bAppendPort=*/ false);

	// Normalize an IPv4-mapped-IPv6 loopback (e.g. ::ffff:127.0.0.1, which dual-stack sockets can
	// produce) down to its IPv4 form before the 127.0.0.0/8 check.
	if (Peer.StartsWith(TEXT("::ffff:"), ESearchCase::IgnoreCase))
	{
		Peer.RightChopInline(7);
	}

	return Peer.StartsWith(TEXT("127.")) || Peer == TEXT("::1") || Peer == TEXT("0:0:0:0:0:0:0:1");
}
