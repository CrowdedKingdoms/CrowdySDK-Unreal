// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;

DECLARE_DELEGATE_OneParam(FOnLoopbackToken, const FString& /*Token*/);
DECLARE_DELEGATE_OneParam(FOnLoopbackError, const FString& /*Message*/);

/**
 * Single-shot loopback HTTP listener that captures a sign-in redirect on
 * http://127.0.0.1:<port>/callback and hands the one-time token/code back to the caller.
 * Used by the runtime auth (magic-link + social sign-in) and by the CrowdyStudio editor
 * (native social / magic-link sign-in); social supplies a server-issued `state`.
 *
 * Lives in CrowdyNet so both the runtime (CrowdyServices) and the editor (CrowdyStudio) can
 * reuse one implementation. Exported CROWDYNET_API for those cross-module consumers.
 *
 * Threading: all public methods AND the bound route handler run on the GAME THREAD. The engine
 * HttpServer module is an FTSTickerObjectBase ticked on the game thread, so handler dispatch and
 * the FTSTicker timeout are game-thread-confined. No locks are needed; the single-shot guard is a
 * plain bool valid under that invariant.
 *
 * Lifecycle: a loopback port is bound once from a fixed candidate set and kept for the session
 * (sticky — a stable redirectUri to allowlist server-side, and no port churn across sign-ins).
 * Only the /callback route is bound/unbound per flow. The class deliberately never calls the
 * engine's process-global StopAllListeners(): that would tear down listeners owned by other
 * consumers in the same process (a second PIE client, WebRemoteControl, ...).
 */
class CROWDYNET_API FCrowdyLoopbackAuthServer
{
public:
	FCrowdyLoopbackAuthServer() = default;
	~FCrowdyLoopbackAuthServer();

	/**
	 * Bind /callback on the sticky loopback port and arm a timeout. Returns the redirect URI
	 * (http://127.0.0.1:<port>/callback) to pass to requestLoginLink, or empty on failure (in
	 * which case InOnError has already fired). If ExpectedState is non-empty, the callback's
	 * `state` query param must match it (CSRF guard for social; the magic-link flow passes empty,
	 * relying on the loopback-only + single-use-token guarantees).
	 */
	FString Start(const FString& ExpectedState, double TimeoutSeconds,
	              FOnLoopbackToken InOnToken, FOnLoopbackError InOnError);

	/**
	 * Bind the sticky loopback port (without binding the /callback route or arming a timeout) and
	 * return its redirect URI (http://127.0.0.1:<port>/callback), or empty on failure. The social
	 * flow needs this redirectUri to hand to socialLoginStart BEFORE it knows the server-issued
	 * state that Start() must be armed with. Idempotent: the same port is reused by the later Start().
	 */
	FString ReserveRedirectUri();

	/** Unbind the route, cancel timers, clear the per-flow state. Keeps the bound port for reuse. */
	void Stop();

	bool IsActive() const { return bRouteBound; }

private:
	bool EnsureListening();
	bool HandleCallback(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool OnTimeout(float DeltaTime);
	bool OnDeferredStop(float DeltaTime);
	static bool IsLoopbackPeer(const FHttpServerRequest& Request);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle RouteHandle;
	uint32 BoundPort = 0;

	FString ExpectedState;
	FOnLoopbackToken OnToken;
	FOnLoopbackError OnError;

	bool bRouteBound = false;
	bool bConsumed = false;
	FTSTicker::FDelegateHandle TimeoutHandle;
	FTSTicker::FDelegateHandle DeferredStopHandle;
};
