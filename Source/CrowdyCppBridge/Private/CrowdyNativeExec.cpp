#include "CrowdyNativeExecInternal.h"

#include "Containers/StringConv.h"

#include <cstdint>
#include <string>
#include <utility>

namespace
{
	std::string CrowdyNativeExecToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value, Value.Len());
		return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
	}

	FString CrowdyNativeExecFromUtf8(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(Value.data(), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	TArray<uint8> CrowdyNativeExecBytes(const std::string& Value)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Value.data()), static_cast<int32>(Value.size()));
	}

	FCrowdyNativeExecReply CrowdyNativeExecReplyFrom(const crowdy::domains::ExecReply& Reply)
	{
		FCrowdyNativeExecReply Out;
		Out.Status = static_cast<ECrowdyNativeExecStatus>(Reply.status);
		Out.bRetryable = Reply.retryable();
		Out.bRateLimited = Reply.rateLimited();
		const std::optional<long> RetryAfter = Reply.retryAfterMs();
		Out.RetryAfterMs = RetryAfter ? static_cast<int32>(FMath::Clamp<long>(*RetryAfter, 0, MAX_int32)) : -1;
		if (Reply.ok())
		{
			Out.Payload = CrowdyNativeExecBytes(Reply.payload);
			return Out;
		}
		Out.Message = CrowdyNativeExecFromUtf8(Reply.payload);
		return Out;
	}

	FCrowdyNativeExecReply CrowdyNativeExecRefusal(ECrowdyNativeExecStatus Status, const FString& Message)
	{
		FCrowdyNativeExecReply Out;
		Out.Status = Status;
		Out.Message = Message;
		return Out;
	}

	// Frame layout of a call: tag, rid, node type (str8), key (str16), method (str8), then the payload.
	uint64 CrowdyNativeExecCallFrameBytes(const std::string& NodeType, const std::string& Key, const std::string& Method,
		int32 PayloadBytes)
	{
		return 1 + 4 + 1 + NodeType.size() + 2 + Key.size() + 1 + Method.size() + static_cast<uint64>(PayloadBytes);
	}
}

struct FCrowdyNativeExecConnection::FImpl : public TSharedFromThis<FImpl, ESPMode::ThreadSafe>
{
	using FReplyCallback = TFunction<void(const FCrowdyNativeExecReply&)>;
	using FConnectCallback = TFunction<void(bool, const FString&)>;

	std::shared_ptr<crowdy::domains::ExecConnection> Connection;

	// Completions not yet delivered, by id, so Close can deliver each exactly once whatever CrowdyCPP does later.
	TMap<uint64, FReplyCallback> PendingReplies;
	TMap<uint64, FConnectCallback> PendingConnects;
	uint64 NextId = 0;
	bool bClosed = false;

	uint64 TrackReply(FReplyCallback OnDone)
	{
		PendingReplies.Add(++NextId, MoveTemp(OnDone));
		return NextId;
	}

	void CompleteReply(uint64 Id, const FCrowdyNativeExecReply& Reply)
	{
		FReplyCallback OnDone;
		if (!PendingReplies.RemoveAndCopyValue(Id, OnDone) || !OnDone)
		{
			return;
		}
		OnDone(Reply);
	}

	void CompleteConnect(uint64 Id, bool bConnected, const FString& Error)
	{
		FConnectCallback OnDone;
		if (!PendingConnects.RemoveAndCopyValue(Id, OnDone) || !OnDone)
		{
			return;
		}
		OnDone(bConnected, Error);
	}

	crowdy::domains::ExecReplyCallback ReplyTo(uint64 Id)
	{
		TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = AsWeak();
		return [Weak, Id](crowdy::domains::ExecReply Reply)
		{
			if (const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin())
			{
				Live->CompleteReply(Id, CrowdyNativeExecReplyFrom(Reply));
			}
		};
	}
};

namespace CrowdyNativeExec
{
	TSharedRef<FCrowdyNativeExecConnection> Wrap(std::shared_ptr<crowdy::domains::ExecConnection> Connection)
	{
		TSharedRef<FCrowdyNativeExecConnection::FImpl, ESPMode::ThreadSafe> Impl =
			MakeShared<FCrowdyNativeExecConnection::FImpl, ESPMode::ThreadSafe>();
		Impl->Connection = std::move(Connection);
		return MakeShared<FCrowdyNativeExecConnection>(Impl);
	}
}

FCrowdyNativeExecConnection::FCrowdyNativeExecConnection(TSharedRef<FImpl> InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FCrowdyNativeExecConnection::~FCrowdyNativeExecConnection()
{
	Close();
}

const FString& FCrowdyNativeExecConnection::CanceledMessage()
{
	static const FString Message = TEXT("the ck-exec connection is closed");
	return Message;
}

void FCrowdyNativeExecConnection::Connect(TFunction<void(bool bConnected, const FString& Error)> OnDone)
{
	if (Impl->bClosed)
	{
		OnDone(false, CanceledMessage());
		return;
	}

	const uint64 Id = ++Impl->NextId;
	Impl->PendingConnects.Add(Id, MoveTemp(OnDone));
	TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = Impl->AsWeak();
	Impl->Connection->connect([Weak, Id](crowdy::Status Result)
	{
		if (const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin())
		{
			Live->CompleteConnect(Id, Result.ok(), Result.ok() ? FString() : FString(UTF8_TO_TCHAR(crowdy::errcName(Result.code))));
		}
	});
}

void FCrowdyNativeExecConnection::Call(const FString& NodeType, const FString& Key, const FString& Method,
	TArray<uint8> Payload, TFunction<void(const FCrowdyNativeExecReply&)> OnDone)
{
	if (Impl->bClosed)
	{
		OnDone(CrowdyNativeExecRefusal(ECrowdyNativeExecStatus::Unavailable, CanceledMessage()));
		return;
	}

	std::string Type = CrowdyNativeExecToUtf8(NodeType);
	std::string InstanceKey = CrowdyNativeExecToUtf8(Key);
	std::string Name = CrowdyNativeExecToUtf8(Method);

	// The transport refuses an oversized frame without telling CrowdyCPP, which would then wait out the call timeout.
	if (CrowdyNativeExecCallFrameBytes(Type, InstanceKey, Name, Payload.Num()) > crowdy::graphql::kDefaultWebSocketFrameLimit)
	{
		OnDone(CrowdyNativeExecRefusal(ECrowdyNativeExecStatus::BadRequest,
			TEXT("the call is larger than one ck-exec frame allows")));
		return;
	}

	const uint64 Id = Impl->TrackReply(MoveTemp(OnDone));
	Impl->Connection->callRaw(std::move(Type), std::move(InstanceKey), std::move(Name),
		std::string(reinterpret_cast<const char*>(Payload.GetData()), static_cast<size_t>(Payload.Num())), Impl->ReplyTo(Id));
}

uint64 FCrowdyNativeExecConnection::Subscribe(const FString& NodeType, const FString& Key, const FString& Topic,
	TFunction<void(const FCrowdyNativeExecPush&)> OnPush, TFunction<void(const FCrowdyNativeExecReply&)> OnDone)
{
	if (Impl->bClosed)
	{
		OnDone(CrowdyNativeExecRefusal(ECrowdyNativeExecStatus::Unavailable, CanceledMessage()));
		return 0;
	}

	const uint64 Id = Impl->TrackReply(MoveTemp(OnDone));
	TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = Impl->AsWeak();
	crowdy::domains::ExecPushHandler Handler = [Weak, OnPush](const crowdy::domains::ExecPush& Push)
	{
		const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin();
		if (!Live || Live->bClosed || !OnPush)
		{
			return;
		}
		FCrowdyNativeExecPush Out;
		Out.NodeType = CrowdyNativeExecFromUtf8(Push.nodeType);
		Out.Key = CrowdyNativeExecFromUtf8(Push.key);
		Out.Topic = CrowdyNativeExecFromUtf8(Push.topic);
		Out.Payload = CrowdyNativeExecBytes(Push.payload);
		OnPush(Out);
	};
	return Impl->Connection->subscribe(CrowdyNativeExecToUtf8(NodeType), CrowdyNativeExecToUtf8(Key),
		CrowdyNativeExecToUtf8(Topic), std::move(Handler), Impl->ReplyTo(Id));
}

void FCrowdyNativeExecConnection::Unsubscribe(uint64 Handle)
{
	if (Impl->bClosed || Handle == 0)
	{
		return;
	}
	Impl->Connection->unsubscribe(Handle);
}

void FCrowdyNativeExecConnection::Ping(TFunction<void(const FCrowdyNativeExecReply&)> OnDone)
{
	if (Impl->bClosed)
	{
		OnDone(CrowdyNativeExecRefusal(ECrowdyNativeExecStatus::Unavailable, CanceledMessage()));
		return;
	}
	const uint64 Id = Impl->TrackReply(MoveTemp(OnDone));
	Impl->Connection->ping(Impl->ReplyTo(Id));
}

void FCrowdyNativeExecConnection::OnReconnect(TFunction<void(const FString& Host)> Listener)
{
	if (Impl->bClosed || !Listener)
	{
		return;
	}
	TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = Impl->AsWeak();
	Impl->Connection->onReconnect([Weak, Listener](std::string Host)
	{
		const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin();
		if (Live && !Live->bClosed)
		{
			Listener(CrowdyNativeExecFromUtf8(Host));
		}
	});
}

FString FCrowdyNativeExecConnection::GetHost() const
{
	return Impl->bClosed ? FString() : CrowdyNativeExecFromUtf8(Impl->Connection->host());
}

bool FCrowdyNativeExecConnection::IsConnected() const
{
	return !Impl->bClosed && Impl->Connection->connected();
}

void FCrowdyNativeExecConnection::Close()
{
	if (Impl->bClosed)
	{
		return;
	}
	Impl->bClosed = true;
	Impl->Connection->close();

	// Delivered here rather than through CrowdyCPP, whose own failures may be posted to a dispatcher that is about to
	// close with the client and would never run.
	TMap<uint64, FImpl::FReplyCallback> Replies = MoveTemp(Impl->PendingReplies);
	TMap<uint64, FImpl::FConnectCallback> Connects = MoveTemp(Impl->PendingConnects);
	Impl->PendingReplies.Reset();
	Impl->PendingConnects.Reset();
	const FCrowdyNativeExecReply Canceled = CrowdyNativeExecRefusal(ECrowdyNativeExecStatus::Unavailable, CanceledMessage());
	for (TPair<uint64, FImpl::FReplyCallback>& Pending : Replies)
	{
		if (Pending.Value)
		{
			Pending.Value(Canceled);
		}
	}
	for (TPair<uint64, FImpl::FConnectCallback>& Pending : Connects)
	{
		if (Pending.Value)
		{
			Pending.Value(false, CanceledMessage());
		}
	}
}
