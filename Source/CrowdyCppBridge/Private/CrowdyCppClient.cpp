#include "CrowdyCppClient.h"

#include "CrowdyCppBridge.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Platform/CrowdyCppHttpTransport.h"
#include "Platform/CrowdyCppWebSocketTransport.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/client.hpp"
#include "crowdy/core/crypto.hpp"
#include "crowdy/core/result.hpp"
#include "crowdy/domains/auth.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/graphql/subscription_client.hpp"
#include "crowdy/studio/model_lint.hpp"
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	// One issued request whose completion has not been delivered yet. The client keeps these so that cancelling,
	// closing, or destroying it delivers the caller's completion as canceled instead of dropping it: the underlying
	// client fences a pending completion rather than running it, and a completion that never arrives is
	// indistinguishable from a hang at the latent node waiting on it.
	struct FPendingCompletion
	{
		TFunction<void()> DeliverCanceled;
	};

	// When the attempt now in flight was sent, and how many times the request has been sent again.
	struct FRequestProgress
	{
		double AttemptSeconds = 0.0;
		int32 Retries = 0;
	};

	// Which refusals a request may be sent again after: none, any the platform says to retry, or only PLATFORM_BUSY.
	enum class EBusyRetryKind : uint8
	{
		None,
		Query,
		NeverStarted
	};

	// Everything needed to send one request again, unchanged, after the platform refused it.
	struct FBusyRetry
	{
		TFunction<void(crowdy::CrowdyClient&, crowdy::graphql::GraphQLCallback)> Send;
		crowdy::graphql::GraphQLCallback Deliver;
		TSharedPtr<double> HandOffSeconds;
		TSharedPtr<FRequestProgress> Progress;
		uint64 HandleId = 0;
		double DueSeconds = 0.0;
		int32 OpSlot = INDEX_NONE;
		ECrowdyCppTokenPlane Plane = ECrowdyCppTokenPlane::Game;
		EBusyRetryKind Kind = EBusyRetryKind::None;
	};

	// Every request issued on one client and not yet completed, keyed by the handle its caller was given.
	struct FRequestRegistry
	{
		TMap<uint64, TSharedPtr<FPendingCompletion>> Pending;
		uint64 NextHandleId = 1;

		// Requests waiting out a retry backoff, still in Pending so a cancel delivers them; at most MaxParkedRetries.
		TArray<TSharedRef<FBusyRetry>> WaitingRetries;

		// Deliver every pending completion as canceled, returning how many ran. The map is moved out first, so a
		// completion that issues or cancels another request from inside its own delivery cannot mutate the container
		// being walked, and the moved-out map keeps each entry alive across its own call.
		int32 DrainAsCanceled()
		{
			WaitingRetries.Reset();
			TMap<uint64, TSharedPtr<FPendingCompletion>> Draining = MoveTemp(Pending);
			Pending.Reset();

			int32 Delivered = 0;
			for (const TPair<uint64, TSharedPtr<FPendingCompletion>>& Entry : Draining)
			{
				if (Entry.Value.IsValid() && Entry.Value->DeliverCanceled)
				{
					++Delivered;
					DeliverCanceledContained(*Entry.Value);
				}
			}
			return Delivered;
		}

		// Run one cancellation, containing anything it throws. This drain runs from the client's destructor, where an
		// escaping exception would terminate the process, and one completion that fails must not cost the rest of
		// them their delivery.
		static void DeliverCanceledContained(const FPendingCompletion& Entry)
		{
			try
			{
				Entry.DeliverCanceled();
			}
			catch (const std::exception& Ex)
			{
				UE_LOG(LogCrowdyCpp, Warning, TEXT("A canceled request completion threw: %hs"), Ex.what());
			}
			catch (...)
			{
				UE_LOG(LogCrowdyCpp, Warning, TEXT("A canceled request completion threw a non-standard exception"));
			}
		}

		// The most recent OpLatencySamples values, the oldest overwritten first. Sized once so recording never allocates.
		struct FSampleRing
		{
			TArray<double> Ms;
			int32 Next = 0;
			int32 Count = 0;

			void Add(double Value)
			{
				Ms[Next] = Value;
				Next = (Next + 1) % Ms.Num();
				Count = FMath::Min(Count + 1, Ms.Num());
			}

			void CopyOldestFirst(TArray<double>& Out) const
			{
				Out.Reset(Count);
				const int32 Oldest = (Next - Count + Ms.Num()) % Ms.Num();
				for (int32 Offset = 0; Offset < Count; ++Offset)
				{
					Out.Add(Ms[(Oldest + Offset) % Ms.Num()]);
				}
			}

			void Reset()
			{
				Next = 0;
				Count = 0;
			}
		};

		// One operation's diagnostics. Slots are never removed, so an index taken at issue stays valid.
		struct FOpSlot
		{
			FString Operation;
			FSampleRing Latency;
			FSampleRing Sdk;
			FSampleRing Http;
			int32 Calls = 0;
			int32 Failures = 0;
			int32 Retries = 0;
			int32 RecoveredByRetry = 0;
			double MaxMs = 0.0;
			TMap<FString, int32> FailureReasons;
		};
		TArray<FOpSlot> OpSlots;

		// Reasons are keyed by server code or status, never message text, so a handful covers every real case.
		static constexpr int32 MaxFailureReasons = 8;
		static constexpr int32 MaxFailureReasonChars = 48;

		static void CountFailureReason(TMap<FString, int32>& Reasons, const FString& Reason)
		{
			const FString Key = Reason.Left(MaxFailureReasonChars);
			if (int32* Count = Reasons.Find(Key))
			{
				++*Count;
				return;
			}
			++Reasons.FindOrAdd(Reasons.Num() < MaxFailureReasons ? Key : FString(TEXT("other")));
		}
		TMap<FString, int32> OpSlotByName;

		int32 FindOrAddOpSlot(const TCHAR* Operation)
		{
			const FString Name(Operation);
			if (const int32* Found = OpSlotByName.Find(Name))
			{
				return *Found;
			}
			const int32 Index = OpSlots.AddDefaulted();
			FOpSlot& Slot = OpSlots[Index];
			Slot.Operation = Name;
			Slot.Latency.Ms.SetNumZeroed(FCrowdyCppClient::OpLatencySamples);
			Slot.Sdk.Ms.SetNumZeroed(FCrowdyCppClient::OpLatencySamples);
			Slot.Http.Ms.SetNumZeroed(FCrowdyCppClient::OpLatencySamples);
			OpSlotByName.Add(Name, Index);
			return Index;
		}

		// HandOffSeconds is when the transport handed the request to the HTTP module, or 0 when it never said, in
		// which case the split is unknown and recorded as nothing rather than as zero.
		void RecordOp(int32 SlotIndex, double IssueSeconds, const FRequestProgress& Progress, double HandOffSeconds,
			bool bSucceeded, const FString& FailureReason)
		{
			if (!OpSlots.IsValidIndex(SlotIndex))
			{
				return;
			}
			const double NowSeconds = FPlatformTime::Seconds();
			const double LatencyMs = (NowSeconds - IssueSeconds) * 1000.0;
			FOpSlot& Slot = OpSlots[SlotIndex];
			++Slot.Calls;
			Slot.Failures += bSucceeded ? 0 : 1;
			Slot.RecoveredByRetry += bSucceeded && Progress.Retries > 0 ? 1 : 0;
			if (!bSucceeded)
			{
				CountFailureReason(Slot.FailureReasons, FailureReason);
			}
			Slot.MaxMs = FMath::Max(Slot.MaxMs, LatencyMs);
			Slot.Latency.Add(LatencyMs);
			if (HandOffSeconds <= 0.0)
			{
				return;
			}
			Slot.Sdk.Add((HandOffSeconds - Progress.AttemptSeconds) * 1000.0);
			Slot.Http.Add((NowSeconds - HandOffSeconds) * 1000.0);
		}
	};

	// Whether a completed request did what it was asked. Every result type carries bOk except these two.
	template <typename ResultType>
	bool DidRequestSucceed(const ResultType& Result)
	{
		return Result.bOk;
	}

	bool DidRequestSucceed(const FCrowdyCppInvokeResult& Result)
	{
		return Result.bTransportOk && Result.bSuccess;
	}

	bool DidRequestSucceed(const FCrowdyCppJsonResult& Result)
	{
		return Result.bTransportOk;
	}

	// The server's own code on a result that carries one: an invoke's in-band fault has no GraphQL error to read it from.
	template <typename ResultType>
	FString ResultFailureCode(const ResultType&)
	{
		return FString();
	}

	FString ResultFailureCode(const FCrowdyCppInvokeResult& Result)
	{
		return Result.FaultCode;
	}

	FString ResultFailureCode(const FCrowdyCppJsonResult& Result)
	{
		return Result.ErrorCode;
	}

	FString ResultFailureCode(const FCrowdyCppAppTokenResult& Result)
	{
		return Result.ErrorCode;
	}

	// The exactly-once delivery closure for one request, and the handle its caller cancels it by.
	template <typename ResultType>
	struct TIssuedRequest
	{
		TFunction<void(ResultType)> Fire;
		FCrowdyCppRequestHandle Handle;

		// Where the transport records the hand-off, while a CrowdyCppTransport::FHandOffScope marks it.
		TSharedPtr<double> HandOffSeconds;

		// Why the request failed, written by its completion from the outcome it saw; see OutcomeFailureReason.
		TSharedPtr<FString> FailureReason;

		TSharedPtr<FRequestProgress> Progress;
		int32 OpSlot = INDEX_NONE;
	};

	// Wrap a caller's completion so it is delivered exactly once, by whichever path finishes first: the server
	// response, an issue-time failure, or a cancellation. Registering it is what makes cancellation possible at all,
	// since the underlying client cannot recall one request; what a cancel ends is the delivery, and the server's
	// eventual answer is then discarded.
	template <typename ResultType>
	TIssuedRequest<ResultType> BeginRequest(const TSharedPtr<FRequestRegistry>& Registry, const TCHAR* Operation,
		TFunction<void(ResultType)> OnDone, ResultType CanceledResult)
	{
		const TSharedRef<TFunction<void(ResultType)>> Done =
			MakeShared<TFunction<void(ResultType)>>(MoveTemp(OnDone));
		const TSharedRef<bool> bFired = MakeShared<bool>(false);

		TIssuedRequest<ResultType> Issued;
		if (Registry.IsValid())
		{
			Issued.Handle.Id = Registry->NextHandleId++;
		}

		// Poll() drains on the game thread and every other delivery path runs synchronously on that same thread, so
		// the fired flag needs no lock. The registry is held weakly because a completion can outlive it only in the
		// order the client tears down in, and deregistering is an optimisation there rather than a requirement.
		const uint64 HandleId = Issued.Handle.Id;
		const TWeakPtr<FRequestRegistry> WeakRegistry = Registry;
		const int32 OpSlot = Registry.IsValid() ? Registry->FindOrAddOpSlot(Operation) : INDEX_NONE;
		const double IssueSeconds = FPlatformTime::Seconds();
		Issued.OpSlot = OpSlot;
		Issued.HandOffSeconds = MakeShared<double>(0.0);
		Issued.FailureReason = MakeShared<FString>();
		Issued.Progress = MakeShared<FRequestProgress>();
		Issued.Progress->AttemptSeconds = IssueSeconds;
		Issued.Fire = [Done, bFired, WeakRegistry, HandleId, OpSlot, IssueSeconds, HandOff = Issued.HandOffSeconds,
			Reason = Issued.FailureReason, Progress = Issued.Progress](ResultType Result)
		{
			if (*bFired)
			{
				return;
			}
			*bFired = true;
			if (const TSharedPtr<FRequestRegistry> Pinned = WeakRegistry.Pin())
			{
				const FString Code = ResultFailureCode(Result);
				const FString Why = !Code.IsEmpty() ? Code : (!Reason->IsEmpty() ? *Reason : FString(TEXT("transport")));
				Pinned->Pending.Remove(HandleId);
				Pinned->RecordOp(OpSlot, IssueSeconds, *Progress, *HandOff, DidRequestSucceed(Result), Why);
			}
			(*Done)(MoveTemp(Result));
		};

		if (Registry.IsValid())
		{
			const TSharedRef<FPendingCompletion> Entry = MakeShared<FPendingCompletion>();
			// A canceled request's http time would stop at the cancel rather than at an answer, so it records no split.
			Entry->DeliverCanceled = [Fire = Issued.Fire, CanceledResult, HandOff = Issued.HandOffSeconds,
				Reason = Issued.FailureReason]()
			{
				*HandOff = 0.0;
				*Reason = TEXT("canceled");
				Fire(CanceledResult);
			};
			Registry->Pending.Add(HandleId, Entry);
		}
		return Issued;
	}

	// The re-sendable form of an issued request, bound to its handle, stats slot and hand-off stamp.
	template <typename ResultType>
	TSharedRef<FBusyRetry> MakeBusyRetry(const TIssuedRequest<ResultType>& Issued, ECrowdyCppTokenPlane Plane,
		EBusyRetryKind Kind)
	{
		const TSharedRef<FBusyRetry> Retry = MakeShared<FBusyRetry>();
		Retry->HandOffSeconds = Issued.HandOffSeconds;
		Retry->Progress = Issued.Progress;
		Retry->HandleId = Issued.Handle.Id;
		Retry->OpSlot = Issued.OpSlot;
		Retry->Plane = Plane;
		Retry->Kind = Kind;
		return Retry;
	}

	// What ListContainers hands back, so its two-value completion can ride the same one-result plumbing as every
	// other call and therefore be canceled the same way.
	struct FContainerListResult
	{
		bool bOk = false;
		TArray<TSharedPtr<FJsonObject>> Containers;
	};

	// Defined here rather than with the other response helpers below because FImpl uses it, and FImpl is written
	// before them.
	FString Utf8ToFString(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(Value.data(), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}
}

struct FCrowdyCppClient::FImpl
{
	TUniquePtr<crowdy::CrowdyClient> Client;

	// Every request issued and not yet completed. Held by shared pointer so a completion can hold it weakly.
	TSharedPtr<FRequestRegistry> Requests = MakeShared<FRequestRegistry>();

	// Shared with the HTTP transport, whose completions may land off the game thread.
	std::shared_ptr<CrowdyCppTransport::FTransportCounters> TransportCounters =
		std::make_shared<CrowdyCppTransport::FTransportCounters>();

	// One bearer per endpoint. The underlying client holds a single auth state shared by both of its GraphQL
	// clients, so the right token is installed immediately before each request is built rather than being seeded
	// once and left for whichever caller comes next. The request builder reads the bearer synchronously while
	// building the headers, so a request already in flight never observes a later plane switch.
	FString GameToken;
	FString ManagementToken;

	/**
	 * The answer re-discovery gives when the client asks where to go, refreshed by the owner through
	 * SetRediscoveredEndpoint and read synchronously from inside CrowdyCPP.
	 *
	 * It is a cache rather than a lookup because of when it is read. CrowdyCPP asks the moment something notices
	 * the endpoint is dead, from whichever thread noticed (the subscription client's reconnect timer, most often),
	 * and the callback is contractually forbidden to block. A live query there would either block that thread or
	 * have to fail, and CrowdyCPP's own bootstrap re-discovery does the latter here: it runs on the blocking
	 * IHttpTransport, which this bridge deliberately leaves null in favour of FHttpModule.
	 *
	 * Held by shared pointer and captured by value into the callback, so the callback stays safe to run even if it
	 * outlives this FImpl. Guarded because the reader is not the game thread.
	 */
	struct FRediscoverAnswer
	{
		FCriticalSection Guard;
		FString ApiUrl;
		FString WsUrl;
	};
	TSharedPtr<FRediscoverAnswer> Rediscover = MakeShared<FRediscoverAnswer>();

	// The callback CrowdyCPP holds, reading that cache and nothing else. Static and capturing the answer by value
	// so it stays safe to run even if it outlives the FImpl that made it, which it can: the client is destroyed
	// while its own reconnect timer may still be inside a call.
	static crowdy::graphql::RediscoverFn MakeRediscoverCallback(TSharedPtr<FRediscoverAnswer> Answer)
	{
		// The app id CrowdyCPP passes is ignored. The bridge builds one client per app, so there is only ever one
		// answer to give, and keying the cache by app would model a distinction that cannot arise here.
		return [Answer](const std::string&) -> crowdy::graphql::RediscoveredEndpoint
		{
			crowdy::graphql::RediscoveredEndpoint Out;
			if (!Answer.IsValid())
			{
				return Out;
			}
			FScopeLock Lock(&Answer->Guard);
			Out.httpUrl = std::string(TCHAR_TO_UTF8(*Answer->ApiUrl));
			Out.wsUrl = std::string(TCHAR_TO_UTF8(*Answer->WsUrl));
			return Out;
		};
	}

	/**
	 * Ask CrowdyCPP to re-discover once the bridge's own socket has failed to reconnect repeatedly.
	 *
	 * CrowdyClient installs this on the socket it owns, which is not the one the bridge uses, so without it the
	 * signal that matters most here (a socket that cannot reconnect because its instance is gone) would never
	 * reach re-discovery. The threshold is CrowdyCPP's own default: a single blip is what reconnect backoff is
	 * for, and moving on the first one would relocate a client on any transient loss.
	 *
	 * The handler runs on the subscription client's timer thread, so it must only touch thread-safe state.
	 * rediscoverEndpoint reads the guarded cache and then moves the GraphQL client, both of which are safe there;
	 * the bridge's own socket follows from the next Poll().
	 */
	void InstallRepeatedFailureHandler()
	{
		if (!SubscriptionClient || !Client)
		{
			return;
		}
		crowdy::CrowdyClient* Raw = Client.Get();
		SubscriptionClient->setRepeatedFailureHandler(
			crowdy::graphql::RediscoverCoordinator::kDefaultAfterFailures,
			[Raw]()
			{
				// Contained rather than allowed to escape: this runs on a timer thread with no handler above it.
				try
				{
					(void)Raw->rediscoverEndpoint(Raw->activeAppId());
				}
				catch (...)
				{
				}
			});
	}

	/**
	 * The GraphQL endpoint as of the last Poll(), so a move can be noticed and followed.
	 *
	 * The bridge builds its own subscription client rather than using the one inside CrowdyClient, because that one
	 * observes the shared auth state and would tear its socket down on every bearer switch. The cost is that
	 * CrowdyClient::moveToDatacenter moves its own socket and not ours, so a redirect would leave this client
	 * querying one datacenter while playing in another. That does not fail; it just puts a WAN under every write.
	 *
	 * Comparing after each pump catches every move (a WRONG_DATACENTER redirect, re-discovery, an explicit move)
	 * without needing a hook CrowdyCPP does not offer.
	 */
	FString LastSeenEndpoint;

#if WITH_DEV_AUTOMATION_TESTS
	// Set only for a test client, where it records what the canned transport was last handed.
	std::shared_ptr<CrowdyCppTransport::FCannedRequestCapture> TestCapture;

	// Set only for a test client, where it stands in for the socket so a test can play the server by hand.
	std::shared_ptr<CrowdyCppTransport::FScriptedWebSocketServer> TestWebSocket;
#endif

	/**
	 * The subscription client is built alongside the underlying client rather than taken from it, because it must
	 * not share the bearer.
	 *
	 * The underlying client holds one token, switched between the game and identity planes immediately before every
	 * request. A subscription client observing that state tears its socket down and reconnects on every switch, and
	 * re-authenticates with whichever token happened to be installed at that moment. This one holds the game bearer
	 * and nothing else, so it reconnects only when that token actually rotates, which is the one case where
	 * reconnecting is the right answer. It shares the client's completion pump, so Poll() drains it too.
	 */
	std::shared_ptr<crowdy::graphql::AuthState> SubscriptionAuth;
	std::shared_ptr<crowdy::graphql::GraphQLSubscriptionClient> SubscriptionClient;

	/**
	 * One live subscription. bFinished is a shared flag rather than a map lookup because it is set from inside a
	 * callback: reaping the entry there would destroy the subscription handle whose delivery is on the stack, and
	 * cancelling it would re-enter the subscription client from inside its own dispatch. So a finished subscription
	 * is marked during delivery and reaped by the next Poll().
	 */
	struct FLiveSubscription
	{
		crowdy::graphql::SubscriptionHandle Handle;
		TSharedRef<bool> bFinished = MakeShared<bool>(false);
		TSharedRef<FCrowdyCppSubscriptionCallbacks> Callbacks = MakeShared<FCrowdyCppSubscriptionCallbacks>();
	};

	TMap<uint64, TUniquePtr<FLiveSubscription>> LiveSubscriptions;

	bool bClosed = false;

	// True when a request may be issued: constructed and not disposed.
	bool IsUsable() const { return Client.IsValid() && !bClosed; }

	// Send Retry under its plane, true when it went out; a refusal it may repeat is queued rather than delivered.
	bool SendRetryable(const TSharedRef<FBusyRetry>& Retry);

	// The operation name Retry's stats are kept under, for messages.
	FString OperationOf(const FBusyRetry& Retry) const;

	// Send again every queued request whose backoff has run out. Called from Poll().
	void SendDueRetries();

	void ApplyToken(ECrowdyCppTokenPlane Plane)
	{
		if (!Client)
		{
			return;
		}
		const FString& Token = Plane == ECrowdyCppTokenPlane::Management ? ManagementToken : GameToken;
		if (Token.IsEmpty())
		{
			// Every operation this facade issues is authenticated, so an empty bearer is a wiring mistake rather
			// than an anonymous call, and it would otherwise surface only as an opaque server-side rejection.
			UE_LOG(LogCrowdyCpp, Warning,
				TEXT("No %s API token is installed; the request will be sent without an Authorization header."),
				Plane == ECrowdyCppTokenPlane::Management ? TEXT("identity") : TEXT("game"));
		}
		Client->setToken(std::string(TCHAR_TO_UTF8(*Token)));
	}

	// Which bearer a call installs. This is deliberately a separate choice from which endpoint the call reaches:
	// they normally agree, but a sign-in reaches the identity endpoint carrying nothing, and an app-token refresh
	// reaches it carrying the game bearer, because the token being rotated is what authorizes its own rotation.
	enum class EBearerChoice : uint8
	{
		Game,
		Management,
		None
	};

	// The client to issue on, with the requested bearer installed, or null when the client is unusable. Every
	// issuing path goes through this so no request can be built under whatever bearer was left behind.
	crowdy::CrowdyClient* UseBearer(EBearerChoice Choice)
	{
		if (!IsUsable())
		{
			return nullptr;
		}
		if (Choice == EBearerChoice::None)
		{
			// Clearing rather than warning: a sign-in is how a caller obtains a bearer, so having none is the
			// expected state, and clearing also stops a previous caller's token from riding a public mutation. An
			// empty bearer sends no Authorization header at all.
			Client->setToken(std::string());
			return Client.Get();
		}
		ApplyToken(Choice == EBearerChoice::Management
			? ECrowdyCppTokenPlane::Management : ECrowdyCppTokenPlane::Game);
		return Client.Get();
	}

	crowdy::CrowdyClient* Use(ECrowdyCppTokenPlane Plane)
	{
		return UseBearer(Plane == ECrowdyCppTokenPlane::Management
			? EBearerChoice::Management : EBearerChoice::Game);
	}

	// Keep the subscription bearer in step with the game token. Installing an unchanged token is a no-op upstream,
	// so the owner reinstalling it on every resolve costs nothing and a genuine rotation reconnects the socket.
	void ApplySubscriptionToken()
	{
		if (SubscriptionAuth)
		{
			SubscriptionAuth->setToken(std::string(TCHAR_TO_UTF8(*GameToken)));
		}
	}

	// Move the subscription socket to wherever the GraphQL client has ended up. Called after each pump rather than
	// from a callback, because the move that prompts it happens inside CrowdyCPP's own redirect handling and there
	// is no hook to hang this on. Reconnecting replays every live subscription, so a caller sees a gap rather than
	// a lost stream.
	void FollowEndpointMove()
	{
		if (!Client)
		{
			return;
		}
		const FString Current = Utf8ToFString(Client->graphqlClient().endpoint());
		if (Current.IsEmpty() || Current == LastSeenEndpoint)
		{
			return;
		}
		LastSeenEndpoint = Current;
		if (!SubscriptionClient)
		{
			return;
		}
		UE_LOG(LogCrowdyCpp, Log, TEXT("API endpoint moved to %s; following with the subscription socket."), *Current);
		try
		{
			SubscriptionClient->setEndpoint(std::string(TCHAR_TO_UTF8(*Current)));
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Moving the subscription socket threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Moving the subscription socket threw a non-standard exception"));
		}
	}

	// Drop the subscriptions the callbacks have marked finished. Called after the pump, never during it.
	void ReapFinishedSubscriptions()
	{
		for (TMap<uint64, TUniquePtr<FLiveSubscription>>::TIterator It(LiveSubscriptions); It; ++It)
		{
			if (!It->Value.IsValid())
			{
				It.RemoveCurrent();
				continue;
			}
			if (*It->Value->bFinished)
			{
				It.RemoveCurrent();
				continue;
			}

			// A subscription the underlying client has dropped without reporting a terminal failure would otherwise
			// sit here forever, counted as active, with its caller still waiting on a recovery that cannot happen.
			// Reporting it here keeps the promise that a subscription which ends says so exactly once.
			if (!It->Value->Handle.active())
			{
				*It->Value->bFinished = true;
				if (It->Value->Callbacks->OnError)
				{
					DeliverContained(*It->Value->Callbacks, TEXT("the subscription ended"));
				}
				It.RemoveCurrent();
			}
		}
	}

	// Runs a caller's terminal OnError with any throw contained. Used on the paths that run during teardown or
	// inside an iteration, where an escaping exception would take the process down or corrupt the walk.
	static void DeliverContained(const FCrowdyCppSubscriptionCallbacks& Callbacks, const FString& Message)
	{
		try
		{
			if (Callbacks.OnError)
			{
				Callbacks.OnError(Message, true);
			}
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("A subscription failure handler threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("A subscription failure handler threw a non-standard exception"));
		}
	}

	// Cancels one subscription with any throw contained, for the same reason as above: cancelling reaches the
	// underlying client, which can throw, and two of the three callers are teardown paths.
	static void CancelContained(crowdy::graphql::SubscriptionHandle& Handle)
	{
		try
		{
			Handle.cancel();
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Cancelling a subscription threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Cancelling a subscription threw a non-standard exception"));
		}
	}

	// Tell every live subscription it is over and stop its callbacks, the way a pending request is delivered as
	// canceled: a stream that simply stops is indistinguishable to its caller from a server with nothing to say.
	void FailLiveSubscriptions(const FString& Message)
	{
		TMap<uint64, TUniquePtr<FLiveSubscription>> Ending = MoveTemp(LiveSubscriptions);
		LiveSubscriptions.Reset();

		for (TPair<uint64, TUniquePtr<FLiveSubscription>>& Entry : Ending)
		{
			if (!Entry.Value.IsValid() || *Entry.Value->bFinished)
			{
				continue;
			}
			*Entry.Value->bFinished = true;

			// Both halves are contained because this runs from the destructor path, where an escaping exception
			// would terminate the process rather than fail a call.
			DeliverContained(*Entry.Value->Callbacks, Message);
			CancelContained(Entry.Value->Handle);
		}
	}
};

namespace
{
	// Length-bounded UTF-8 conversion so an embedded NUL never truncates the value.
	/**
	 * The one error out of a GraphQL errors array that a caller should be told about.
	 *
	 * Normally the first, which is what the server led with. The exception is APP_UNAVAILABLE: it says the app's own
	 * datacenter cannot serve it and there is nowhere to move to, and it is not guaranteed to lead, because a
	 * partially resolved query reports whatever failed first and the routing error can sit behind it.
	 *
	 * Message and code come from this same entry on purpose. Choosing them independently is how a caller ends up
	 * branching on APP_UNAVAILABLE while showing an unrelated error's text.
	 */
	const crowdy::graphql::GraphQLErrorDetail* LeadingError(const crowdy::graphql::GraphQLOutcome& Out)
	{
		if (Out.errors.empty())
		{
			return nullptr;
		}
		for (const crowdy::graphql::GraphQLErrorDetail& Error : Out.errors)
		{
			if (Error.code == crowdy::graphql::kAppUnavailableCode)
			{
				return &Error;
			}
		}
		return &Out.errors[0];
	}

	FString OutcomeErrorMessage(const crowdy::graphql::GraphQLOutcome& Out)
	{
		// A server-authored message is the most useful thing a caller can put in front of a user, so it wins. The
		// APP_UNAVAILABLE one especially: the server knows why the app cannot be served and the client does not, so
		// it is written for a player to read and worth showing rather than replacing with generic text.
		if (const crowdy::graphql::GraphQLErrorDetail* Error = LeadingError(Out))
		{
			return Utf8ToFString(Error->message);
		}
		if (!Out.errorMessage.empty())
		{
			return Utf8ToFString(Out.errorMessage);
		}

		// Neither is set for a non-2xx that carried no GraphQL errors, which is the shape a gateway or an auth
		// middleware rejection arrives in. The status code is then the only actionable detail there is: without it
		// the caller can only show an internal error-code name, which tells a user nothing about what to do.
		if (Out.httpStatus != 0)
		{
			return FString::Printf(TEXT("Server returned HTTP %d"), Out.httpStatus);
		}
		return FString(UTF8_TO_TCHAR(crowdy::errcName(Out.status.code)));
	}

	// The blame attribution and retry verdict for a failed outcome, read from the same entry OutcomeErrorMessage
	// uses. This is the channel the overload refusal arrives on: writes to one hot container are serialised and a
	// call has a time budget, so past a certain rate the platform refuses rather than serving late, and says so with
	// blame PLATFORM and retryable true.
	//
	// Retryable is reported ONLY alongside an attribution. CrowdyCPP defaults extensions.retryable to true so that a
	// caller with no information errs toward trying again, which is the right default for a read; an invoke can have
	// committed before the failure was reported, so here the absence of attribution has to read as "do not know",
	// and only a server that named whose fault it was licenses a retry.
	void ReadOutcomeFault(const crowdy::graphql::GraphQLOutcome& Out, FCrowdyCppInvokeResult& OutResult)
	{
		const crowdy::graphql::GraphQLErrorDetail* Error = LeadingError(Out);
		if (!Error)
		{
			return;
		}
		OutResult.FaultCode = Utf8ToFString(Error->code);
		OutResult.Blame = Utf8ToFString(Error->blame);
		OutResult.bRetryable = !OutResult.Blame.IsEmpty() && Error->retryable;
		// Read unconditionally rather than under a code test: the player boundary rewrites the code to
		// USER_CODE_ERROR and leaves these three alone, so keying off the code would miss the refusal on
		// exactly the path a player takes.
		OutResult.QuarantinedKind = Utf8ToFString(Error->quarantinedKind);
		OutResult.QuarantinedName = Utf8ToFString(Error->quarantinedName);
		OutResult.QuarantineReason = Utf8ToFString(Error->quarantineReason);
		// Passed through exactly as CrowdyCPP resolved it, absence included. It already refuses a non-numeric value,
		// so anything present here is a number the server meant as a duration; bounding it is the consumer's job,
		// since only the consumer knows what waiting that long would cost it.
		if (Error->retryAfterMs.has_value())
		{
			OutResult.RetryAfterMs = static_cast<int64>(*Error->retryAfterMs);
		}
	}

	// The same attribution for a generic operation, which has no quarantine fields.
	void ReadOutcomeFault(const crowdy::graphql::GraphQLOutcome& Out, FCrowdyCppJsonResult& OutResult)
	{
		const crowdy::graphql::GraphQLErrorDetail* Error = LeadingError(Out);
		if (!Error)
		{
			return;
		}
		OutResult.ErrorCode = Utf8ToFString(Error->code);
		OutResult.Blame = Utf8ToFString(Error->blame);
		OutResult.bRetryable = !OutResult.Blame.IsEmpty() && Error->retryable;
		if (Error->retryAfterMs.has_value())
		{
			OutResult.RetryAfterMs = static_cast<int64>(*Error->retryAfterMs);
		}
	}

	// The server's stable extensions.code for a failed outcome, or empty when the failure carried no GraphQL errors
	// (a network error, a timeout, a non-2xx with no error body). Read from the same entry OutcomeErrorMessage uses,
	// so the code a caller branches on and the text it shows always describe the same failure.
	FString OutcomeErrorCode(const crowdy::graphql::GraphQLOutcome& Out)
	{
		const crowdy::graphql::GraphQLErrorDetail* Error = LeadingError(Out);
		return Error ? Utf8ToFString(Error->code) : FString();
	}

	// A short, bounded name for why an outcome failed, safe to count and print: the server's code, else the HTTP
	// status, else whether the server answered at all. Never message text, which can carry ids.
	FString OutcomeFailureReason(const crowdy::graphql::GraphQLOutcome& Out)
	{
		const FString Code = OutcomeErrorCode(Out);
		if (!Code.IsEmpty())
		{
			return Code;
		}
		if (Out.httpStatus != 0 && (Out.httpStatus < 200 || Out.httpStatus >= 300))
		{
			return FString::Printf(TEXT("http %d"), Out.httpStatus);
		}
		if (!Out.errors.empty())
		{
			return TEXT("graphql");
		}
		return Out.ok() ? TEXT("rejected") : TEXT("transport");
	}

	TAutoConsoleVariable<int32> CVarRetryBusy(
		TEXT("crowdy.net.retry.busy"), 1,
		TEXT("Sends an SDK query again, up to 3 times with backoff, when the platform refuses it and says to retry (blame PLATFORM, retryable); a container ensure or an invoke is sent again only when the platform says the work never started (PLATFORM_BUSY). 0 hands every refusal straight to the caller."),
		ECVF_Default);

	// Whether Out is a refusal a request of this kind may send again; CrowdyCPP reads a missing retryable as true.
	bool IsRetryableRefusal(const crowdy::graphql::GraphQLOutcome& Out, EBusyRetryKind Kind)
	{
		const crowdy::graphql::GraphQLErrorDetail* Error = LeadingError(Out);
		if (Kind == EBusyRetryKind::None || !Error || !Error->retryable
			|| !Utf8ToFString(Error->blame).Equals(TEXT("PLATFORM"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		if (Kind == EBusyRetryKind::NeverStarted)
		{
			return Error->code == "PLATFORM_BUSY";
		}
		// CrowdyCPP has already followed the redirect, and an unavailable app has nowhere else to be served from.
		return Error->code != crowdy::graphql::kWrongDatacenterCode
			&& Error->code != crowdy::graphql::kAppUnavailableCode;
	}

	// A query only reads and an ensure gets or creates one row by key, so sending either again cannot apply twice.
	EBusyRetryKind RetryKindFor(ECrowdyCppApiDomain Domain, std::string_view Document, const std::string& Operation)
	{
		if (Document.substr(0, 6) == "query ")
		{
			return EBusyRetryKind::Query;
		}
		const bool bEnsure = Domain == ECrowdyCppApiDomain::GameModel && Operation == "GameModelEnsureContainer";
		return bEnsure ? EBusyRetryKind::NeverStarted : EBusyRetryKind::None;
	}

	// Queue Retry to be sent again when Out is a refusal it may repeat; false when Out is to be delivered.
	bool TryQueueBusyRetry(FRequestRegistry& Registry, const TSharedRef<FBusyRetry>& Retry,
		const crowdy::graphql::GraphQLOutcome& Out)
	{
		if (Retry->Progress->Retries >= CrowdyCppMaxBusyRetries || !IsRetryableRefusal(Out, Retry->Kind)
			|| !CrowdyCppIsBusyRetryEnabled() || !Registry.Pending.Contains(Retry->HandleId))
		{
			return false;
		}
		if (Registry.WaitingRetries.Num() >= FCrowdyCppClient::MaxParkedRetries)
		{
			return false;
		}
		const std::optional<std::int64_t>& RetryAfterMs = LeadingError(Out)->retryAfterMs;
		const TOptional<double> Delay = CrowdyCppBusyRetryDelaySeconds(Retry->Progress->Retries,
			RetryAfterMs.has_value() ? TOptional<int64>(static_cast<int64>(*RetryAfterMs)) : TOptional<int64>());
		if (!Delay.IsSet())
		{
			return false;
		}
		Retry->DueSeconds = FPlatformTime::Seconds() + Delay.GetValue();
		Registry.WaitingRetries.Add(Retry);
		return true;
	}

	// The outcome a request that never reached the transport completes with.
	crowdy::graphql::GraphQLOutcome UnsentOutcome(std::string Message)
	{
		crowdy::graphql::GraphQLOutcome Out;
		Out.status = crowdy::Errc::SocketError;
		Out.kind = crowdy::graphql::GraphQLErrorKind::Network;
		Out.errorMessage = std::move(Message);
		return Out;
	}

	// Convert a UE JSON value tree to a CrowdyCPP JVal (the GraphQL variable-building type). Used to carry
	// FCrowdyGameApiCodec::BuildXVariables output across the boundary verbatim, so the request CrowdyCPP sends
	// carries exactly the variables the codec produced. A whole-valued number becomes an integer so
	// GraphQL Int fields (e.g. traverse depth) serialize without a decimal point, matching UE's own JSON writer.
	crowdy::graphql::JVal JsonValueToJVal(const TSharedPtr<FJsonValue>& Value)
	{
		using crowdy::graphql::JVal;
		if (!Value.IsValid())
		{
			return JVal(nullptr);
		}
		switch (Value->Type)
		{
		case EJson::Null:
			return JVal(nullptr);
		case EJson::Boolean:
			return JVal(Value->AsBool());
		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			if (FMath::IsFinite(Number) && FMath::Frac(Number) == 0.0
				&& Number >= -9.2e18 && Number <= 9.2e18)
			{
				return JVal(static_cast<std::int64_t>(Number));
			}
			return JVal(Number);
		}
		case EJson::String:
			return JVal(std::string(TCHAR_TO_UTF8(*Value->AsString())));
		case EJson::Array:
		{
			crowdy::graphql::JArray Array;
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			Array.reserve(Items.Num());
			for (const TSharedPtr<FJsonValue>& Item : Items)
			{
				Array.push_back(JsonValueToJVal(Item));
			}
			return JVal(std::move(Array));
		}
		case EJson::Object:
		{
			crowdy::graphql::JObject Object;
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (Obj.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
				{
					Object.emplace(std::string(TCHAR_TO_UTF8(*Pair.Key)), JsonValueToJVal(Pair.Value));
				}
			}
			return JVal(std::move(Object));
		}
		default:
			return JVal(nullptr);
		}
	}

	crowdy::graphql::JVal ObjectToJVal(const TSharedPtr<FJsonObject>& Object)
	{
		crowdy::graphql::JObject Out;
		if (Object.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				Out.emplace(std::string(TCHAR_TO_UTF8(*Pair.Key)), JsonValueToJVal(Pair.Value));
			}
		}
		return crowdy::graphql::JVal(std::move(Out));
	}

	// Decode an already-unwrapped CrowdyCPP JSON value into a UE JSON value. yyjson parsed the response iteratively,
	// but this UE re-parse is recursive, so the re-serialized text is nesting-guarded first: a forged deep value
	// would otherwise build a DOM that overflows on teardown. Returns null on an over-deep or undecodable value.
	TSharedPtr<FJsonValue> UnwrappedJsonToUeValue(const crowdy::graphql::Json& Value)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_GM_DecodeResponse);
		const FString Text = Utf8ToFString(Value.dump());
		if (Text.IsEmpty() || !CrowdyJsonSafety::IsNestingWithinLimit(Text))
		{
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		TSharedPtr<FJsonValue> Parsed;
		if (!FJsonSerializer::Deserialize(Reader, Parsed))
		{
			return nullptr;
		}
		return Parsed;
	}

	FCrowdyCppAuthResult MapAuthResponse(const crowdy::graphql::GraphQLOutcome& Out,
		const crowdy::domains::AuthResponse& Value)
	{
		FCrowdyCppAuthResult Result;
		if (!Out.ok())
		{
			Result.ErrorMessage = OutcomeErrorMessage(Out);
			return Result;
		}

		Result.SessionToken = Utf8ToFString(Value.token);
		if (Result.SessionToken.IsEmpty())
		{
			// A reachable server that answers without a token has not signed anyone in, and treating that as success
			// would install an empty bearer and fail later as an opaque rejection.
			Result.ErrorMessage = TEXT("sign-in returned no session token");
			return Result;
		}

		// Both ids are BigInt scalars, which arrive as decimal strings so no width is assumed on the wire.
		Result.SessionGameTokenID = FCString::Atoi64(*Utf8ToFString(Value.gameTokenId));
		Result.UserID = FCString::Atoi64(*Utf8ToFString(Value.userId));
		if (Result.UserID == 0)
		{
			// An answer carrying a token but no user identifies nobody. Accepting it would persist a session whose
			// user id is zero, and every later restore would read that same zero back.
			Result.ErrorMessage = TEXT("sign-in returned no user id");
			return Result;
		}

		Result.Email = Utf8ToFString(Value.email.valueOrEmpty());
		Result.Gamertag = Utf8ToFString(Value.gamertag.valueOrEmpty());
		Result.bOk = true;
		return Result;
	}

	FCrowdyCppAppTokenResult MapAppTokenResponse(const crowdy::graphql::GraphQLOutcome& Out,
		const crowdy::domains::AppTokenResponse& Value)
	{
		FCrowdyCppAppTokenResult Result;
		if (!Out.ok())
		{
			Result.ErrorMessage = OutcomeErrorMessage(Out);
			Result.ErrorCode = OutcomeErrorCode(Out);
			return Result;
		}

		Result.AppToken = Utf8ToFString(Value.token);
		if (Result.AppToken.IsEmpty())
		{
			Result.ErrorMessage = TEXT("app-token request returned no token");
			return Result;
		}

		Result.AppGameTokenID = FCString::Atoi64(*Utf8ToFString(Value.gameTokenId));
		Result.AppID = Utf8ToFString(Value.appId);
		Result.ExpiresAt = Utf8ToFString(Value.expiresAt);
		Result.GameApiUrl = Utf8ToFString(Value.gameApiUrl.valueOrEmpty());
		Result.GameApiWsUrl = Utf8ToFString(Value.gameApiWsUrl.valueOrEmpty());
		Result.DiscoveryUrl = Utf8ToFString(Value.discoveryUrl.valueOrEmpty());
		Result.LaunchUrl = Utf8ToFString(Value.launchUrl.valueOrEmpty());

		// Only when the reply actually names a server. A response with no authorizedServer parses into an empty
		// address and a zero port, and copying those through unguarded would offer a caller an endpoint to compare
		// against instead of the absence it has to treat as "re-assign".
		if (Value.hasAuthorizedServer())
		{
			Result.AuthorizedServerIp4 = Utf8ToFString(Value.authorizedServerIp4);
			Result.AuthorizedServerClientPort = Value.authorizedServerClientPort;
		}

		Result.bOk = true;
		return Result;
	}

	// Shared body for every auth-surface call. The three shapes differ only in which twin is invoked and how its
	// payload maps to an Unreal result, so both ride in as callables and everything else (exactly-once delivery,
	// the not-available guard, containing a throw at issue time and again inside the completion) is written once.
	// PayloadType is the twin's second callback argument: the typed response, or the raw JSON for the untyped ops.
	template <typename ResultType, typename PayloadType, typename InvokeFn, typename MapFn>
	FCrowdyCppRequestHandle RunAuthOp(crowdy::CrowdyClient* Client, const TSharedPtr<FRequestRegistry>& Registry,
		const TCHAR* Label, TFunction<void(ResultType)> OnDone, InvokeFn Invoke, MapFn Map)
	{
		ResultType Canceled;
		Canceled.ErrorMessage = FCrowdyCppClient::CanceledErrorMessage();
		const TIssuedRequest<ResultType> Issued =
			BeginRequest<ResultType>(Registry, Label, MoveTemp(OnDone), MoveTemp(Canceled));
		const TFunction<void(ResultType)>& FireOnce = Issued.Fire;

		if (!Client)
		{
			ResultType Result;
			Result.ErrorMessage = TEXT("CrowdyCPP client is not available");
			FireOnce(MoveTemp(Result));
			return Issued.Handle;
		}

		std::function<void(crowdy::graphql::GraphQLOutcome, PayloadType)> Cb =
			[FireOnce, Map, Label, Reason = Issued.FailureReason](crowdy::graphql::GraphQLOutcome Out, PayloadType Value)
			{
				*Reason = OutcomeFailureReason(Out);
				// The callback runs from Poll()/drain(), OUTSIDE the issue-time try below, so contain any throw here
				// and deliver a clean failure rather than let it escape into the engine ticker. The recovery delivery
				// is contained too: it runs the caller's completion from inside a handler that has no try around it,
				// and the stack above this point is the engine ticker.
				ResultType Failed;
				try
				{
					FireOnce(Map(Out, Value));
					return;
				}
				catch (const std::exception& Ex)
				{
					Failed.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
				}
				catch (...)
				{
					Failed.ErrorMessage = FString::Printf(TEXT("%s callback threw a non-standard exception"), Label);
				}

				try
				{
					FireOnce(MoveTemp(Failed));
				}
				catch (...)
				{
					UE_LOG(LogCrowdyCpp, Warning, TEXT("A %s completion threw while reporting a failure"), Label);
				}
			};

		const CrowdyCppTransport::FHandOffScope HandOff(*Issued.HandOffSeconds);
		try
		{
			Invoke(*Client, MoveTemp(Cb));
		}
		catch (const std::exception& Ex)
		{
			ResultType Result;
			Result.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
			FireOnce(MoveTemp(Result));
		}
		catch (...)
		{
			ResultType Result;
			Result.ErrorMessage = FString::Printf(TEXT("%s request threw a non-standard exception"), Label);
			FireOnce(MoveTemp(Result));
		}
		return Issued.Handle;
	}

	// The map for the auth calls whose payload has no typed counterpart: the outcome's data is already unwrapped to
	// the operation's single selected field, so it is handed back as-is for the caller to read.
	FCrowdyCppJsonValueResult MapUnwrappedJson(const crowdy::graphql::GraphQLOutcome& Out,
		const crowdy::graphql::Json& Data)
	{
		FCrowdyCppJsonValueResult Result;
		if (!Out.ok())
		{
			Result.ErrorMessage = OutcomeErrorMessage(Out);
			return Result;
		}
		Result.Value = UnwrappedJsonToUeValue(Data);
		if (!Result.Value.IsValid())
		{
			Result.ErrorMessage = TEXT("malformed or over-deep response data");
			return Result;
		}
		Result.bOk = true;
		return Result;
	}

	FCrowdyCppStringListResult MapStringList(const crowdy::graphql::GraphQLOutcome& Out,
		const std::vector<std::string>& Values)
	{
		FCrowdyCppStringListResult Result;
		if (!Out.ok())
		{
			Result.ErrorMessage = OutcomeErrorMessage(Out);
			return Result;
		}
		if (!Out.data.isArray())
		{
			// The twin builds its vector by walking the value, which is a no-op on anything that is not a list, so a
			// null answer would otherwise read as "this server offers no sign-in providers" rather than as a fault.
			Result.ErrorMessage = TEXT("malformed response: expected a list");
			return Result;
		}
		Result.Values.Reserve(static_cast<int32>(Values.size()));
		for (const std::string& Value : Values)
		{
			// An empty entry names no provider and would reach a sign-in call as a blank one.
			if (!Value.empty())
			{
				Result.Values.Add(Utf8ToFString(Value));
			}
		}
		Result.bOk = true;
		return Result;
	}

	FCrowdyCppBoolResult MapBool(const crowdy::graphql::GraphQLOutcome& Out, bool bValue)
	{
		FCrowdyCppBoolResult Result;
		if (!Out.ok())
		{
			Result.ErrorMessage = OutcomeErrorMessage(Out);
			return Result;
		}
		if (!Out.data.isBool())
		{
			// A missing or null answer reads as false through the twin, which is indistinguishable from the server
			// deliberately refusing. Those mean opposite things to a caller, so an absent answer is a fault here.
			Result.ErrorMessage = TEXT("malformed response: expected a boolean");
			return Result;
		}
		Result.bValue = bValue;
		Result.bOk = true;
		return Result;
	}

	// The three callback shapes the auth twins hand their payload back in.
	using FAuthPayloadCallback =
		std::function<void(crowdy::graphql::GraphQLOutcome, crowdy::domains::AuthResponse)>;
	using FAppTokenPayloadCallback =
		std::function<void(crowdy::graphql::GraphQLOutcome, crowdy::domains::AppTokenResponse)>;
	using FJsonPayloadCallback =
		std::function<void(crowdy::graphql::GraphQLOutcome, crowdy::graphql::Json)>;

	// Adapt a twin whose callback takes only the outcome to the two-argument shape RunAuthOp maps from. The JSON
	// value holds a shared reference to its own document, so the copy stays valid past the moved-from outcome.
	crowdy::graphql::GraphQLCallback AsJsonPayloadCallback(FJsonPayloadCallback Cb)
	{
		return [Cb = std::move(Cb)](crowdy::graphql::GraphQLOutcome Out)
		{
			crowdy::graphql::Json Data = Out.data;
			Cb(std::move(Out), std::move(Data));
		};
	}

	// Subscription handle ids are unique for the life of the process rather than per client. The client is rebuilt
	// whenever an endpoint changes, and a per-client counter would restart, so a handle held across that rebuild
	// would silently name a different caller's subscription on the new client and cancel that instead.
	uint64 NextSubscriptionHandleId()
	{
		static std::atomic<uint64> Next{1};
		return Next.fetch_add(1, std::memory_order_relaxed);
	}

	// The single-operation document to send, out of the generated operation sets. An operation the domain does not
	// define resolves to an empty view, which the caller refuses rather than sending a guessed document.
	//
	// The generated set no longer names an endpoint alongside the document: there is one API origin, so an operation
	// has nowhere else to be sent. What used to be read off that assignment is the BEARER, and that is now stated by
	// DomainDefaultPlane and OperationPlaneException below.
	std::string_view ResolveDocument(ECrowdyCppApiDomain Domain, const std::string& Op)
	{
		using namespace crowdy::gen;
		switch (Domain)
		{
		case ECrowdyCppApiDomain::GameModel:     return gameModel::documentFor(Op);
		case ECrowdyCppApiDomain::Auth:          return auth::documentFor(Op);
		case ECrowdyCppApiDomain::Users:         return users::documentFor(Op);
		case ECrowdyCppApiDomain::Apps:          return apps::documentFor(Op);
		case ECrowdyCppApiDomain::AppAccess:     return appAccess::documentFor(Op);
		case ECrowdyCppApiDomain::GameApps:      return gameApps::documentFor(Op);
		case ECrowdyCppApiDomain::Organizations: return organizations::documentFor(Op);
		case ECrowdyCppApiDomain::CrowdyStudio:  return crowdyStudio::documentFor(Op);
		case ECrowdyCppApiDomain::Teams:         return teams::documentFor(Op);
		case ECrowdyCppApiDomain::Channels:      return channels::documentFor(Op);
		case ECrowdyCppApiDomain::Avatars:       return avatars::documentFor(Op);
		case ECrowdyCppApiDomain::State:         return state::documentFor(Op);
		case ECrowdyCppApiDomain::Host:          return host::documentFor(Op);
		case ECrowdyCppApiDomain::Chunks:        return chunks::documentFor(Op);
		case ECrowdyCppApiDomain::Voxels:        return voxels::documentFor(Op);
		case ECrowdyCppApiDomain::Actors:        return actors::documentFor(Op);
		case ECrowdyCppApiDomain::ServerStatus:  return serverStatus::documentFor(Op);
		case ECrowdyCppApiDomain::Teleport:      return teleport::documentFor(Op);
		case ECrowdyCppApiDomain::Platform:      return platform::documentFor(Op);
		case ECrowdyCppApiDomain::Realtime:      return realtime::documentFor(Op);
		case ECrowdyCppApiDomain::Compute:       return compute::documentFor(Op);
		}
		return std::string_view();
	}

	// The token plane a domain's operations are issued under.
	//
	// This used to be read off the generated endpoint assignment, which named one URL per operation and so named a
	// bearer with it. The platform has since merged its two GraphQL origins onto one server and the generated set
	// stopped naming an endpoint at all, but the two tokens still mean different things: the same call under the
	// app-scoped bearer and under the session bearer answers about different things.
	//
	// So the plane is stated here instead. Every value reproduces the endpoint the same operation resolved to
	// before the merge, which is the arrangement the live gates were run against; CrowdyCppPlaneRoutingTests pins
	// that operation by operation. A domain whose operations did not agree on one plane before the merge is left
	// unset on purpose, and a caller that reaches one of those has to name the plane itself.
	TOptional<ECrowdyCppTokenPlane> DomainDefaultPlane(ECrowdyCppApiDomain Domain)
	{
		switch (Domain)
		{
		case ECrowdyCppApiDomain::GameModel:
		case ECrowdyCppApiDomain::GameApps:
		case ECrowdyCppApiDomain::CrowdyStudio:
		case ECrowdyCppApiDomain::Teams:
		case ECrowdyCppApiDomain::Channels:
		case ECrowdyCppApiDomain::Avatars:
		case ECrowdyCppApiDomain::State:
		case ECrowdyCppApiDomain::Host:
		case ECrowdyCppApiDomain::Chunks:
		case ECrowdyCppApiDomain::Voxels:
		case ECrowdyCppApiDomain::Actors:
		case ECrowdyCppApiDomain::Teleport:
		case ECrowdyCppApiDomain::Compute:
			return ECrowdyCppTokenPlane::Game;

		case ECrowdyCppApiDomain::Apps:
		case ECrowdyCppApiDomain::AppAccess:
		case ECrowdyCppApiDomain::Organizations:
		case ECrowdyCppApiDomain::Platform:
			return ECrowdyCppTokenPlane::Management;

		// Auth, Users and ServerStatus each served operations on both planes before the merge, so there is no one
		// answer for the domain to give. Most of their operations were reachable at either endpoint even then and
		// so have always required the caller to choose; the handful that did not are named individually below.
		default:
			return TOptional<ECrowdyCppTokenPlane>();
		}
	}

	// The operations whose plane disagrees with their own domain's. Each one resolved to exactly this endpoint
	// before the platform merged its origins, and every other operation in these three domains was reachable at
	// both endpoints even then, so those keep requiring the caller to say which plane it means rather than
	// inheriting a domain-wide guess.
	TOptional<ECrowdyCppTokenPlane> OperationPlaneException(ECrowdyCppApiDomain Domain, const std::string& Op)
	{
		switch (Domain)
		{
		case ECrowdyCppApiDomain::Auth:
			if (Op == "LogoutAllDevices") { return ECrowdyCppTokenPlane::Management; }
			break;

		case ECrowdyCppApiDomain::Users:
			if (Op == "UsersConnection" || Op == "SetOperator") { return ECrowdyCppTokenPlane::Management; }
			if (Op == "UpdateUserState") { return ECrowdyCppTokenPlane::Game; }
			break;

		case ECrowdyCppApiDomain::ServerStatus:
			if (Op == "ServerWithLeastClients" || Op == "GameClientBootstrap") { return ECrowdyCppTokenPlane::Game; }
			break;

		default:
			break;
		}
		return TOptional<ECrowdyCppTokenPlane>();
	}

	// Which admin authoring mutation a studio/automation input op issues.
	enum class EGameModelInputOp
	{
		Seed,
		UpsertAutomation,
		UpsertAutomationTrigger
	};

	// The operation name an input op's diagnostics are recorded under.
	const TCHAR* InputOpName(EGameModelInputOp Op)
	{
		switch (Op)
		{
		case EGameModelInputOp::Seed:
			return TEXT("GameModelSeed");
		case EGameModelInputOp::UpsertAutomation:
			return TEXT("GameModelUpsertAutomation");
		case EGameModelInputOp::UpsertAutomationTrigger:
			return TEXT("GameModelUpsertAutomationTrigger");
		}
		return TEXT("GameModelInputOp");
	}

	// Shared body for the three input-style authoring mutations (seedAsync, upsertAutomationAsync,
	// upsertAutomationTriggerAsync): each takes one input object and reports only success or failure. The input
	// string is the kit-emit output (our own, not forged), but the nesting pre-scan is kept before the recursive
	// UE reader for consistency with the other parse sites. OnDone is delivered exactly once, from Poll().
	FCrowdyCppRequestHandle RunGameModelInputOp(crowdy::CrowdyClient* Client,
		const TSharedPtr<FRequestRegistry>& Registry, EGameModelInputOp Op, const FString& InputJson,
		TFunction<void(FCrowdyCppStudioOpResult)> OnDone)
	{
		FCrowdyCppStudioOpResult Canceled;
		Canceled.ErrorMessage = FCrowdyCppClient::CanceledErrorMessage();
		const TIssuedRequest<FCrowdyCppStudioOpResult> Issued =
			BeginRequest<FCrowdyCppStudioOpResult>(Registry, InputOpName(Op), MoveTemp(OnDone), MoveTemp(Canceled));
		const TFunction<void(FCrowdyCppStudioOpResult)>& FireOnce = Issued.Fire;

		if (!Client)
		{
			FCrowdyCppStudioOpResult Result;
			Result.ErrorMessage = TEXT("CrowdyCPP client is not available");
			FireOnce(MoveTemp(Result));
			return Issued.Handle;
		}

		if (!CrowdyJsonSafety::IsNestingWithinLimit(InputJson))
		{
			FCrowdyCppStudioOpResult Result;
			Result.ErrorMessage = TEXT("deploy input JSON is too deeply nested");
			FireOnce(MoveTemp(Result));
			return Issued.Handle;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InputJson);
		TSharedPtr<FJsonObject> InputObject;
		if (!FJsonSerializer::Deserialize(Reader, InputObject) || !InputObject.IsValid())
		{
			FCrowdyCppStudioOpResult Result;
			Result.ErrorMessage = TEXT("deploy input JSON is not a JSON object");
			FireOnce(MoveTemp(Result));
			return Issued.Handle;
		}

		// The seed input already carries appId as a JSON string (the kit emit stamped it); ObjectToJVal preserves
		// that string and re-parses whole numbers as integers, exactly as the runtime-op path does.
		const crowdy::graphql::JVal Input = ObjectToJVal(InputObject);
		crowdy::graphql::GraphQLCallback Cb =
			[FireOnce, Reason = Issued.FailureReason](crowdy::graphql::GraphQLOutcome Out)
			{
				*Reason = OutcomeFailureReason(Out);
				// The callback runs from Poll()/drain(), OUTSIDE the issue-time try below, so contain any throw here
				// and deliver a clean failure rather than let it escape into the engine ticker.
				try
				{
					FCrowdyCppStudioOpResult Result;
					Result.bOk = Out.ok();
					if (!Result.bOk)
					{
						Result.ErrorMessage = OutcomeErrorMessage(Out);
					}
					FireOnce(MoveTemp(Result));
				}
				catch (...)
				{
					FCrowdyCppStudioOpResult Failed;
					Failed.ErrorMessage = TEXT("Game Model authoring op callback threw");
					FireOnce(MoveTemp(Failed));
				}
			};

		const CrowdyCppTransport::FHandOffScope HandOff(*Issued.HandOffSeconds);
		try
		{
			crowdy::domains::GameModelAPI& GameModel = Client->gameModel();
			switch (Op)
			{
			case EGameModelInputOp::Seed:
				GameModel.seedAsync(Input, Cb);
				break;
			case EGameModelInputOp::UpsertAutomation:
				GameModel.upsertAutomationAsync(Input, Cb);
				break;
			case EGameModelInputOp::UpsertAutomationTrigger:
				GameModel.upsertAutomationTriggerAsync(Input, Cb);
				break;
			}
		}
		catch (const std::exception& Ex)
		{
			FCrowdyCppStudioOpResult Result;
			Result.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
			FireOnce(MoveTemp(Result));
		}
		catch (...)
		{
			FCrowdyCppStudioOpResult Result;
			Result.ErrorMessage = TEXT("Game Model authoring op threw a non-standard exception");
			FireOnce(MoveTemp(Result));
		}
		return Issued.Handle;
	}

	// Construct the CrowdyCPP client, containing any construction exception at the
	// boundary so it never escapes into the engine. Returns null on failure.
	TUniquePtr<crowdy::CrowdyClient> TryConstructClient(crowdy::ClientConfig&& Config)
	{
		try
		{
			return MakeUnique<crowdy::CrowdyClient>(std::move(Config));
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("CrowdyClient construction failed: %hs"), Ex.what());
			return nullptr;
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("CrowdyClient construction threw a non-standard exception"));
			return nullptr;
		}
	}

	// Whether an endpoint is unencrypted and not on this machine. Loopback is the one place a plain-HTTP endpoint is
	// a legitimate development setup rather than a credential leak, so it is the only exemption.
	bool IsInsecureRemoteEndpoint(const std::string& Endpoint)
	{
		const FString Url = Utf8ToFString(Endpoint);
		if (!Url.StartsWith(TEXT("http://")) && !Url.StartsWith(TEXT("ws://")))
		{
			return false;
		}

		FString Authority = Url.RightChop(Url.StartsWith(TEXT("http://")) ? 7 : 5);
		int32 Cut = INDEX_NONE;
		if (Authority.FindChar(TEXT('/'), Cut))
		{
			Authority.LeftInline(Cut);
		}
		// Strip any userinfo and the port so only the host is compared.
		if (Authority.FindLastChar(TEXT('@'), Cut))
		{
			Authority.RightChopInline(Cut + 1);
		}
		if (Authority.FindLastChar(TEXT(':'), Cut) && !Authority.EndsWith(TEXT("]")))
		{
			Authority.LeftInline(Cut);
		}
		Authority.RemoveFromStart(TEXT("["));
		Authority.RemoveFromEnd(TEXT("]"));

		return !(Authority.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
			|| Authority == TEXT("127.0.0.1")
			|| Authority == TEXT("::1"));
	}

	// Build the subscription client that carries the game bearer alone, sharing the client's completion pump so
	// Poll() drains subscription callbacks along with everything else. Returns null when it could not be built, in
	// which case Subscribe reports itself unavailable rather than the client failing to construct at all.
	std::shared_ptr<crowdy::graphql::GraphQLSubscriptionClient> TryConstructSubscriptionClient(
		crowdy::CrowdyClient& Client, std::shared_ptr<crowdy::graphql::IWebSocketTransport> Transport,
		std::shared_ptr<crowdy::graphql::AuthState>& OutAuth)
	{
		if (!Transport)
		{
			return nullptr;
		}
		try
		{
			crowdy::graphql::GraphQLSubscriptionClientConfig Config;
			// The Game API serves subscriptions at the same endpoint as its queries, so the socket URL is that
			// endpoint under a ws scheme. Deriving it from the resolved HTTP endpoint rather than configuring a
			// second URL matches what the console does when it writes the project's WebSocket setting, and leaves
			// no second value that can drift out of step with the first.
			Config.endpoint = Client.graphqlClient().endpoint();
			Config.endpointKind = crowdy::graphql::GraphQLWebSocketEndpointKind::Complete;

			// The bearer travels inside the first message on this socket rather than as a request header, so an
			// unencrypted endpoint would put the gameplay token on the wire in the clear for anyone on the path to
			// lift and replay. Refused outright rather than warned about, because a warning in a log is not
			// something a shipped build's players will ever see. Loopback is exempt so local development, which is
			// the only place a plain-HTTP endpoint is legitimate, still works.
			if (IsInsecureRemoteEndpoint(Config.endpoint))
			{
				UE_LOG(LogCrowdyCpp, Error,
					TEXT("The Game API endpoint is not encrypted, so GraphQL subscriptions are disabled: opening one would send the gameplay token in clear text. Point the project at an https endpoint to enable them."));
				return nullptr;
			}

			// Named rather than defaulted. The reconnect delay is the one that matters: a server that accepts a
			// connection and then drops it resets the attempt counter each time, so the attempt cap never bites and
			// the delay is all that stands between that and a reconnect loop at full speed.
			Config.options.initialReconnectDelayMs = 1000;
			Config.options.maxReconnectDelayMs = 30000;

			OutAuth = std::make_shared<crowdy::graphql::AuthState>();
			return std::make_shared<crowdy::graphql::GraphQLSubscriptionClient>(
				std::move(Config), std::move(Transport), OutAuth, Client.graphqlClient().dispatcher());
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("GraphQL subscription client construction failed: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Error, TEXT("GraphQL subscription client construction threw a non-standard exception"));
		}
		OutAuth.reset();
		return nullptr;
	}
}

bool FCrowdyCppClient::FImpl::SendRetryable(const TSharedRef<FBusyRetry>& Retry)
{
	crowdy::CrowdyClient* Raw = Use(Retry->Plane);
	if (!Raw)
	{
		Retry->Deliver(UnsentOutcome("CrowdyCPP client is not available"));
		return false;
	}

	const TWeakPtr<FRequestRegistry> WeakRegistry = Requests;
	const CrowdyCppTransport::FHandOffScope HandOff(*Retry->HandOffSeconds);
	try
	{
		Retry->Send(*Raw, [Retry, WeakRegistry](crowdy::graphql::GraphQLOutcome Out)
		{
			const TSharedPtr<FRequestRegistry> Registry = WeakRegistry.Pin();
			if (Registry.IsValid() && TryQueueBusyRetry(*Registry, Retry, Out))
			{
				return;
			}
			Retry->Deliver(std::move(Out));
		});
		return true;
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("%s request threw while being sent: %hs"), *OperationOf(*Retry), Ex.what());
		Retry->Deliver(UnsentOutcome(Ex.what()));
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("%s request threw a non-standard exception while being sent"),
			*OperationOf(*Retry));
		Retry->Deliver(UnsentOutcome(std::string(TCHAR_TO_UTF8(
			*FString::Printf(TEXT("operation '%s' threw a non-standard exception"), *OperationOf(*Retry))))));
	}
	return false;
}

FString FCrowdyCppClient::FImpl::OperationOf(const FBusyRetry& Retry) const
{
	return Requests.IsValid() && Requests->OpSlots.IsValidIndex(Retry.OpSlot)
		? Requests->OpSlots[Retry.OpSlot].Operation : FString(TEXT("An SDK"));
}

void FCrowdyCppClient::FImpl::SendDueRetries()
{
	if (!Requests.IsValid() || Requests->WaitingRetries.IsEmpty())
	{
		return;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	TArray<TSharedRef<FBusyRetry>> Due;
	for (int32 Index = Requests->WaitingRetries.Num() - 1; Index >= 0; --Index)
	{
		if (Requests->WaitingRetries[Index]->DueSeconds > NowSeconds)
		{
			continue;
		}
		Due.Add(Requests->WaitingRetries[Index]);
		Requests->WaitingRetries.RemoveAtSwap(Index, EAllowShrinking::No);
	}

	for (const TSharedRef<FBusyRetry>& Retry : Due)
	{
		// A request canceled while it waited has already been delivered and is not sent again.
		if (!IsUsable() || !Requests->Pending.Contains(Retry->HandleId))
		{
			continue;
		}
		++Retry->Progress->Retries;
		Retry->Progress->AttemptSeconds = NowSeconds;
		*Retry->HandOffSeconds = 0.0;
		if (SendRetryable(Retry) && Requests->OpSlots.IsValidIndex(Retry->OpSlot))
		{
			++Requests->OpSlots[Retry->OpSlot].Retries;
		}
	}
}

bool CrowdyCppIsBusyRetryEnabled()
{
	return CVarRetryBusy.GetValueOnGameThread() != 0;
}

TOptional<double> CrowdyCppBusyRetryDelaySeconds(int32 RetryIndex, TOptional<int64> RetryAfterMs)
{
	constexpr double MinDelaySeconds = 0.1;
	constexpr double MaxDelaySeconds = 5.0;
	if (RetryAfterMs.IsSet() && RetryAfterMs.GetValue() >= 0)
	{
		const double NamedSeconds = static_cast<double>(RetryAfterMs.GetValue()) / 1000.0;
		if (NamedSeconds > MaxDelaySeconds)
		{
			return TOptional<double>();
		}
		return FMath::Max(NamedSeconds, MinDelaySeconds) * FMath::FRandRange(1.0, 1.2);
	}
	const double BaseSeconds = FMath::FRandRange(MinDelaySeconds, 2.0 * MinDelaySeconds);
	return FMath::Min(BaseSeconds * static_cast<double>(1 << FMath::Clamp(RetryIndex, 0, 8)), MaxDelaySeconds);
}

FCrowdyCppClient::FCrowdyCppClient()
	: Impl(MakeUnique<FImpl>())
{
}

FCrowdyCppClient::~FCrowdyCppClient()
{
	// Releasing the client is a complete teardown on its own: Close() fences the underlying transports and then
	// delivers every request still pending as canceled, so an owner that simply drops its last reference cannot
	// strand a caller waiting on a completion that will now never come from the server.
	Close();
}

bool CrowdyCppIsModelRefusalCode(const FString& Code)
{
	if (Code.IsEmpty())
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*Code);
	return crowdy::studio::isModelRefusalCode(std::string_view(Utf8.Get(), Utf8.Length()));
}

const FString& FCrowdyCppClient::CanceledErrorMessage()
{
	static const FString Message = TEXT("request canceled");
	return Message;
}

void FCrowdyCppClient::GetStats(TArray<FCrowdyCppOpStats>& OutOps, FCrowdyCppTransportStats& OutTransport) const
{
	OutOps.Reset();
	OutTransport = FCrowdyCppTransportStats();
	if (!Impl)
	{
		return;
	}

	if (Impl->TransportCounters)
	{
		OutTransport.RequestBytes = Impl->TransportCounters->RequestBytes.load(std::memory_order_relaxed);
		OutTransport.ResponseBytes = Impl->TransportCounters->ResponseBytes.load(std::memory_order_relaxed);
		OutTransport.Responses = Impl->TransportCounters->Responses.load(std::memory_order_relaxed);
		OutTransport.InFlight = Impl->TransportCounters->InFlight.load(std::memory_order_relaxed);
		OutTransport.PeakInFlight = Impl->TransportCounters->PeakInFlight.load(std::memory_order_relaxed);
	}

	if (!Impl->Requests.IsValid())
	{
		return;
	}
	for (const FRequestRegistry::FOpSlot& Slot : Impl->Requests->OpSlots)
	{
		if (Slot.Calls == 0)
		{
			continue;
		}
		FCrowdyCppOpStats& Op = OutOps.AddDefaulted_GetRef();
		Op.Operation = Slot.Operation;
		Op.Calls = Slot.Calls;
		Op.Failures = Slot.Failures;
		Op.Retries = Slot.Retries;
		Op.RecoveredByRetry = Slot.RecoveredByRetry;
		Op.MaxMs = Slot.MaxMs;
		Slot.Latency.CopyOldestFirst(Op.RecentLatenciesMs);
		Slot.Sdk.CopyOldestFirst(Op.RecentSdkMs);
		Slot.Http.CopyOldestFirst(Op.RecentHttpMs);
		Op.FailureReasons = Slot.FailureReasons.Array();
	}
}

void FCrowdyCppClient::ResetStats()
{
	if (!Impl)
	{
		return;
	}
	if (Impl->TransportCounters)
	{
		Impl->TransportCounters->RequestBytes.store(0, std::memory_order_relaxed);
		Impl->TransportCounters->ResponseBytes.store(0, std::memory_order_relaxed);
		Impl->TransportCounters->Responses.store(0, std::memory_order_relaxed);
		// Requests still in flight stay counted, so the peak restarts from them rather than from zero.
		Impl->TransportCounters->PeakInFlight.store(Impl->TransportCounters->InFlight.load(std::memory_order_relaxed),
			std::memory_order_relaxed);
	}
	if (!Impl->Requests.IsValid())
	{
		return;
	}
	for (FRequestRegistry::FOpSlot& Slot : Impl->Requests->OpSlots)
	{
		Slot.Latency.Reset();
		Slot.Sdk.Reset();
		Slot.Http.Reset();
		Slot.FailureReasons.Reset();
		Slot.Calls = 0;
		Slot.Failures = 0;
		Slot.Retries = 0;
		Slot.RecoveredByRetry = 0;
		Slot.MaxMs = 0.0;
	}
}

TSharedPtr<FCrowdyCppClient> FCrowdyCppClient::Make(const FCrowdyCppClientConfig& InConfig)
{
	TSharedPtr<FCrowdyCppClient> Wrapper = MakeShareable(new FCrowdyCppClient());

	// A config naming no origin at all no longer fails: CrowdyCPP fills in the origin for the TIER THIS SNAPSHOT WAS
	// VENDORED FROM (see ThirdParty/CrowdyCPP/VENDOR.txt), which is a working client pointed somewhere nobody chose.
	// It is a reasonable default and a terrible silent one, so say which host is about to be dialled and why.
	if (InConfig.ApiUrl.IsEmpty())
	{
		UE_LOG(LogCrowdyCpp, Warning,
			TEXT("[CrowdyCpp] no API origin was configured, so this client will use the vendored SDK's '%s' tier default (%s). Set the Game API URL, or the shared origin, to choose the tier yourself."),
			UTF8_TO_TCHAR(crowdy::kDefaultTier), UTF8_TO_TCHAR(crowdy::kDefaultHttpOrigin));
	}

	crowdy::ClientConfig Config;
	Config.httpUrl = std::string(TCHAR_TO_UTF8(*InConfig.ApiUrl));
	Config.discoveryUrl = std::string(TCHAR_TO_UTF8(*InConfig.DiscoveryUrl));
	Config.rediscover = FImpl::MakeRediscoverCallback(Wrapper->Impl->Rediscover);
	Config.asyncTransport = CrowdyCppTransport::MakeFHttpTransport(Wrapper->Impl->TransportCounters);
	// config.transport (the sync path) stays null; the async path never derefs it.
	// Name the provider rather than relying on the client's build-flag fallback,
	// which resolves to the always-failing provider if the flag is ever lost.
	Config.crypto = &crowdy::core::opensslCrypto();

	TUniquePtr<crowdy::CrowdyClient> Client = TryConstructClient(std::move(Config));
	if (!Client)
	{
		return nullptr;
	}
	Wrapper->Impl->Client = MoveTemp(Client);
	Wrapper->Impl->SubscriptionClient = TryConstructSubscriptionClient(*Wrapper->Impl->Client,
		CrowdyCppTransport::MakeFWebSocketTransport(), Wrapper->Impl->SubscriptionAuth);
	Wrapper->Impl->InstallRepeatedFailureHandler();
	Wrapper->Impl->LastSeenEndpoint = Utf8ToFString(Wrapper->Impl->Client->graphqlClient().endpoint());
	return Wrapper;
}

#if WITH_DEV_AUTOMATION_TESTS
TSharedPtr<FCrowdyCppClient> FCrowdyCppClient::MakeForTest(const FString& CannedResponseBody, int32 HttpStatus,
	const FCrowdyCppClientConfig& InConfig)
{
	std::shared_ptr<CrowdyCppTransport::FCannedRequestCapture> Capture =
		std::make_shared<CrowdyCppTransport::FCannedRequestCapture>();

	TSharedPtr<FCrowdyCppClient> Wrapper = MakeShareable(new FCrowdyCppClient());

	crowdy::ClientConfig Config;
	Config.httpUrl = std::string(TCHAR_TO_UTF8(*InConfig.ApiUrl));
	Config.discoveryUrl = std::string(TCHAR_TO_UTF8(*InConfig.DiscoveryUrl));
	Config.rediscover = FImpl::MakeRediscoverCallback(Wrapper->Impl->Rediscover);
	Config.asyncTransport = CrowdyCppTransport::MakeCannedTransport(std::string(TCHAR_TO_UTF8(*CannedResponseBody)),
		static_cast<int>(HttpStatus), Capture, Wrapper->Impl->TransportCounters);
	Config.crypto = &crowdy::core::opensslCrypto();

	TUniquePtr<crowdy::CrowdyClient> Client = TryConstructClient(std::move(Config));
	if (!Client)
	{
		return nullptr;
	}
	Wrapper->Impl->Client = MoveTemp(Client);
	Wrapper->Impl->TestCapture = MoveTemp(Capture);

	// No socket either: the scripted server replaces it, so a test drives the graphql-transport-ws handshake by hand.
	Wrapper->Impl->TestWebSocket = std::make_shared<CrowdyCppTransport::FScriptedWebSocketServer>();
	Wrapper->Impl->SubscriptionClient = TryConstructSubscriptionClient(*Wrapper->Impl->Client,
		CrowdyCppTransport::MakeScriptedWebSocketTransport(Wrapper->Impl->TestWebSocket),
		Wrapper->Impl->SubscriptionAuth);

	Wrapper->Impl->InstallRepeatedFailureHandler();
	Wrapper->Impl->LastSeenEndpoint = Utf8ToFString(Wrapper->Impl->Client->graphqlClient().endpoint());

	// The canned transport ignores the bearer, so seed one rather than warn every test about a missing token.
	Wrapper->Impl->GameToken = TEXT("test-game-token");
	Wrapper->Impl->ApplySubscriptionToken();
	return Wrapper;
}

bool FCrowdyCppClient::GetLastTestRequest(FString& OutUrl, FString& OutAuthorizationHeader) const
{
	if (!Impl || !Impl->TestCapture || !Impl->TestCapture->bHasRequest)
	{
		return false;
	}
	OutUrl = Utf8ToFString(Impl->TestCapture->Url);
	OutAuthorizationHeader = Utf8ToFString(Impl->TestCapture->Authorization);
	return true;
}

bool FCrowdyCppClient::GetLastTestRequestBody(FString& OutBody) const
{
	if (!Impl || !Impl->TestCapture || !Impl->TestCapture->bHasRequest)
	{
		return false;
	}
	OutBody = Utf8ToFString(Impl->TestCapture->Body);
	return true;
}

void FCrowdyCppClient::SetTestStampsHandOff(bool bStamp)
{
	if (Impl && Impl->TestCapture)
	{
		Impl->TestCapture->bStampHandOff = bStamp;
	}
}

void FCrowdyCppClient::SetTestResponseScript(TArray<TPair<int32, FString>> Responses)
{
	if (!Impl || !Impl->TestCapture)
	{
		return;
	}
	Impl->TestCapture->ScriptedResponses.clear();
	Impl->TestCapture->NextScriptedResponse = 0;
	for (const TPair<int32, FString>& Response : Responses)
	{
		Impl->TestCapture->ScriptedResponses.emplace_back(
			static_cast<int>(Response.Key), std::string(TCHAR_TO_UTF8(*Response.Value)));
	}
}

void FCrowdyCppClient::SetTestOnRequest(TFunction<void(const FString& Url)> Hook)
{
	if (!Impl || !Impl->TestCapture)
	{
		return;
	}
	if (!Hook)
	{
		Impl->TestCapture->OnRequest = nullptr;
		return;
	}
	Impl->TestCapture->OnRequest = [Hook = MoveTemp(Hook)](const std::string& Url)
	{
		Hook(Utf8ToFString(Url));
	};
}
#endif

void FCrowdyCppClient::SetGameToken(const FString& Token)
{
	if (Impl)
	{
		Impl->GameToken = Token;
		Impl->ApplySubscriptionToken();
	}
}

void FCrowdyCppClient::SetManagementToken(const FString& Token)
{
	if (Impl)
	{
		Impl->ManagementToken = Token;
	}
}

void FCrowdyCppClient::Close()
{
	if (!Impl || Impl->bClosed)
	{
		return;
	}
	Impl->bClosed = true;

	// Ended before the pump is fenced, so each caller is told its stream is over while its callback can still run.
	//
	// The order of the two closes below is load-bearing and not interchangeable. Closing the subscription client
	// first joins its timer thread while the shared completion pump is still open; closing the underlying client
	// first would shut that pump down, and the subscription client would then destroy the callers' callbacks on its
	// own timer thread rather than here, taking whatever those callbacks captured with them.
	Impl->FailLiveSubscriptions(CanceledErrorMessage());
	if (Impl->SubscriptionClient)
	{
		try
		{
			Impl->SubscriptionClient->close();
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("GraphQL subscription client close threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("GraphQL subscription client close threw a non-standard exception"));
		}
	}
	// Released here rather than left to the destructor: closing joins its timer thread, and the socket it owns must
	// not outlive the teardown that fenced everything else.
	Impl->SubscriptionClient.reset();
	if (Impl->SubscriptionAuth)
	{
		Impl->SubscriptionAuth->clearToken();
		Impl->SubscriptionAuth.reset();
	}

	// The timer thread is joined by now, so nothing else can park a socket, and this is still the game thread.
	// Without it the sockets those closes just dropped would be waiting on a queued task that teardown cannot
	// promise will ever run, and a socket released after the engine's WebSocket support is gone is a crash.
	CrowdyCppTransport::FlushPendingWebSocketReleases();

	if (Impl->Client)
	{
		// close() fences retained completions, so a callback queued by a request already in flight will not run after
		// this returns. Contain any throw so a teardown path never propagates one into the engine.
		try
		{
			// Drop the bearer the last request installed first. Closing shuts the transports down but leaves the
			// client's own copy of the token resident, and a disposed client must not keep credential material alive.
			Impl->Client->setToken(std::string());
			Impl->Client->close();
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("CrowdyClient close threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("CrowdyClient close threw a non-standard exception"));
		}
	}
	// Drop the bearers with the client: a disposed client must not keep token material alive.
	Impl->GameToken.Empty();
	Impl->ManagementToken.Empty();

	// The fenced completions above would otherwise be dropped un-run, which is the one thing a caller cannot
	// recover from, so each is delivered as canceled here. This runs after bClosed is set, so a completion that
	// reissues from inside its own delivery is refused immediately rather than queued on a disposed client.
	if (Impl->Requests.IsValid())
	{
		Impl->Requests->DrainAsCanceled();
	}
}

bool FCrowdyCppClient::Cancel(FCrowdyCppRequestHandle Handle)
{
	if (!Impl || !Impl->Requests.IsValid() || !Handle.IsValid())
	{
		return false;
	}

	// Take the entry out of the map before delivering, so the completion (which deregisters itself) cannot destroy
	// the closure it is running inside, and a re-entrant cancel of the same handle finds nothing.
	TSharedPtr<FPendingCompletion> Entry;
	if (!Impl->Requests->Pending.RemoveAndCopyValue(Handle.Id, Entry) || !Entry.IsValid())
	{
		return false;
	}
	Impl->Requests->WaitingRetries.RemoveAllSwap(
		[&Handle](const TSharedRef<FBusyRetry>& Retry) { return Retry->HandleId == Handle.Id; });
	if (Entry->DeliverCanceled)
	{
		FRequestRegistry::DeliverCanceledContained(*Entry);
	}
	return true;
}

int32 FCrowdyCppClient::CancelAll()
{
	if (!Impl || !Impl->Requests.IsValid())
	{
		return 0;
	}
	return Impl->Requests->DrainAsCanceled();
}

int32 FCrowdyCppClient::NumPendingRequests() const
{
	return Impl && Impl->Requests.IsValid() ? Impl->Requests->Pending.Num() : 0;
}

#if WITH_DEV_AUTOMATION_TESTS
int32 FCrowdyCppClient::NumTestWaitingRetries() const
{
	return Impl && Impl->Requests.IsValid() ? Impl->Requests->WaitingRetries.Num() : 0;
}

double FCrowdyCppClient::GetTestNextRetryDueSeconds() const
{
	if (!Impl || !Impl->Requests.IsValid() || Impl->Requests->WaitingRetries.IsEmpty())
	{
		return 0.0;
	}
	double Soonest = Impl->Requests->WaitingRetries[0]->DueSeconds;
	for (const TSharedRef<FBusyRetry>& Retry : Impl->Requests->WaitingRetries)
	{
		Soonest = FMath::Min(Soonest, Retry->DueSeconds);
	}
	return Soonest;
}
#endif

void FCrowdyCppClient::Poll()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_ApiPoll);
	// Polling a closed client is a no-op rather than an error: an owner may tick once more before it drops us.
	if (Impl && Impl->Client && !Impl->bClosed)
	{
		Impl->Client->poll();

		// After the pump, never during it: a subscription that ended is reaped here rather than from inside the
		// callback that ended it, where cancelling would re-enter the subscription client mid-dispatch.
		Impl->ReapFinishedSubscriptions();

		// Also after the pump, because a datacenter redirect is applied while a completion is being delivered.
		Impl->FollowEndpointMove();

		Impl->SendDueRetries();
	}
}

FCrowdyCppRequestHandle FCrowdyCppClient::ReadContainerState(int64 AppId, const FString& ContainerId,
	TFunction<void(FCrowdyCppContainerStateResult)> OnDone)
{
	// Deliver exactly once across every path: the async completion, the catch below, the not-constructed guard, and
	// a cancellation. Poll() drains on the game thread and every other path runs synchronously on the same thread,
	// so the fired flag needs no lock.
	FCrowdyCppContainerStateResult Canceled;
	Canceled.ErrorMessage = CanceledErrorMessage();
	const TIssuedRequest<FCrowdyCppContainerStateResult> Issued = BeginRequest<FCrowdyCppContainerStateResult>(
		Impl ? Impl->Requests : nullptr, TEXT("GameModelContainerState"), MoveTemp(OnDone), MoveTemp(Canceled));
	const TFunction<void(FCrowdyCppContainerStateResult)>& FireOnce = Issued.Fire;

	if (!Impl || !Impl->IsUsable())
	{
		FCrowdyCppContainerStateResult Result;
		Result.ErrorMessage = TEXT("CrowdyCPP client is not available");
		FireOnce(MoveTemp(Result));
		return Issued.Handle;
	}

	// appId is a BigInt scalar: the domain twin emits the string we pass here.
	const std::string AppIdStr = std::string(TCHAR_TO_UTF8(*LexToString(AppId)));
	const std::string ContainerIdStr = std::string(TCHAR_TO_UTF8(*ContainerId));

	const TSharedRef<FBusyRetry> Retry = MakeBusyRetry(Issued, ECrowdyCppTokenPlane::Game, EBusyRetryKind::Query);
	Retry->Send = [AppIdStr, ContainerIdStr](crowdy::CrowdyClient& Client, crowdy::graphql::GraphQLCallback Cb)
	{
		Client.gameModel().containerStateAsync(AppIdStr, ContainerIdStr, std::move(Cb));
	};
	Retry->Deliver = [FireOnce, Reason = Issued.FailureReason](crowdy::graphql::GraphQLOutcome Out)
	{
		*Reason = OutcomeFailureReason(Out);
		// The callback runs from Poll()/drain(), outside any issue-time try, so contain any throw here and deliver a
		// clean failure rather than let it escape into the engine ticker.
		try
		{
			FCrowdyCppContainerStateResult Result;
			if (!Out.ok())
			{
				Result.ErrorMessage = OutcomeErrorMessage(Out);
				FireOnce(MoveTemp(Result));
				return;
			}

			// out.data is the unwrapped gameModelContainerState object, or a
			// JSON null when the container does not exist; its propertiesJson
			// field is the container state as a JSON string. bOk stays false
			// unless that string decodes to an object, so an absent container
			// and an unparseable one are both reported the same way: no state.
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_GM_DecodeResponse);
				const std::string Props = Out.data["propertiesJson"].asString();
				const FString PropertiesJson = Utf8ToFString(Props);
				if (!PropertiesJson.IsEmpty() && CrowdyJsonSafety::IsNestingWithinLimit(PropertiesJson))
				{
					// The nesting pre-scan bounds the untrusted string before the
					// recursive UE deserializer builds a DOM a forged deep payload
					// could overflow on teardown.
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PropertiesJson);
					TSharedPtr<FJsonObject> Parsed;
					if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
					{
						Result.bOk = true;
						Result.State = Parsed;
					}
				}
			}

			if (!Result.bOk)
			{
				Result.ErrorMessage = TEXT("container not found or no readable state");
			}
			FireOnce(MoveTemp(Result));
		}
		catch (...)
		{
			FCrowdyCppContainerStateResult Failed;
			Failed.ErrorMessage = TEXT("gameModelContainerState callback threw");
			FireOnce(MoveTemp(Failed));
		}
	};
	Impl->SendRetryable(Retry);
	return Issued.Handle;
}

FCrowdyCppRequestHandle FCrowdyCppClient::InvokeFunction(int64 AppId, const FString& FunctionName,
	const FString& SelfContainerId, const FString& SessionId, const FString& ParamsJson,
	TFunction<void(FCrowdyCppInvokeResult)> OnDone)
{
	FCrowdyCppInvokeResult Canceled;
	Canceled.ErrorMessage = CanceledErrorMessage();
	const TIssuedRequest<FCrowdyCppInvokeResult> Issued = BeginRequest<FCrowdyCppInvokeResult>(
		Impl ? Impl->Requests : nullptr, TEXT("GameModelInvoke"), MoveTemp(OnDone), MoveTemp(Canceled));
	const TFunction<void(FCrowdyCppInvokeResult)>& FireOnce = Issued.Fire;

	crowdy::CrowdyClient* Client = Impl ? Impl->Use(ECrowdyCppTokenPlane::Game) : nullptr;
	if (!Client)
	{
		FCrowdyCppInvokeResult Result;
		Result.ErrorMessage = TEXT("CrowdyCPP client is not available");
		FireOnce(MoveTemp(Result));
		return Issued.Handle;
	}

	// appId is a BigInt scalar (a JSON string, never a number); paramsJson is already the compact-serialized
	// params object; sessionId is omitted for app-global scope. This mirrors BuildInvokeVariables.
	crowdy::graphql::JVal Input;
	Input["appId"] = std::string(TCHAR_TO_UTF8(*LexToString(AppId)));
	Input["functionName"] = std::string(TCHAR_TO_UTF8(*FunctionName));
	Input["selfContainerId"] = std::string(TCHAR_TO_UTF8(*SelfContainerId));
	if (!SessionId.IsEmpty())
	{
		Input["sessionId"] = std::string(TCHAR_TO_UTF8(*SessionId));
	}
	Input["paramsJson"] = std::string(TCHAR_TO_UTF8(*ParamsJson));

	const CrowdyCppTransport::FHandOffScope HandOff(*Issued.HandOffSeconds);
	try
	{
		Client->gameModel().invokeAsync(Input,
			[FireOnce, Reason = Issued.FailureReason](crowdy::graphql::GraphQLOutcome Out)
			{
				*Reason = OutcomeFailureReason(Out);
				// The callback runs from Poll()/drain(), OUTSIDE the issue-time try below, so contain any throw
				// here (e.g. bad_alloc decoding a forged response) and deliver a clean failure rather than let it
				// escape into the engine ticker.
				try
				{
					FCrowdyCppInvokeResult Result;
					if (!Out.ok())
					{
						// A transport or GraphQL failure: the request never yielded a GmInvokeResult, so this is
						// NOT a rolled-back invoke. bTransportOk stays false, keeping a network/authorization
						// error distinct from a committed-but-failed function. This is also where the platform's
						// overload refusal lands, which is the one failure a caller SHOULD repeat, so the
						// attribution is read here rather than left for the caller to guess at.
						Result.ErrorMessage = OutcomeErrorMessage(Out);
						ReadOutcomeFault(Out, Result);
						FireOnce(MoveTemp(Result));
						return;
					}

					// out.data is the unwrapped GmInvokeResult. Re-serialize it and decode with the UE JSON
					// reader, then read the fields with the UE accessors (TryGetBoolField / TryGetStringField
					// / TryGetArrayField), so field extraction, type coercion, and the non-object-element skip
					// go through one shared code path. This also keeps the mutations walk O(n): a yyjson
					// index walk over a non-flat array (an array of objects) is O(n^2). The dumped object is
					// nesting-guarded before the recursive UE deserializer, since a forged deep value would
					// otherwise overflow on teardown (yyjson parsed it iteratively; the UE re-parse is recursive).
					TSharedPtr<FJsonObject> Parsed;
					{
						TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_GM_DecodeResponse);
						const FString InvokeJson = Utf8ToFString(Out.data.dump());
						if (CrowdyJsonSafety::IsNestingWithinLimit(InvokeJson))
						{
							const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InvokeJson);
							FJsonSerializer::Deserialize(Reader, Parsed);
						}
					}
					if (!Parsed.IsValid())
					{
						// The server was reached (out.ok()) but returned no decodable GmInvokeResult object: a
						// malformed response, not a rolled-back invoke, so bTransportOk stays false because the
						// gameModelInvoke object is required.
						Result.ErrorMessage = TEXT("malformed response: missing gameModelInvoke");
						FireOnce(MoveTemp(Result));
						return;
					}

					// A real GmInvokeResult object: transport-ok regardless of the logic outcome (success is
					// false for a rolled-back invoke, with the reason in fault).
					Result.bTransportOk = true;

					bool bInvokeSuccess = false;
					Parsed->TryGetBoolField(TEXT("success"), bInvokeSuccess);
					Result.bSuccess = bInvokeSuccess;
					Parsed->TryGetStringField(TEXT("returnValueJson"), Result.ReturnValueJson);
					Parsed->TryGetStringField(TEXT("errorMessage"), Result.ErrorMessage);

					// The in-band channel: an authority denial or an evaluation failure. Absent when success is
					// true. Unlike the thrown channel above, retryable is taken as the server sent it, because a
					// fault object IS the attribution.
					const TSharedPtr<FJsonObject>* FaultPtr = nullptr;
					if (Parsed->TryGetObjectField(TEXT("fault"), FaultPtr) && FaultPtr && FaultPtr->IsValid())
					{
						const TSharedPtr<FJsonObject>& Fault = *FaultPtr;
						Fault->TryGetStringField(TEXT("code"), Result.FaultCode);
						Fault->TryGetStringField(TEXT("blame"), Result.Blame);
						Fault->TryGetBoolField(TEXT("retryable"), Result.bRetryable);
					}

					const TArray<TSharedPtr<FJsonValue>>* Mutations = nullptr;
					if (Parsed->TryGetArrayField(TEXT("mutationsApplied"), Mutations) && Mutations)
					{
						Result.Mutations.Reserve(Mutations->Num());
						for (const TSharedPtr<FJsonValue>& Entry : *Mutations)
						{
							const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
							if (!Obj.IsValid())
							{
								continue;
							}
							FCrowdyCppMutationApplied M;
							Obj->TryGetStringField(TEXT("containerId"), M.ContainerId);
							Obj->TryGetStringField(TEXT("key"), M.Key);
							Obj->TryGetStringField(TEXT("oldValueJson"), M.OldValueJson);
							Obj->TryGetStringField(TEXT("newValueJson"), M.NewValueJson);
							Result.Mutations.Add(MoveTemp(M));
						}
					}

					FireOnce(MoveTemp(Result));
				}
				catch (const std::exception& Ex)
				{
					FCrowdyCppInvokeResult Failed;
					Failed.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
					FireOnce(MoveTemp(Failed));
				}
				catch (...)
				{
					FCrowdyCppInvokeResult Failed;
					Failed.ErrorMessage = TEXT("gameModelInvoke callback threw a non-standard exception");
					FireOnce(MoveTemp(Failed));
				}
			});
	}
	catch (const std::exception& Ex)
	{
		FCrowdyCppInvokeResult Result;
		Result.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
		FireOnce(MoveTemp(Result));
	}
	catch (...)
	{
		FCrowdyCppInvokeResult Result;
		Result.ErrorMessage = TEXT("gameModelInvoke request threw a non-standard exception");
		FireOnce(MoveTemp(Result));
	}
	return Issued.Handle;
}

FCrowdyCppRequestHandle FCrowdyCppClient::ListContainers(int64 AppId, const FString& TypeName,
	const FString& SessionId, TFunction<void(bool, TArray<TSharedPtr<FJsonObject>>)> OnDone)
{
	// The caller's two-value completion is adapted to the one-result shape the shared delivery plumbing uses, so
	// this call is cancelable on the same terms as every other; a canceled read reports failure and an empty list.
	const TSharedRef<TFunction<void(bool, TArray<TSharedPtr<FJsonObject>>)>> Caller =
		MakeShared<TFunction<void(bool, TArray<TSharedPtr<FJsonObject>>)>>(MoveTemp(OnDone));
	TFunction<void(FContainerListResult)> Adapted = [Caller](FContainerListResult Result)
	{
		(*Caller)(Result.bOk, MoveTemp(Result.Containers));
	};

	const TIssuedRequest<FContainerListResult> Issued = BeginRequest<FContainerListResult>(
		Impl ? Impl->Requests : nullptr, TEXT("GameModelContainers"), MoveTemp(Adapted), FContainerListResult());
	const TFunction<void(FContainerListResult)>& FireResult = Issued.Fire;
	auto FireOnce = [FireResult](bool bOk, TArray<TSharedPtr<FJsonObject>> Containers)
	{
		FContainerListResult Result;
		Result.bOk = bOk;
		Result.Containers = MoveTemp(Containers);
		FireResult(MoveTemp(Result));
	};

	if (!Impl || !Impl->IsUsable())
	{
		FireOnce(false, TArray<TSharedPtr<FJsonObject>>());
		return Issued.Handle;
	}

	// typeName and sessionId are omitted server-side when empty (the twin drops empty vars).
	const std::string AppIdStr = std::string(TCHAR_TO_UTF8(*LexToString(AppId)));
	const std::string TypeNameStr = std::string(TCHAR_TO_UTF8(*TypeName));
	const std::string SessionIdStr = std::string(TCHAR_TO_UTF8(*SessionId));

	const TSharedRef<FBusyRetry> Retry = MakeBusyRetry(Issued, ECrowdyCppTokenPlane::Game, EBusyRetryKind::Query);
	Retry->Send = [AppIdStr, TypeNameStr, SessionIdStr](crowdy::CrowdyClient& Client,
		crowdy::graphql::GraphQLCallback Cb)
	{
		Client.gameModel().containersAsync(AppIdStr, TypeNameStr, SessionIdStr, std::move(Cb));
	};
	Retry->Deliver = [FireOnce, Reason = Issued.FailureReason](crowdy::graphql::GraphQLOutcome Out)
	{
		*Reason = OutcomeFailureReason(Out);
		// The callback runs from Poll()/drain(), outside any issue-time try, so contain any throw here and deliver a
		// clean failure rather than let it escape into the engine ticker.
		try
		{
			TArray<TSharedPtr<FJsonObject>> Containers;
			if (!Out.ok() || !Out.data.isArray())
			{
				// Transport/GraphQL failure or an unexpected non-array payload: no containers, bOk false,
				// since a missing or non-array field is always a failed read.
				FireOnce(false, MoveTemp(Containers));
				return;
			}

			// out.data is the unwrapped [GmContainer] array. Re-serialize it and decode with the UE JSON
			// reader so each element is a plain FJsonObject the subsystem can consume directly.
			// yyjson parsed the response with an iterative parse, but this
			// UE re-parse is recursive, so the dumped string is nesting-guarded first: a forged deep
			// element would otherwise build a DOM that overflows on teardown. Over-limit rejects the
			// whole list.
			TArray<TSharedPtr<FJsonValue>> Parsed;
			bool bDecoded = false;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_GM_DecodeResponse);
				const FString ArrayJson = Utf8ToFString(Out.data.dump());
				if (CrowdyJsonSafety::IsNestingWithinLimit(ArrayJson))
				{
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArrayJson);
					bDecoded = FJsonSerializer::Deserialize(Reader, Parsed);
				}
			}
			if (!bDecoded)
			{
				FireOnce(false, MoveTemp(Containers));
				return;
			}
			Containers.Reserve(Parsed.Num());
			for (const TSharedPtr<FJsonValue>& V : Parsed)
			{
				const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
				if (Obj.IsValid())
				{
					Containers.Add(Obj);
				}
			}
			FireOnce(true, MoveTemp(Containers));
		}
		catch (...)
		{
			FireOnce(false, TArray<TSharedPtr<FJsonObject>>());
		}
	};
	Impl->SendRetryable(Retry);
	return Issued.Handle;
}

FCrowdyCppRequestHandle FCrowdyCppClient::RunOp(ECrowdyCppApiDomain Domain, const FString& OperationName,
	const TSharedPtr<FJsonObject>& Variables, TFunction<void(FCrowdyCppJsonResult)> OnDone,
	TOptional<ECrowdyCppTokenPlane> Plane)
{
	// Deliver exactly once across every path: the async completion, the catch below, each pre-flight rejection, and
	// a cancellation. The async transport delivers its callback from Poll() on the game thread and every other path
	// runs synchronously on the same thread, so the fired flag needs no lock and the OnDone can safely touch
	// game-thread state.
	FCrowdyCppJsonResult Canceled;
	Canceled.ErrorMessage = CanceledErrorMessage();
	const TIssuedRequest<FCrowdyCppJsonResult> Issued = BeginRequest<FCrowdyCppJsonResult>(
		Impl ? Impl->Requests : nullptr, *OperationName, MoveTemp(OnDone), MoveTemp(Canceled));
	const TFunction<void(FCrowdyCppJsonResult)>& FireOnce = Issued.Fire;

	if (!Impl)
	{
		FCrowdyCppJsonResult Result;
		Result.ErrorMessage = TEXT("CrowdyCPP client is not available");
		FireOnce(MoveTemp(Result));
		return Issued.Handle;
	}

	// Variables is already the full GraphQL variables object; appId rides as a JSON string, nullable inputs are
	// already omitted, and an explicit JSON null (a turn clear) stays null, all preserved by the conversion.
	const crowdy::graphql::JVal JVars = ObjectToJVal(Variables);
	const std::string OperationNameStr = std::string(TCHAR_TO_UTF8(*OperationName));

	// Send only this operation's own document. A bundled document carrying every operation is validated in full by
	// the server before one is selected, so a single field the deployed server does not implement would fail every
	// call at once rather than just the one that needs it. An operation the domain does not define yields an empty
	// document, which fails closed here rather than as a confusing server-side syntax error.
	const std::string_view Document = ResolveDocument(Domain, OperationNameStr);
	if (Document.empty())
	{
		FCrowdyCppJsonResult Result;
		Result.ErrorMessage = FString::Printf(
			TEXT("no document for operation '%s' in the requested API domain"), *OperationName);
		FireOnce(MoveTemp(Result));
		return Issued.Handle;
	}

	// One origin serves every operation, so all that is left to decide is the bearer. The caller's plane wins if it
	// named one, otherwise the operation's own exception, otherwise the domain's plane. A domain whose operations
	// never agreed on one plane has no answer to fall back on, and that is refused rather than guessed, because a
	// request under the wrong bearer is syntactically fine and semantically about the wrong subject.
	TOptional<ECrowdyCppTokenPlane> Chosen = Plane;
	if (!Chosen.IsSet())
	{
		Chosen = OperationPlaneException(Domain, OperationNameStr);
	}
	if (!Chosen.IsSet())
	{
		Chosen = DomainDefaultPlane(Domain);
	}
	if (!Chosen.IsSet())
	{
		FCrowdyCppJsonResult Result;
		Result.ErrorMessage = FString::Printf(
			TEXT("operation '%s' has no default token plane; the caller must name the plane to issue it under"),
			*OperationName);
		FireOnce(MoveTemp(Result));
		return Issued.Handle;
	}

	const TSharedRef<FBusyRetry> Retry =
		MakeBusyRetry(Issued, Chosen.GetValue(), RetryKindFor(Domain, Document, OperationNameStr));
	Retry->Send = [Document, JVars, OperationNameStr](crowdy::CrowdyClient& Client, crowdy::graphql::GraphQLCallback Cb)
	{
		Client.graphqlClient().requestAsync(Document, JVars, OperationNameStr, std::move(Cb));
	};
	Retry->Deliver = [FireOnce, Reason = Issued.FailureReason](crowdy::graphql::GraphQLOutcome Out)
	{
		*Reason = OutcomeFailureReason(Out);
		// The callback runs from Poll()/drain(), outside any issue-time try, so contain any throw here and deliver a
		// clean failure rather than let it escape into the engine ticker.
		try
		{
			FCrowdyCppJsonResult Result;
			if (!Out.ok())
			{
				// Transport or GraphQL failure: no data object. bTransportOk stays false for a
				// non-2xx call or any errors[] in the response.
				//
				// A WRONG_DATACENTER almost never reaches here: the client follows it and retries once,
				// including when a concurrent request's redirect already moved the client, so what a
				// caller sees is the answer from the datacenter it moved to. Only datacenters that keep
				// disagreeing about an app surface it. APP_UNAVAILABLE does reach here, and is the one
				// code worth branching on, because there is nowhere to move to.
				Result.ErrorMessage = OutcomeErrorMessage(Out);
				ReadOutcomeFault(Out, Result);
				FireOnce(MoveTemp(Result));
				return;
			}

			// Out.data is the GraphQL response's `data` object (requestAsync does not unwrap the single root
			// field, so it stays { "<gameModelX>": ... } - the shape FCrowdyGameApiCodec's ParseXEnvelope
			// reads after GetDataObject). yyjson parsed the response iteratively, but this UE re-parse is
			// recursive, so the dumped object is nesting-guarded first: a forged deep value would otherwise
			// build a DOM that overflows on teardown. Over-limit fails closed.
			TSharedPtr<FJsonObject> Parsed;
			bool bDecoded = false;
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_GM_DecodeResponse);
				const FString DataJson = Utf8ToFString(Out.data.dump());
				if (CrowdyJsonSafety::IsNestingWithinLimit(DataJson))
				{
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DataJson);
					bDecoded = FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid();
				}
			}
			if (bDecoded)
			{
				Result.bTransportOk = true;
				Result.Data = Parsed;
				FireOnce(MoveTemp(Result));
				return;
			}

			Result.ErrorMessage = TEXT("malformed or over-deep response data");
			FireOnce(MoveTemp(Result));
		}
		catch (const std::exception& Ex)
		{
			FCrowdyCppJsonResult Failed;
			Failed.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
			FireOnce(MoveTemp(Failed));
		}
		catch (...)
		{
			FCrowdyCppJsonResult Failed;
			Failed.ErrorMessage = TEXT("Game Model runtime op callback threw a non-standard exception");
			FireOnce(MoveTemp(Failed));
		}
	};
	Impl->SendRetryable(Retry);
	return Issued.Handle;
}

FCrowdyCppRequestHandle FCrowdyCppClient::SeedSchema(const FString& InputJson,
	TFunction<void(FCrowdyCppStudioOpResult)> OnDone)
{
	return RunGameModelInputOp(Impl ? Impl->Use(ECrowdyCppTokenPlane::Game) : nullptr,
		Impl ? Impl->Requests : nullptr, EGameModelInputOp::Seed, InputJson, MoveTemp(OnDone));
}

FCrowdyCppRequestHandle FCrowdyCppClient::UpsertAutomation(const FString& InputJson,
	TFunction<void(FCrowdyCppStudioOpResult)> OnDone)
{
	return RunGameModelInputOp(Impl ? Impl->Use(ECrowdyCppTokenPlane::Game) : nullptr,
		Impl ? Impl->Requests : nullptr, EGameModelInputOp::UpsertAutomation, InputJson, MoveTemp(OnDone));
}

FCrowdyCppRequestHandle FCrowdyCppClient::UpsertAutomationTrigger(const FString& InputJson,
	TFunction<void(FCrowdyCppStudioOpResult)> OnDone)
{
	return RunGameModelInputOp(Impl ? Impl->Use(ECrowdyCppTokenPlane::Game) : nullptr,
		Impl ? Impl->Requests : nullptr, EGameModelInputOp::UpsertAutomationTrigger, InputJson, MoveTemp(OnDone));
}

FCrowdyCppRequestHandle FCrowdyCppClient::SignInWithPassword(const FString& Email, const FString& Password,
	TFunction<void(FCrowdyCppAuthResult)> OnDone)
{
	const std::string EmailStr = std::string(TCHAR_TO_UTF8(*Email));
	const std::string PasswordStr = std::string(TCHAR_TO_UTF8(*Password));
	return RunAuthOp<FCrowdyCppAuthResult, crowdy::domains::AuthResponse>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("login"), MoveTemp(OnDone),
		[EmailStr, PasswordStr](crowdy::CrowdyClient& Client, FAuthPayloadCallback Cb)
		{
			Client.auth().loginAsync(EmailStr, PasswordStr, std::move(Cb));
		},
		&MapAuthResponse);
}

FCrowdyCppRequestHandle FCrowdyCppClient::RegisterWithPassword(const FString& Email, const FString& Password,
	const FString& Gamertag, TFunction<void(FCrowdyCppAuthResult)> OnDone)
{
	const std::string EmailStr = std::string(TCHAR_TO_UTF8(*Email));
	const std::string PasswordStr = std::string(TCHAR_TO_UTF8(*Password));
	const std::string GamertagStr = std::string(TCHAR_TO_UTF8(*Gamertag));
	return RunAuthOp<FCrowdyCppAuthResult, crowdy::domains::AuthResponse>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("register"), MoveTemp(OnDone),
		[EmailStr, PasswordStr, GamertagStr](crowdy::CrowdyClient& Client, FAuthPayloadCallback Cb)
		{
			Client.auth().registerUserAsync(EmailStr, PasswordStr, GamertagStr, std::move(Cb));
		},
		&MapAuthResponse);
}

FCrowdyCppRequestHandle FCrowdyCppClient::RequestLoginLink(const FString& Email, const FString& RedirectUri,
	TFunction<void(FCrowdyCppJsonValueResult)> OnDone)
{
	const std::string EmailStr = std::string(TCHAR_TO_UTF8(*Email));
	const std::string RedirectUriStr = std::string(TCHAR_TO_UTF8(*RedirectUri));
	return RunAuthOp<FCrowdyCppJsonValueResult, crowdy::graphql::Json>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("requestLoginLink"), MoveTemp(OnDone),
		[EmailStr, RedirectUriStr](crowdy::CrowdyClient& Client, FJsonPayloadCallback Cb)
		{
			Client.auth().requestLoginLinkAsync(EmailStr, RedirectUriStr, AsJsonPayloadCallback(std::move(Cb)));
		},
		&MapUnwrappedJson);
}

FCrowdyCppRequestHandle FCrowdyCppClient::CompleteLoginLink(const FString& Token,
	TFunction<void(FCrowdyCppAuthResult)> OnDone)
{
	const std::string TokenStr = std::string(TCHAR_TO_UTF8(*Token));
	return RunAuthOp<FCrowdyCppAuthResult, crowdy::domains::AuthResponse>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("completeLoginLink"), MoveTemp(OnDone),
		[TokenStr](crowdy::CrowdyClient& Client, FAuthPayloadCallback Cb)
		{
			Client.auth().completeLoginLinkAsync(TokenStr, std::move(Cb));
		},
		&MapAuthResponse);
}

FCrowdyCppRequestHandle FCrowdyCppClient::SocialLoginStart(const FString& Provider, const FString& RedirectUri,
	TFunction<void(FCrowdyCppJsonValueResult)> OnDone)
{
	const std::string ProviderStr = std::string(TCHAR_TO_UTF8(*Provider));
	const std::string RedirectUriStr = std::string(TCHAR_TO_UTF8(*RedirectUri));
	return RunAuthOp<FCrowdyCppJsonValueResult, crowdy::graphql::Json>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("socialLoginStart"), MoveTemp(OnDone),
		[ProviderStr, RedirectUriStr](crowdy::CrowdyClient& Client, FJsonPayloadCallback Cb)
		{
			Client.auth().socialLoginStartAsync(ProviderStr, RedirectUriStr, AsJsonPayloadCallback(std::move(Cb)));
		},
		&MapUnwrappedJson);
}

FCrowdyCppRequestHandle FCrowdyCppClient::SocialLoginComplete(const FString& Provider, const FString& Code,
	const FString& State, TFunction<void(FCrowdyCppAuthResult)> OnDone)
{
	const std::string ProviderStr = std::string(TCHAR_TO_UTF8(*Provider));
	const std::string CodeStr = std::string(TCHAR_TO_UTF8(*Code));
	const std::string StateStr = std::string(TCHAR_TO_UTF8(*State));
	return RunAuthOp<FCrowdyCppAuthResult, crowdy::domains::AuthResponse>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("socialLoginComplete"), MoveTemp(OnDone),
		[ProviderStr, CodeStr, StateStr](crowdy::CrowdyClient& Client, FAuthPayloadCallback Cb)
		{
			Client.auth().socialLoginCompleteAsync(ProviderStr, CodeStr, StateStr, std::move(Cb));
		},
		&MapAuthResponse);
}

FCrowdyCppRequestHandle FCrowdyCppClient::ListLoginProviders(TFunction<void(FCrowdyCppStringListResult)> OnDone)
{
	return RunAuthOp<FCrowdyCppStringListResult, std::vector<std::string>>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("availableLoginProviders"), MoveTemp(OnDone),
		[](crowdy::CrowdyClient& Client,
			std::function<void(crowdy::graphql::GraphQLOutcome, std::vector<std::string>)> Cb)
		{
			Client.auth().availableLoginProvidersAsync(std::move(Cb));
		},
		&MapStringList);
}

FCrowdyCppRequestHandle FCrowdyCppClient::ListMyIdentities(TFunction<void(FCrowdyCppJsonValueResult)> OnDone)
{
	return RunAuthOp<FCrowdyCppJsonValueResult, crowdy::graphql::Json>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::Management) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("myIdentities"), MoveTemp(OnDone),
		[](crowdy::CrowdyClient& Client, FJsonPayloadCallback Cb)
		{
			Client.auth().myIdentitiesAsync(AsJsonPayloadCallback(std::move(Cb)));
		},
		&MapUnwrappedJson);
}

FCrowdyCppRequestHandle FCrowdyCppClient::LinkIdentity(const FString& Provider, const FString& Code,
	const FString& State, TFunction<void(FCrowdyCppJsonValueResult)> OnDone)
{
	const std::string ProviderStr = std::string(TCHAR_TO_UTF8(*Provider));
	const std::string CodeStr = std::string(TCHAR_TO_UTF8(*Code));
	const std::string StateStr = std::string(TCHAR_TO_UTF8(*State));
	return RunAuthOp<FCrowdyCppJsonValueResult, crowdy::graphql::Json>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::Management) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("linkIdentity"), MoveTemp(OnDone),
		[ProviderStr, CodeStr, StateStr](crowdy::CrowdyClient& Client, FJsonPayloadCallback Cb)
		{
			Client.auth().linkIdentityAsync(ProviderStr, CodeStr, StateStr, AsJsonPayloadCallback(std::move(Cb)));
		},
		&MapUnwrappedJson);
}

FCrowdyCppRequestHandle FCrowdyCppClient::UnlinkIdentity(const FString& IdentityId,
	TFunction<void(FCrowdyCppBoolResult)> OnDone)
{
	const std::string IdentityIdStr = std::string(TCHAR_TO_UTF8(*IdentityId));
	return RunAuthOp<FCrowdyCppBoolResult, bool>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::Management) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("unlinkIdentity"), MoveTemp(OnDone),
		[IdentityIdStr](crowdy::CrowdyClient& Client,
			std::function<void(crowdy::graphql::GraphQLOutcome, bool)> Cb)
		{
			Client.auth().unlinkIdentityAsync(IdentityIdStr, std::move(Cb));
		},
		&MapBool);
}

FCrowdyCppRequestHandle FCrowdyCppClient::MintAppToken(int64 AppId,
	TFunction<void(FCrowdyCppAppTokenResult)> OnDone)
{
	// appId is a BigInt scalar: the twin emits the string we pass here, never a number.
	const std::string AppIdStr = std::string(TCHAR_TO_UTF8(*LexToString(AppId)));
	return RunAuthOp<FCrowdyCppAppTokenResult, crowdy::domains::AppTokenResponse>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::Management) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("mintAppToken"), MoveTemp(OnDone),
		[AppIdStr](crowdy::CrowdyClient& Client, FAppTokenPayloadCallback Cb)
		{
			Client.portal().mintAppTokenAsync(AppIdStr, std::move(Cb));
		},
		&MapAppTokenResponse);
}

FString FCrowdyCppClient::GetApiEndpoint() const
{
	if (!Impl || !Impl->Client)
	{
		return FString();
	}
	return Utf8ToFString(Impl->Client->graphqlClient().endpoint());
}

void FCrowdyCppClient::SetRediscoveredEndpoint(const FString& ApiUrl, const FString& WsUrl)
{
	if (!Impl || !Impl->Rediscover.IsValid())
	{
		return;
	}
	FScopeLock Lock(&Impl->Rediscover->Guard);
	Impl->Rediscover->ApiUrl = ApiUrl;
	Impl->Rediscover->WsUrl = WsUrl;
}

bool FCrowdyCppClient::MoveToDatacenter(const FString& ApiUrl, const FString& WsUrl)
{
	if (!Impl || !Impl->IsUsable())
	{
		return false;
	}

	bool bMoved = false;
	try
	{
		bMoved = Impl->Client->moveToDatacenter(
			std::string(TCHAR_TO_UTF8(*ApiUrl)), std::string(TCHAR_TO_UTF8(*WsUrl)));
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("Moving to another datacenter threw: %hs"), Ex.what());
		return false;
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Warning, TEXT("Moving to another datacenter threw a non-standard exception"));
		return false;
	}

	// Carry the socket over now rather than waiting for the next Poll(). The caller asked for this move and can
	// reasonably issue against the new datacenter on the very next line; leaving the socket a tick behind would
	// make the two disagree for exactly as long as it takes to notice.
	if (bMoved)
	{
		Impl->FollowEndpointMove();
	}
	return bMoved;
}

FCrowdyCppRequestHandle FCrowdyCppClient::ResolveAppEndpoints(const TArray<FString>& AppIDs,
	TFunction<void(FCrowdyCppAppDiscoveryResult)> OnDone)
{
	FCrowdyCppAppDiscoveryResult Canceled;
	Canceled.ErrorMessage = CanceledErrorMessage();
	const TIssuedRequest<FCrowdyCppAppDiscoveryResult> Issued = BeginRequest<FCrowdyCppAppDiscoveryResult>(
		Impl ? Impl->Requests : nullptr, TEXT("AppDiscovery"), MoveTemp(OnDone), MoveTemp(Canceled));
	const TFunction<void(FCrowdyCppAppDiscoveryResult)>& FireOnce = Issued.Fire;

	// No bearer at all, and that is the point of the call: a client knows its app id long before it holds any
	// credential, so this is what it can ask first. Clearing also stops a previous caller's token from riding it.
	crowdy::CrowdyClient* Client = Impl ? Impl->UseBearer(FImpl::EBearerChoice::None) : nullptr;
	if (!Client)
	{
		FCrowdyCppAppDiscoveryResult Result;
		Result.ErrorMessage = TEXT("CrowdyCPP client is not available");
		FireOnce(MoveTemp(Result));
		return Issued.Handle;
	}

	std::vector<std::string> Ids;
	Ids.reserve(AppIDs.Num());
	for (const FString& Id : AppIDs)
	{
		if (!Id.IsEmpty())
		{
			Ids.push_back(std::string(TCHAR_TO_UTF8(*Id)));
		}
	}
	if (Ids.empty())
	{
		// Answered here rather than sent: the server would reject an empty list, and only after a round trip.
		FCrowdyCppAppDiscoveryResult Result;
		Result.ErrorMessage = TEXT("no app ids to resolve");
		FireOnce(MoveTemp(Result));
		return Issued.Handle;
	}

	const CrowdyCppTransport::FHandOffScope HandOff(*Issued.HandOffSeconds);
	try
	{
		Client->discovery().appsAsync(Ids,
			[FireOnce, Reason = Issued.FailureReason](crowdy::graphql::GraphQLOutcome Out, std::vector<crowdy::domains::AppEndpoint> Endpoints)
			{
				*Reason = OutcomeFailureReason(Out);
				// Runs from Poll(), outside the try below, so contain any throw here too.
				try
				{
					FCrowdyCppAppDiscoveryResult Result;
					if (!Out.ok())
					{
						Result.ErrorMessage = OutcomeErrorMessage(Out);
						FireOnce(MoveTemp(Result));
						return;
					}
					Result.Endpoints.Reserve(static_cast<int32>(Endpoints.size()));
					for (const crowdy::domains::AppEndpoint& Endpoint : Endpoints)
					{
						FCrowdyCppAppEndpoint Mapped;
						Mapped.AppID = Utf8ToFString(Endpoint.appId);
						Mapped.DatacenterCode = Utf8ToFString(Endpoint.datacenterCode);
						Mapped.GameApiUrl = Utf8ToFString(Endpoint.gameApiUrl);
						Mapped.GameApiWsUrl = Utf8ToFString(Endpoint.gameApiWsUrl);
						Result.Endpoints.Add(MoveTemp(Mapped));
					}
					// An app with no placement is still a successful answer, so bOk is set from the query rather
					// than from whether anything came back placed.
					Result.bOk = true;
					FireOnce(MoveTemp(Result));
				}
				catch (const std::exception& Ex)
				{
					FCrowdyCppAppDiscoveryResult Failed;
					Failed.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
					FireOnce(MoveTemp(Failed));
				}
				catch (...)
				{
					FCrowdyCppAppDiscoveryResult Failed;
					Failed.ErrorMessage = TEXT("appDiscovery callback threw a non-standard exception");
					FireOnce(MoveTemp(Failed));
				}
			});
	}
	catch (const std::exception& Ex)
	{
		FCrowdyCppAppDiscoveryResult Result;
		Result.ErrorMessage = FString(UTF8_TO_TCHAR(Ex.what()));
		FireOnce(MoveTemp(Result));
	}
	catch (...)
	{
		FCrowdyCppAppDiscoveryResult Result;
		Result.ErrorMessage = TEXT("appDiscovery threw a non-standard exception");
		FireOnce(MoveTemp(Result));
	}
	return Issued.Handle;
}

FCrowdyCppSubscriptionHandle FCrowdyCppClient::WatchRealtimeControl(
	TFunction<void(FCrowdyCppRealtimeControlEvent)> OnEvent)
{
	// Opened on the bridge's own socket rather than through CrowdyClient::watchRealtimeControl, which uses the
	// socket inside CrowdyClient. That one observes the shared auth state and so reconnects on every bearer switch,
	// which for a stream whose whole value is being connected when the warning arrives is the wrong socket to be on.
	//
	// Handling draining here rather than in the caller keeps the two halves of the signal together: the advance
	// warning is only worth having if something acts on it, and every owner would otherwise write the same handler.
	FCrowdyCppSubscriptionCallbacks Callbacks;

	// `this` by raw pointer, which is safe here and only here because of who owns the callback: it is held in
	// FImpl::LiveSubscriptions, so it cannot outlive the FImpl, which cannot outlive this client. Close() suppresses
	// every live subscription before the pump is fenced, so nothing runs during teardown either.
	Callbacks.OnNext = [this, OnEvent](TSharedPtr<FJsonObject> Data)
	{
		if (!Data.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!Data->TryGetObjectField(TEXT("udpNotifications"), Payload) || !Payload || !Payload->IsValid())
		{
			return;
		}

		// The union carries the browser UDP proxy's gameplay fan-out as well, which a native client speaking UDP
		// directly has no business acting on. Anything that is not a control frame is dropped rather than parsed
		// into an event with empty fields, which would read as a malformed control frame instead of as not one.
		FString TypeName;
		if (!(*Payload)->TryGetStringField(TEXT("__typename"), TypeName)
			|| TypeName != TEXT("RealtimeConnectionEvent"))
		{
			return;
		}

		FCrowdyCppRealtimeControlEvent Event;
		(*Payload)->TryGetStringField(TEXT("status"), Event.Status);
		(*Payload)->TryGetStringField(TEXT("code"), Event.Code);
		(*Payload)->TryGetStringField(TEXT("message"), Event.Message);
		// Absent means retryable: the server's contract is that only an explicit false forecloses a retry.
		if (!(*Payload)->TryGetBoolField(TEXT("retryable"), Event.bRetryable))
		{
			Event.bRetryable = true;
		}

		if (Event.IsDraining())
		{
			// Advisory and mid-stream: the instance still answers, so move now rather than waiting for it to stop.
			// This is the case re-discovery exists to get ahead of, so it is not counted toward the repeated-failure
			// threshold; nothing has failed yet, and that is the whole value of the signal.
			UE_LOG(LogCrowdyCpp, Warning,
				TEXT("The API instance is draining; re-discovering an endpoint before it stops serving."));
			if (Impl && Impl->IsUsable())
			{
				try
				{
					(void)Impl->Client->rediscoverEndpoint(Impl->Client->activeAppId());
				}
				catch (...)
				{
				}
				Impl->FollowEndpointMove();
			}
		}

		if (OnEvent)
		{
			OnEvent(MoveTemp(Event));
		}
	};

	return SubscribeOperation(ECrowdyCppApiDomain::Realtime, TEXT("RealtimeControlEvents"), nullptr,
		MoveTemp(Callbacks));
}

FCrowdyCppRequestHandle FCrowdyCppClient::RefreshAppToken(TFunction<void(FCrowdyCppAppTokenResult)> OnDone)
{
	// The game bearer: the app-scoped token being rotated is what authorizes its own rotation, so the session token
	// would be refused here.
	return RunAuthOp<FCrowdyCppAppTokenResult, crowdy::domains::AppTokenResponse>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::Game) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("refreshAppToken"), MoveTemp(OnDone),
		[](crowdy::CrowdyClient& Client, FAppTokenPayloadCallback Cb)
		{
			// install=false: the twin would otherwise write the replacement into the client's shared bearer, which
			// the next per-call install overwrites anyway. Which plane the new token belongs to is the owner's call.
			Client.portal().refreshAsync(std::move(Cb), false);
		},
		&MapAppTokenResponse);
}

FCrowdyCppRequestHandle FCrowdyCppClient::RefreshAppToken(const FString& CurrentServerIp4,
	const int32 CurrentServerClientPort, TFunction<void(FCrowdyCppAppTokenResult)> OnDone)
{
	// An unnamed server is the no-server rotation, not a rotation naming nothing: the twin below would send an empty
	// address the API has no way to match, and the reply would carry no authorizedServer for a reason the caller
	// could not tell apart from an API too old to answer one.
	if (CurrentServerIp4.IsEmpty() || CurrentServerClientPort <= 0)
	{
		return RefreshAppToken(MoveTemp(OnDone));
	}

	const std::string Ip4 = std::string(TCHAR_TO_UTF8(*CurrentServerIp4));

	return RunAuthOp<FCrowdyCppAppTokenResult, crowdy::domains::AppTokenResponse>(
		Impl ? Impl->UseBearer(FImpl::EBearerChoice::Game) : nullptr, Impl ? Impl->Requests : nullptr,
		TEXT("refreshAppToken"), MoveTemp(OnDone),
		[Ip4, CurrentServerClientPort](crowdy::CrowdyClient& Client, FAppTokenPayloadCallback Cb)
		{
			// install=false for the same reason as the no-server form above.
			Client.portal().refreshAsync(Ip4, CurrentServerClientPort, std::move(Cb), false);
		},
		&MapAppTokenResponse);
}

FCrowdyCppSubscriptionHandle FCrowdyCppClient::Subscribe(const FString& Document,
	const TSharedPtr<FJsonObject>& Variables, const FString& OperationName,
	FCrowdyCppSubscriptionCallbacks Callbacks)
{
	FCrowdyCppSubscriptionHandle Handle;

	if (!Impl || !Impl->IsUsable() || !Impl->SubscriptionClient)
	{
		// Reported rather than dropped: a caller waiting on a stream that will never open cannot tell that apart
		// from a server with nothing to say.
		if (Callbacks.OnError)
		{
			Callbacks.OnError(TEXT("GraphQL subscriptions are unavailable on this client"), true);
		}
		return Handle;
	}

	TUniquePtr<FImpl::FLiveSubscription> Live = MakeUnique<FImpl::FLiveSubscription>();
	*Live->Callbacks = MoveTemp(Callbacks);

	// Captured by the CrowdyCPP callbacks so they reach the caller's without owning the map entry, which they
	// outlive in the window between the server ending the subscription and the next Poll() reaping it.
	const TSharedRef<FCrowdyCppSubscriptionCallbacks> Shared = Live->Callbacks;
	const TSharedRef<bool> Finished = Live->bFinished;

	crowdy::graphql::GraphQLSubscriptionCallbacks Native;

	Native.onNext = [Shared, Finished](crowdy::graphql::GraphQLSubscriptionOutcome Outcome)
	{
		if (*Finished)
		{
			return;
		}
		if (!Outcome.ok())
		{
			// GraphQL may return errors on one push without ending the stream, so this is not terminal: the
			// subscription stays open and the next push may well succeed.
			if (Shared->OnError)
			{
				const FString Message = Outcome.errors.empty()
					? FString(TEXT("the server rejected a subscription payload"))
					: Utf8ToFString(Outcome.errors[0].message);
				Shared->OnError(Message, false);
			}
			return;
		}
		// Nesting-guarded on the way in, like every other server payload: this arrives straight off the wire and
		// the UE re-parse is recursive.
		const TSharedPtr<FJsonValue> Value = UnwrappedJsonToUeValue(Outcome.data);
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			// Refusing to decode is not the same as the server having nothing to say, and handing the caller an
			// empty payload would make those two indistinguishable. The stream itself is unharmed, so this is a
			// recoverable failure rather than the end of it.
			if (Shared->OnError)
			{
				Shared->OnError(TEXT("a subscription payload could not be decoded"), false);
			}
			return;
		}
		if (Shared->OnNext)
		{
			Shared->OnNext(Value->AsObject());
		}
	};

	Native.onError = [Shared, Finished](crowdy::graphql::GraphQLSubscriptionError Error)
	{
		if (*Finished)
		{
			return;
		}
		if (Error.terminal)
		{
			*Finished = true;
		}
		if (Shared->OnError)
		{
			FString Message = Utf8ToFString(Error.message);
			if (Message.IsEmpty())
			{
				// A protocol failure may carry only its stable code, which is still more use than an empty string.
				Message = Error.code.empty()
					? FString(UTF8_TO_TCHAR(crowdy::errcName(Error.status.code)))
					: Utf8ToFString(Error.code);
			}
			Shared->OnError(Message, Error.terminal);
		}
	};

	Native.onComplete = [Shared, Finished]()
	{
		if (*Finished)
		{
			return;
		}
		*Finished = true;
		if (Shared->OnComplete)
		{
			Shared->OnComplete();
		}
	};

	crowdy::graphql::GraphQLSubscriptionRequest Request;
	Request.document = std::string(TCHAR_TO_UTF8(*Document));
	// Null rather than an empty object when there are none, so the field is omitted from the payload entirely.
	Request.variables = Variables.IsValid() ? ObjectToJVal(Variables) : crowdy::graphql::JVal(nullptr);
	Request.operationName = std::string(TCHAR_TO_UTF8(*OperationName));

	try
	{
		Live->Handle = Impl->SubscriptionClient->subscribe(std::move(Request), std::move(Native));
	}
	catch (const std::exception& Ex)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("GraphQL subscribe threw: %hs"), Ex.what());
		if (!*Finished)
		{
			*Finished = true;
			if (Shared->OnError)
			{
				Shared->OnError(TEXT("the subscription could not be opened"), true);
			}
		}
		return Handle;
	}
	catch (...)
	{
		UE_LOG(LogCrowdyCpp, Error, TEXT("GraphQL subscribe threw a non-standard exception"));
		if (!*Finished)
		{
			*Finished = true;
			if (Shared->OnError)
			{
				Shared->OnError(TEXT("the subscription could not be opened"), true);
			}
		}
		return Handle;
	}

	// A subscription the underlying client refused (a closed client, an unusable endpoint, an empty document) comes
	// back already inactive, having queued its own error. Registering it would over-report it as live and let
	// Unsubscribe claim to have ended something that never started, so it is reported here and now instead. The
	// handle is then dropped, which suppresses the queued duplicate, leaving exactly one failure for the caller.
	if (!Live->Handle.active())
	{
		if (!*Finished)
		{
			*Finished = true;
			if (Shared->OnError)
			{
				Shared->OnError(TEXT("the subscription could not be opened"), true);
			}
		}
		return Handle;
	}

	Handle.Id = NextSubscriptionHandleId();
	Impl->LiveSubscriptions.Add(Handle.Id, MoveTemp(Live));
	return Handle;
}

FCrowdyCppSubscriptionHandle FCrowdyCppClient::SubscribeOperation(ECrowdyCppApiDomain Domain,
	const FString& OperationName, const TSharedPtr<FJsonObject>& Variables,
	FCrowdyCppSubscriptionCallbacks Callbacks)
{
	const std::string_view Document = ResolveDocument(Domain, std::string(TCHAR_TO_UTF8(*OperationName)));

	FString Refusal;
	if (Document.empty())
	{
		// Refused without a socket rather than sent as an empty document, for the same reason RunOp refuses an
		// unknown name: the server would reject it, and only after a round trip that told the caller nothing.
		Refusal = FString::Printf(TEXT("'%s' is not an operation this domain defines"), *OperationName);
	}
	else if (!Document.starts_with("subscription"))
	{
		// A query or mutation resolves to a perfectly good document that is simply not subscribable. Sending it as
		// a subscribe would open a socket and then fail against the server for a reason the caller cannot read.
		Refusal = FString::Printf(TEXT("'%s' is not a subscription"), *OperationName);
	}

	if (!Refusal.IsEmpty())
	{
		if (Callbacks.OnError)
		{
			Callbacks.OnError(Refusal, true);
		}
		return FCrowdyCppSubscriptionHandle();
	}

	// Unlike RunOp there is no plane to choose. The subscription client holds the game bearer and nothing else,
	// deliberately, so every operation on this path is unambiguous rather than a guess.
	return Subscribe(Utf8ToFString(std::string(Document)), Variables, OperationName, MoveTemp(Callbacks));
}

bool FCrowdyCppClient::Unsubscribe(FCrowdyCppSubscriptionHandle Handle)
{
	if (!Impl || !Handle.IsValid())
	{
		return false;
	}

	TUniquePtr<FImpl::FLiveSubscription>* Found = Impl->LiveSubscriptions.Find(Handle.Id);
	if (!Found || !Found->IsValid())
	{
		return false;
	}

	// Taken out of the map first, so cancelling cannot see a half-removed entry if it re-enters, and held locally so
	// the handle outlives its own cancel.
	TUniquePtr<FImpl::FLiveSubscription> Ending = MoveTemp(*Found);
	Impl->LiveSubscriptions.Remove(Handle.Id);

	// Suppressed before cancelling: the caller has said it is done, and cancel drains what the server already sent.
	*Ending->bFinished = true;
	FImpl::CancelContained(Ending->Handle);
	return true;
}

int32 FCrowdyCppClient::UnsubscribeAll()
{
	if (!Impl)
	{
		return 0;
	}

	TMap<uint64, TUniquePtr<FImpl::FLiveSubscription>> Ending = MoveTemp(Impl->LiveSubscriptions);
	Impl->LiveSubscriptions.Reset();

	int32 Ended = 0;
	for (TPair<uint64, TUniquePtr<FImpl::FLiveSubscription>>& Entry : Ending)
	{
		if (!Entry.Value.IsValid() || *Entry.Value->bFinished)
		{
			continue;
		}
		++Ended;
		*Entry.Value->bFinished = true;
		FImpl::CancelContained(Entry.Value->Handle);
	}
	return Ended;
}

int32 FCrowdyCppClient::NumActiveSubscriptions() const
{
	if (!Impl)
	{
		return 0;
	}

	int32 Active = 0;
	for (const TPair<uint64, TUniquePtr<FImpl::FLiveSubscription>>& Entry : Impl->LiveSubscriptions)
	{
		// Counted by the flag rather than by map size, so one the server ended reads as gone before the next Poll()
		// gets round to reaping it.
		if (Entry.Value.IsValid() && !*Entry.Value->bFinished)
		{
			++Active;
		}
	}
	return Active;
}

#if WITH_DEV_AUTOMATION_TESTS
TArray<FString> FCrowdyCppClient::TakeTestWebSocketSentFrames()
{
	TArray<FString> Frames;
	if (!Impl || !Impl->TestWebSocket)
	{
		return Frames;
	}
	const std::vector<std::string> Sent = Impl->TestWebSocket->TakeSentFrames();
	Frames.Reserve(static_cast<int32>(Sent.size()));
	for (const std::string& Frame : Sent)
	{
		Frames.Add(Utf8ToFString(Frame));
	}
	return Frames;
}

void FCrowdyCppClient::TestWebSocketOpen()
{
	if (Impl && Impl->TestWebSocket)
	{
		Impl->TestWebSocket->Open();
	}
}

void FCrowdyCppClient::TestWebSocketReceiveText(const FString& Text)
{
	if (Impl && Impl->TestWebSocket)
	{
		Impl->TestWebSocket->ReceiveText(std::string(TCHAR_TO_UTF8(*Text)));
	}
}

void FCrowdyCppClient::TestWebSocketCloseFromServer(int32 Code, bool bClean)
{
	if (Impl && Impl->TestWebSocket)
	{
		Impl->TestWebSocket->CloseFromServer(static_cast<std::uint16_t>(FMath::Clamp(Code, 0, 65535)),
			std::string(), bClean);
	}
}

bool FCrowdyCppClient::WasTestWebSocketClosedByClient() const
{
	return Impl && Impl->TestWebSocket && Impl->TestWebSocket->WasClosedByClient();
}

int32 FCrowdyCppClient::NumTestWebSocketConnections() const
{
	return Impl && Impl->TestWebSocket ? static_cast<int32>(Impl->TestWebSocket->ConnectionsCreated()) : 0;
}
#endif
