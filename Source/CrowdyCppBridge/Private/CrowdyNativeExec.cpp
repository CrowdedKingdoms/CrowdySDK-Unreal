#include "CrowdyNativeExecInternal.h"

#include "Containers/StringConv.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/graphql/json.hpp"
THIRD_PARTY_INCLUDES_END

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

	constexpr int32 CrowdyNativeExecMaxDepth = 64;

	// The packer recurses per nesting level, so deep text would exhaust the stack.
	bool CrowdyNativeExecJsonDepthWithin(const FString& Json)
	{
		int32 Depth = 0;
		bool bInString = false;
		bool bEscaped = false;
		for (const TCHAR Char : Json)
		{
			if (bInString)
			{
				const bool bWasEscaped = bEscaped;
				bEscaped = !bWasEscaped && Char == TEXT('\\');
				bInString = bWasEscaped || Char != TEXT('"');
				continue;
			}
			if (Char == TEXT('"'))
			{
				bInString = true;
			}
			else if (Char == TEXT('[') || Char == TEXT('{'))
			{
				if (++Depth > CrowdyNativeExecMaxDepth)
				{
					return false;
				}
			}
			else if (Char == TEXT(']') || Char == TEXT('}'))
			{
				--Depth;
			}
		}
		return true;
	}

	// The converter writes a non-string map key as escaped JSON text, which doubles at every nesting level, so a
	// container, binary or extension key is refused before conversion.
	bool CrowdyNativeExecMsgpackKeysSafe(TConstArrayView<uint8> Bytes)
	{
		struct FFrame
		{
			uint64 Left;
			bool bMap;
		};
		TArray<FFrame, TInlineAllocator<CrowdyNativeExecMaxDepth>> Stack;
		const uint64 Size = static_cast<uint64>(Bytes.Num());
		uint64 Pos = 0;
		bool bRootRead = false;
		auto ReadWidth = [&Bytes, &Pos, Size](int32 Width, uint64& Out)
		{
			if (Size - Pos < static_cast<uint64>(Width))
			{
				return false;
			}
			Out = 0;
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out = (Out << 8) | Bytes[static_cast<int32>(Pos++)];
			}
			return true;
		};
		while (true)
		{
			bool bKey = false;
			if (!Stack.IsEmpty())
			{
				FFrame& Top = Stack.Last();
				if (Top.Left == 0)
				{
					Stack.Pop(EAllowShrinking::No);
					continue;
				}
				bKey = Top.bMap && Top.Left % 2 == 0;
				--Top.Left;
			}
			else if (bRootRead)
			{
				return true;
			}
			bRootRead = true;
			if (Pos >= Size)
			{
				return false;
			}
			const uint8 Tag = Bytes[static_cast<int32>(Pos++)];
			uint64 Count = 0;
			uint64 Skip = 0;
			int32 Kind = 0;
			if (Tag <= 0x7f || Tag >= 0xe0 || Tag == 0xc0 || Tag == 0xc2 || Tag == 0xc3)
			{
				Kind = 0;
			}
			else if (Tag >= 0xa0 && Tag <= 0xbf)
			{
				Skip = Tag & 0x1f;
			}
			else if (Tag >= 0x90 && Tag <= 0x9f)
			{
				Kind = 1;
				Count = Tag & 0x0f;
			}
			else if (Tag >= 0x80 && Tag <= 0x8f)
			{
				Kind = 2;
				Count = Tag & 0x0f;
			}
			else
			{
				switch (Tag)
				{
				case 0xc4: case 0xc5: case 0xc6:
					Kind = 3;
					if (!ReadWidth(Tag == 0xc4 ? 1 : (Tag == 0xc5 ? 2 : 4), Skip)) { return false; }
					break;
				case 0xc7: case 0xc8: case 0xc9:
					Kind = 3;
					if (!ReadWidth(Tag == 0xc7 ? 1 : (Tag == 0xc8 ? 2 : 4), Skip)) { return false; }
					++Skip;
					break;
				case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
					Kind = 3;
					Skip = 1 + (1ull << (Tag - 0xd4));
					break;
				case 0xca: case 0xce: case 0xd2: Skip = 4; break;
				case 0xcb: case 0xcf: case 0xd3: Skip = 8; break;
				case 0xcc: case 0xd0: Skip = 1; break;
				case 0xcd: case 0xd1: Skip = 2; break;
				case 0xd9: case 0xda: case 0xdb:
					if (!ReadWidth(Tag == 0xd9 ? 1 : (Tag == 0xda ? 2 : 4), Skip)) { return false; }
					break;
				case 0xdc: case 0xdd:
					Kind = 1;
					if (!ReadWidth(Tag == 0xdc ? 2 : 4, Count)) { return false; }
					break;
				case 0xde: case 0xdf:
					Kind = 2;
					if (!ReadWidth(Tag == 0xde ? 2 : 4, Count)) { return false; }
					break;
				default:
					return false;
				}
			}
			if (bKey && Kind != 0)
			{
				return false;
			}
			if (Size - Pos < Skip)
			{
				return false;
			}
			Pos += Skip;
			if (Kind == 1 || Kind == 2)
			{
				if (Stack.Num() >= CrowdyNativeExecMaxDepth)
				{
					return false;
				}
				Stack.Add({Kind == 2 ? Count * 2 : Count, Kind == 2});
			}
		}
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
	std::shared_ptr<FString> DialRefusal;
	std::shared_ptr<bool> RefusedForGood;

	// Completions not yet delivered, by id, so Close can deliver each exactly once whatever CrowdyCPP does later.
	TMap<uint64, FReplyCallback> PendingReplies;
	TMap<uint64, FConnectCallback> PendingConnects;
	uint64 NextId = 0;
	bool bClosed = false;

	TArray<TFunction<void(const FString&)>> AccessEndedListeners;
	FString AccessEndedReason;
	bool bAccessEnded = false;

	void HandleAccessEnded(const std::string& Reason)
	{
		if (bClosed || bAccessEnded)
		{
			return;
		}
		bAccessEnded = true;
		AccessEndedReason = Reason.empty() ? FString(TEXT("access ended")) : CrowdyNativeExecFromUtf8(Reason);
		TArray<TFunction<void(const FString&)>> Listeners = MoveTemp(AccessEndedListeners);
		AccessEndedListeners.Reset();
		for (const TFunction<void(const FString&)>& Listener : Listeners)
		{
			Listener(AccessEndedReason);
		}
	}

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

	/** Why connecting failed in the platform's words: the Game API's refusal to name a host, else the gateway's, else the failure's name. */
	FString ConnectFailure(const crowdy::Status& Result) const
	{
		if (DialRefusal && !DialRefusal->IsEmpty())
		{
			return *DialRefusal;
		}
		const std::optional<crowdy::domains::ExecReply> Failure = Connection->lastFailure();
		return Failure && !Failure->payload.empty() ? CrowdyNativeExecFromUtf8(Failure->payload) : FString(UTF8_TO_TCHAR(crowdy::errcName(Result.code)));
	}

	crowdy::domains::ExecReplyCallback ReplyTo(uint64 Id)
	{
		TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = AsWeak();
		return [Weak, Id](crowdy::domains::ExecReply Reply)
		{
			if (const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin())
			{
				Live->CompleteReply(Id, CrowdyNativeExec::ReplyFrom(Reply));
			}
		};
	}
};

namespace CrowdyNativeExec
{
	TSharedRef<FCrowdyNativeExecConnection> Wrap(std::shared_ptr<crowdy::domains::ExecConnection> Connection, std::shared_ptr<FString> DialRefusal,
		std::shared_ptr<bool> RefusedForGood)
	{
		TSharedRef<FCrowdyNativeExecConnection::FImpl, ESPMode::ThreadSafe> Impl =
			MakeShared<FCrowdyNativeExecConnection::FImpl, ESPMode::ThreadSafe>();
		Impl->Connection = std::move(Connection);
		Impl->DialRefusal = std::move(DialRefusal);
		Impl->RefusedForGood = std::move(RefusedForGood);
		TWeakPtr<FCrowdyNativeExecConnection::FImpl, ESPMode::ThreadSafe> Weak = Impl->AsWeak();
		Impl->Connection->onAccessEnded([Weak](std::string Reason)
		{
			if (const TSharedPtr<FCrowdyNativeExecConnection::FImpl, ESPMode::ThreadSafe> Live = Weak.Pin())
			{
				Live->HandleAccessEnded(Reason);
			}
		});
		return MakeShared<FCrowdyNativeExecConnection>(Impl);
	}

	bool JsonToMsgpack(const FString& Json, TArray<uint8>& OutBytes)
	{
		if (!CrowdyNativeExecJsonDepthWithin(Json))
		{
			return false;
		}
		try
		{
			const crowdy::graphql::Json Parsed = crowdy::graphql::Json::parse(CrowdyNativeExecToUtf8(Json));
			if (!Parsed.ok())
			{
				return false;
			}
			OutBytes = CrowdyNativeExecBytes(Parsed.toMsgpack());
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool MsgpackToJson(TConstArrayView<uint8> Bytes, FString& OutJson)
	{
		if (Bytes.IsEmpty())
		{
			OutJson = TEXT("null");
			return true;
		}
		if (!CrowdyNativeExecMsgpackKeysSafe(Bytes))
		{
			return false;
		}
		try
		{
			const crowdy::graphql::Json Parsed = crowdy::graphql::Json::fromMsgpack(
				std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())));
			if (!Parsed.ok())
			{
				return false;
			}
			OutJson = CrowdyNativeExecFromUtf8(Parsed.dump());
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	FCrowdyNativeExecReply ReplyFrom(const crowdy::domains::ExecReply& Reply)
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
		// The client's own call timeout, as opposed to a deadline the server reports.
		Out.bNoReply = Out.Status == ECrowdyNativeExecStatus::DeadlineExceeded && Out.Message.StartsWith(TEXT("no reply in "), ESearchCase::CaseSensitive);
		return Out;
	}
}

std::shared_ptr<crowdy::domains::ExecConnection> FCrowdyNativeExecAccess::Connection(const FCrowdyNativeExecConnection& Wrapped)
{
	return Wrapped.Impl->bClosed ? nullptr : Wrapped.Impl->Connection;
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
			Live->CompleteConnect(Id, Result.ok(), Result.ok() ? FString() : Live->ConnectFailure(Result));
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

void FCrowdyNativeExecConnection::OnAccessEnded(TFunction<void(const FString& Reason)> Listener)
{
	if (Impl->bClosed || !Listener)
	{
		return;
	}
	if (Impl->bAccessEnded)
	{
		Listener(Impl->AccessEndedReason);
		return;
	}
	Impl->AccessEndedListeners.Add(MoveTemp(Listener));
}

bool FCrowdyNativeExecConnection::IsAccessEnded() const
{
	return Impl->bAccessEnded;
}

FString FCrowdyNativeExecConnection::GetAccessEndedReason() const
{
	return Impl->AccessEndedReason;
}

FString FCrowdyNativeExecConnection::GetHost() const
{
	return Impl->bClosed ? FString() : CrowdyNativeExecFromUtf8(Impl->Connection->host());
}

bool FCrowdyNativeExecConnection::IsConnected() const
{
	return !Impl->bClosed && Impl->Connection->connected();
}

bool FCrowdyNativeExecConnection::IsRefusedForGood() const
{
	return Impl->RefusedForGood && *Impl->RefusedForGood;
}

FString FCrowdyNativeExecConnection::GetRefusal() const
{
	return Impl->DialRefusal ? *Impl->DialRefusal : FString();
}

void FCrowdyNativeExecConnection::Close()
{
	if (Impl->bClosed)
	{
		return;
	}
	Impl->bClosed = true;
	Impl->AccessEndedListeners.Reset();
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
