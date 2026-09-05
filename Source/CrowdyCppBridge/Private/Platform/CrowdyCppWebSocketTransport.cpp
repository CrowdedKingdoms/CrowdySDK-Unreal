#include "Platform/CrowdyCppWebSocketTransport.h"

#include "CrowdyCppBridge.h"
#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "Math/UnrealMathUtility.h"
#include "Templates/SharedPointer.h"

#if WITH_WEBSOCKETS
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#endif

#include <atomic>
#include <utility>

namespace
{
	using crowdy::Errc;
	using crowdy::Status;
	using crowdy::graphql::IWebSocketConnection;
	using crowdy::graphql::IWebSocketTransport;
	using crowdy::graphql::WebSocketConnectRequest;
	using crowdy::graphql::WebSocketEvent;
	using crowdy::graphql::WebSocketEventCallback;
	using crowdy::graphql::WebSocketEventKind;
	using crowdy::graphql::WebSocketErrorKind;
	using crowdy::graphql::WebSocketFrame;
	using crowdy::graphql::WebSocketFrameKind;

	// Length-bounded conversions so an embedded NUL never truncates a payload in either direction.
	std::string FStringToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value, Value.Len());
		return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
	}

	FString Utf8ToFString(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(Value.data(), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	// Run Work on the game thread: inline when already there, queued otherwise.
	//
	// Every engine call this transport makes goes through here, because the subscription client reconnects and times
	// out from its own timer thread while FWebSocketsModule is a game-thread stack.
	//
	// This does NOT establish an order between a call made on the game thread and one queued from another thread:
	// the queued one can run either side of the inline one. What keeps that safe is the closed flag, which every
	// deferred step re-checks before touching anything, not the order the two arrive in.
	void RunOnGameThread(TUniqueFunction<void()> Work)
	{
		if (IsInGameThread())
		{
			Work();
			return;
		}
		AsyncTask(ENamedThreads::GameThread, MoveTemp(Work));
	}

	// The engine-side half of one connection, kept separate from the CrowdyCPP-side object because the two have
	// different lifetimes: the connection can be dropped on the subscription client's timer thread, while the socket
	// it owns may only be touched and destroyed on the game thread.
	struct FUnrealWebSocketState : public TSharedFromThis<FUnrealWebSocketState, ESPMode::ThreadSafe>
	{
		FString Url;
		FString Protocol;
		uint64 MaxFrameBytes = 0;
		long ConnectTimeoutMs = 0;

		// Game thread only, from start() onwards. IWebSocket is only declared where the engine has WebSocket
		// support, so the socket and every use of it are guarded rather than just the code that creates it.
		WebSocketEventCallback Callback;
#if WITH_WEBSOCKETS
		TSharedPtr<IWebSocket> Socket;
#endif
		bool bStarted = false;

		// Read from any thread, so a close issued off the game thread short-circuits a start still sitting in the
		// queue. Relaxed ordering is enough because this publishes no data: everything it guards is written only on
		// the game thread, and the fields set before the connection is handed over are read-only from then on.
		std::atomic<bool> bClosed{false};

		// Fires once if the socket neither opens nor fails within the connect timeout. The subscription client arms
		// deadlines for acknowledgement and backoff but not for connecting, so this is the only thing standing
		// between a socket that stalls mid-handshake and a subscription that waits on it forever with no callback.
		FTSTicker::FDelegateHandle ConnectTimeoutHandle;
		bool bOpened = false;

		bool IsClosed() const { return bClosed.load(std::memory_order_relaxed); }

		void Deliver(WebSocketEvent Event)
		{
			// A closed connection is not one the subscription client still wants events from: whenever it closes, it
			// has already decided the outcome, and a late event would only be matched against a stale generation.
			if (IsClosed() || !Callback)
			{
				return;
			}
			Callback(MoveTemp(Event));
		}

		void DeliverTransportError(WebSocketErrorKind Kind, Errc Code, const char* Message, bool bRetryable)
		{
			WebSocketEvent Event;
			Event.kind = WebSocketEventKind::Error;
			Event.error.kind = Kind;
			Event.error.status = Code;
			Event.error.message = Message;
			Event.error.retryable = bRetryable;
			Deliver(MoveTemp(Event));
		}

		void Start(WebSocketEventCallback InCallback)
		{
			if (bStarted)
			{
				return;
			}
			bStarted = true;
			Callback = MoveTemp(InCallback);

			// Closed while the start was queued. Nothing was opened, so there is nothing to close and no event the
			// caller still wants.
			if (IsClosed())
			{
				return;
			}

#if WITH_WEBSOCKETS
			// Always returns a socket, so there is no null to check for here. A missing WebSockets module is not
			// something this can recover from either: the engine asserts inside the call rather than returning.
			Socket = FWebSocketsModule::Get().CreateWebSocket(Url, Protocol);

			// Bounds the reassembled inbound text message so a hostile or broken server cannot drive an unbounded
			// allocation from the wire. Two caveats worth knowing: the engine counts this limit in characters while
			// the check in DeliverText counts UTF-8 bytes, so the effective bound is whichever is tighter for the
			// encoding in play; and one engine backend accepts the setting and ignores it, which is why that
			// second check exists at all rather than being redundant.
			Socket->SetTextMessageMemoryLimit(MaxFrameBytes);

			const TWeakPtr<FUnrealWebSocketState, ESPMode::ThreadSafe> Weak = AsWeak();

			Socket->OnConnected().AddLambda([Weak]()
			{
				if (const TSharedPtr<FUnrealWebSocketState, ESPMode::ThreadSafe> Live = Weak.Pin())
				{
					Live->bOpened = true;
					Live->ClearConnectTimeout();

					WebSocketEvent Event;
					Event.kind = WebSocketEventKind::Open;
					Live->Deliver(MoveTemp(Event));
				}
			});

			Socket->OnConnectionError().AddLambda([Weak](const FString& Error)
			{
				if (const TSharedPtr<FUnrealWebSocketState, ESPMode::ThreadSafe> Live = Weak.Pin())
				{
					WebSocketEvent Event;
					Event.kind = WebSocketEventKind::Error;
					Event.error.kind = WebSocketErrorKind::Connection;
					Event.error.status = Errc::SocketError;
					Event.error.message = FStringToUtf8(Error);
					Event.error.retryable = true;
					Live->Deliver(MoveTemp(Event));
				}
			});

			Socket->OnClosed().AddLambda([Weak](int32 StatusCode, const FString& Reason, bool bWasClean)
			{
				if (const TSharedPtr<FUnrealWebSocketState, ESPMode::ThreadSafe> Live = Weak.Pin())
				{
					WebSocketEvent Event;
					Event.kind = WebSocketEventKind::Close;
					Event.close.code = static_cast<std::uint16_t>(FMath::Clamp(StatusCode, 0, 65535));
					Event.close.reason = FStringToUtf8(Reason);
					Event.close.clean = bWasClean;
					Live->Deliver(MoveTemp(Event));
				}
			});

			Socket->OnMessage().AddLambda([Weak](const FString& Message)
			{
				if (const TSharedPtr<FUnrealWebSocketState, ESPMode::ThreadSafe> Live = Weak.Pin())
				{
					Live->DeliverText(Message);
				}
			});

			// Neither the binary nor the raw message delegate is bound, deliberately. graphql-transport-ws is a text
			// protocol, so a binary frame is either a broken server or a hostile one, and binding either delegate is
			// what tells the engine to start buffering those frames for delivery. That buffer has no size limit and
			// no way to refuse, so subscribing to it would hand a hostile server an unbounded allocation for frames
			// this code would only throw away. Left unbound, they are discarded before anything is copied, and a
			// server that sends nothing else still ends the connection through the acknowledgement timeout.

			ArmConnectTimeout();

			// Must be the last statement here, and not merely by preference. On some failures the engine reports the
			// error from inside this call rather than later, and that reported error runs the whole failure path
			// synchronously: this connection is closed, released and marked closed before Connect returns. Anything
			// added below would be touching state that has already been torn down.
			Socket->Connect();
#else
			DeliverTransportError(WebSocketErrorKind::Unavailable, Errc::NotConnected,
				"this build has no WebSocket support", false);
#endif
		}

		void DeliverText(const FString& Message)
		{
			const FTCHARToUTF8 Utf8(*Message, Message.Len());
			if (MaxFrameBytes > 0 && static_cast<uint64>(Utf8.Length()) > MaxFrameBytes)
			{
				// The engine's own text limit is set from the same bound and should reject this first. Reporting it
				// here rather than passing it on keeps the configured limit true even if that changes.
				DeliverTransportError(WebSocketErrorKind::FrameTooLarge, Errc::Malformed,
					"an inbound text frame exceeded the configured size limit", false);
				return;
			}

			WebSocketEvent Event;
			Event.kind = WebSocketEventKind::Frame;
			Event.frame.kind = WebSocketFrameKind::Text;
			Event.frame.payload.assign(Utf8.Get(), static_cast<size_t>(Utf8.Length()));
			Deliver(MoveTemp(Event));
		}

		void SendFrame(WebSocketFrame Frame)
		{
			if (IsClosed())
			{
				return;
			}

#if WITH_WEBSOCKETS
			if (!Socket.IsValid() || !Socket->IsConnected())
			{
				DeliverTransportError(WebSocketErrorKind::Io, Errc::NotConnected,
					"the WebSocket is not connected", true);
				return;
			}

			if (Frame.kind == WebSocketFrameKind::Binary)
			{
				Socket->Send(Frame.payload.data(), Frame.payload.size(), true);
				return;
			}
			Socket->Send(Utf8ToFString(Frame.payload));
#else
			DeliverTransportError(WebSocketErrorKind::Unavailable, Errc::NotConnected,
				"this build has no WebSocket support", false);
#endif
		}

		// Closes the socket without releasing it, so this is safe to call from inside a socket event: a UE WebSocket
		// tolerates being closed during its own broadcast, but not being destroyed there.
		void CloseSocket(int32 Code, const FString& Reason)
		{
#if WITH_WEBSOCKETS
			if (Socket.IsValid())
			{
				Socket->Close(Code, Reason);
			}
#else
			(void)Code;
			(void)Reason;
#endif
		}

		// Drops the socket. Only ever called from the deferred release below, never from inside a socket event.
		void ReleaseSocket()
		{
#if WITH_WEBSOCKETS
			Socket.Reset();
#endif
			ClearConnectTimeout();
		}

		void ArmConnectTimeout()
		{
			if (ConnectTimeoutMs <= 0)
			{
				return;
			}

			const TWeakPtr<FUnrealWebSocketState, ESPMode::ThreadSafe> Weak = AsWeak();
			ConnectTimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([Weak](float)
				{
					if (const TSharedPtr<FUnrealWebSocketState, ESPMode::ThreadSafe> Live = Weak.Pin())
					{
						Live->ConnectTimeoutHandle.Reset();
						Live->ReportConnectTimeout();
					}
					return false;
				}),
				static_cast<float>(ConnectTimeoutMs) / 1000.0f);
		}

		void ClearConnectTimeout()
		{
			if (ConnectTimeoutHandle.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(ConnectTimeoutHandle);
				ConnectTimeoutHandle.Reset();
			}
		}

		void ReportConnectTimeout()
		{
			if (bOpened || IsClosed())
			{
				return;
			}

			// Closed first so the stalled socket stops, then reported, so the subscription client sees a failure it
			// can back off and retry from rather than sitting in its one state that has no deadline of its own.
			CloseSocket(4408, TEXT("connect timeout"));
			DeliverTransportError(WebSocketErrorKind::Timeout, Errc::Timeout,
				"the WebSocket did not finish connecting in time", true);
		}
	};

	using FWebSocketStatePtr = TSharedPtr<FUnrealWebSocketState, ESPMode::ThreadSafe>;

	// Connections whose CrowdyCPP-side object is gone but whose engine socket has not been let go of yet.
	//
	// The release cannot happen where the connection dies: that can be inside a socket event, and a UE WebSocket
	// destroyed from inside its own broadcast tears down the object whose callback is still on the stack. It can
	// also be on the subscription client's timer thread, where touching the socket at all is unsafe. So the state
	// is parked here and let go of on the game thread instead.
	FCriticalSection GPendingReleaseLock;
	TArray<FWebSocketStatePtr> GPendingReleases;

	// Let go of everything parked above. Game thread only.
	void DrainPendingReleases()
	{
		TArray<FWebSocketStatePtr> Draining;
		{
			FScopeLock Lock(&GPendingReleaseLock);
			Draining = MoveTemp(GPendingReleases);
			GPendingReleases.Reset();
		}

		for (FWebSocketStatePtr& State : Draining)
		{
			if (State.IsValid())
			{
				State->CloseSocket(1000, TEXT("connection released"));
				State->Callback = nullptr;
				State->ReleaseSocket();
			}
		}
	}

	void ReleaseState(FWebSocketStatePtr State)
	{
		if (!State.IsValid())
		{
			return;
		}
		State->bClosed.store(true, std::memory_order_relaxed);

		{
			FScopeLock Lock(&GPendingReleaseLock);
			GPendingReleases.Add(MoveTemp(State));
		}

		// Queued as the ordinary path. It is only a backstop: an owner disposing of its client drains this
		// explicitly while it still holds the game thread, because a queued task is not something teardown can
		// rely on running at all, and a socket released after the WebSockets module has gone is worse than late.
		if (FTaskGraphInterface::IsRunning())
		{
			AsyncTask(ENamedThreads::GameThread, []() { DrainPendingReleases(); });
		}
		else if (IsInGameThread())
		{
			DrainPendingReleases();
		}
	}

	class FUnrealWebSocketConnection final : public IWebSocketConnection
	{
	public:
		explicit FUnrealWebSocketConnection(const WebSocketConnectRequest& Request)
			: State(MakeShared<FUnrealWebSocketState, ESPMode::ThreadSafe>())
		{
			State->Url = Utf8ToFString(Request.url);
			State->Protocol = Utf8ToFString(Request.subprotocol);
			State->MaxFrameBytes = static_cast<uint64>(Request.maxFrameBytes);
			State->ConnectTimeoutMs = Request.connectTimeoutMs;
		}

		~FUnrealWebSocketConnection() override
		{
			ReleaseState(MoveTemp(State));
		}

		void start(WebSocketEventCallback Callback) override
		{
			// Queued unconditionally, unlike send and close. Creating a socket appends to the engine's own list of
			// live sockets, and the engine walks that list by reference while broadcasting socket events. Running
			// this inline from inside one of those broadcasts would grow the list mid-walk. Nothing reaches here
			// that way today, but the cost of not relying on that is one tick of connect latency.
			AsyncTask(ENamedThreads::GameThread, [Live = State, Callback = MoveTemp(Callback)]() mutable
			{
				Live->Start(MoveTemp(Callback));
			});
		}

		Status send(WebSocketFrame Frame) override
		{
			if (State->IsClosed())
			{
				return Errc::NotConnected;
			}
			if (Frame.kind == WebSocketFrameKind::Ping || Frame.kind == WebSocketFrameKind::Pong)
			{
				// The engine answers protocol-level pings itself and never surfaces one, so the subscription
				// client's pong reply is unreachable here. Accepting it keeps that path from being reported as an
				// I/O failure if a future engine version starts delivering control frames.
				return Errc::Ok;
			}
			if (State->MaxFrameBytes > 0 && Frame.payload.size() > State->MaxFrameBytes)
			{
				return Errc::InvalidArgument;
			}

			// Reported optimistically: the interface allows a later send failure to arrive through the event
			// callback, which is where a queued send that finds a dead socket reports itself.
			RunOnGameThread([Live = State, Frame = MoveTemp(Frame)]() mutable
			{
				Live->SendFrame(MoveTemp(Frame));
			});
			return Errc::Ok;
		}

		void close(std::uint16_t Code, std::string_view Reason) override
		{
			// Marking closed first is what makes this idempotent and safe from the subscription client's timer
			// thread: it stops a queued start from opening anything and stops any late event from being delivered.
			if (State->bClosed.exchange(true, std::memory_order_relaxed))
			{
				return;
			}

			const FString ReasonText = Utf8ToFString(std::string(Reason));
			RunOnGameThread([Live = State, Code, ReasonText]()
			{
				Live->CloseSocket(static_cast<int32>(Code), ReasonText);
			});
		}

	private:
		FWebSocketStatePtr State;
	};

	class FUnrealWebSocketTransport final : public IWebSocketTransport
	{
	public:
		std::shared_ptr<IWebSocketConnection> createConnection(const WebSocketConnectRequest& Request) override
		{
			// Dormant by contract: no engine call is made here, which is what lets the subscription client's timer
			// thread reconnect without hopping threads first. The socket appears in start().
			return std::make_shared<FUnrealWebSocketConnection>(Request);
		}
	};

	class FScriptedWebSocketConnection final : public IWebSocketConnection
	{
	public:
		explicit FScriptedWebSocketConnection(std::shared_ptr<CrowdyCppTransport::FScriptedWebSocketServer> InServer)
			: Server(std::move(InServer))
		{
		}

		void start(WebSocketEventCallback Callback) override
		{
			if (Server)
			{
				Server->NoteStarted(std::move(Callback));
			}
		}

		Status send(WebSocketFrame Frame) override
		{
			if (!Server || Frame.kind != WebSocketFrameKind::Text)
			{
				return Server ? Status(Errc::Ok) : Status(Errc::NotConnected);
			}
			return Server->NoteSent(std::move(Frame.payload)) ? Status(Errc::Ok) : Status(Errc::NotConnected);
		}

		void close(std::uint16_t Code, std::string_view) override
		{
			if (Server)
			{
				Server->NoteClosedByClient(Code);
			}
		}

	private:
		std::shared_ptr<CrowdyCppTransport::FScriptedWebSocketServer> Server;
	};

	class FScriptedWebSocketTransport final : public IWebSocketTransport
	{
	public:
		explicit FScriptedWebSocketTransport(std::shared_ptr<CrowdyCppTransport::FScriptedWebSocketServer> InServer)
			: Server(std::move(InServer))
		{
		}

		std::shared_ptr<IWebSocketConnection> createConnection(const WebSocketConnectRequest&) override
		{
			if (!Server)
			{
				return nullptr;
			}
			Server->NoteConnectionCreated();
			return std::make_shared<FScriptedWebSocketConnection>(Server);
		}

	private:
		std::shared_ptr<CrowdyCppTransport::FScriptedWebSocketServer> Server;
	};
}

namespace CrowdyCppTransport
{
	WebSocketEventCallback FScriptedWebSocketServer::LiveCallback() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Callback;
	}

	void FScriptedWebSocketServer::Open()
	{
		const WebSocketEventCallback Live = LiveCallback();
		if (!Live)
		{
			return;
		}
		WebSocketEvent Event;
		Event.kind = WebSocketEventKind::Open;
		Live(std::move(Event));
	}

	void FScriptedWebSocketServer::ReceiveText(const std::string& Text)
	{
		const WebSocketEventCallback Live = LiveCallback();
		if (!Live)
		{
			return;
		}
		WebSocketEvent Event;
		Event.kind = WebSocketEventKind::Frame;
		Event.frame.kind = WebSocketFrameKind::Text;
		Event.frame.payload = Text;
		Live(std::move(Event));
	}

	void FScriptedWebSocketServer::CloseFromServer(std::uint16_t Code, const std::string& Reason, bool bClean)
	{
		const WebSocketEventCallback Live = LiveCallback();
		if (!Live)
		{
			return;
		}
		WebSocketEvent Event;
		Event.kind = WebSocketEventKind::Close;
		Event.close.code = Code;
		Event.close.reason = Reason;
		Event.close.clean = bClean;
		Live(std::move(Event));
	}

	int FScriptedWebSocketServer::ConnectionsCreated() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return ConnectionsCreatedCount;
	}

	bool FScriptedWebSocketServer::WasClosedByClient() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return bClosedByClient;
	}

	std::uint16_t FScriptedWebSocketServer::ClientCloseCode() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return CloseCode;
	}

	std::vector<std::string> FScriptedWebSocketServer::TakeSentFrames()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<std::string> Taken;
		Taken.swap(SentFrames);
		return Taken;
	}

	void FScriptedWebSocketServer::NoteConnectionCreated()
	{
		// A reconnect gets a fresh connection against the same server, so the per-connection state resets while the
		// sent-frame log stays where the test left it and the whole conversation stays visible.
		std::lock_guard<std::mutex> Lock(Mutex);
		++ConnectionsCreatedCount;
		bClosedByClient = false;
		CloseCode = 0;
		Callback = nullptr;
	}

	void FScriptedWebSocketServer::NoteStarted(WebSocketEventCallback InCallback)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Callback = std::move(InCallback);
	}

	bool FScriptedWebSocketServer::NoteSent(std::string Text)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (bClosedByClient)
		{
			return false;
		}
		SentFrames.push_back(std::move(Text));
		return true;
	}

	void FScriptedWebSocketServer::NoteClosedByClient(std::uint16_t Code)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (bClosedByClient)
		{
			return;
		}
		bClosedByClient = true;
		CloseCode = Code;
		// Dropped with the close, so a late server-side event in a test reaches nobody, exactly as it would not
		// reach a closed socket.
		Callback = nullptr;
	}

	void FlushPendingWebSocketReleases()
	{
		if (IsInGameThread())
		{
			DrainPendingReleases();
		}
	}

	std::shared_ptr<IWebSocketTransport> MakeFWebSocketTransport()
	{
#if WITH_WEBSOCKETS
		return std::make_shared<FUnrealWebSocketTransport>();
#else
		// The subscription client reports a null transport as unavailable, which is the honest answer on a platform
		// built without WebSocket support and is not something a retry would fix.
		UE_LOG(LogCrowdyCpp, Log, TEXT("This build has no WebSocket support, so GraphQL subscriptions are unavailable."));
		return nullptr;
#endif
	}

	std::shared_ptr<IWebSocketTransport> MakeScriptedWebSocketTransport(std::shared_ptr<FScriptedWebSocketServer> Server)
	{
		return std::make_shared<FScriptedWebSocketTransport>(std::move(Server));
	}
}
