#include "CrowdyNativeKitInternal.h"

#include "CrowdyCppBridge.h"
#include "Containers/StringConv.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/kit/kit.hpp"
#include "crowdy/kit/realtime.hpp"
THIRD_PARTY_INCLUDES_END

#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

namespace
{
	std::string CrowdyNativeKitToUtf8(const FString& Value)
	{
		const FTCHARToUTF8 Converted(*Value, Value.Len());
		return std::string(Converted.Get(), static_cast<size_t>(Converted.Length()));
	}

	FString CrowdyNativeKitFromUtf8(const std::string& Value)
	{
		const auto Converted = StringCast<TCHAR>(Value.data(), static_cast<int32>(Value.size()));
		return FString(Converted.Length(), Converted.Get());
	}

	FCrowdyNativeKitReply CrowdyNativeKitRefusal(ECrowdyNativeExecStatus Status, const FString& Message)
	{
		FCrowdyNativeKitReply Out;
		Out.Status = Status;
		Out.Message = Message;
		return Out;
	}

	FCrowdyNativeKitReply CrowdyNativeKitReplyFrom(const crowdy::kit::KitReply& Reply)
	{
		FCrowdyNativeKitReply Out;
		Out.Status = static_cast<ECrowdyNativeExecStatus>(Reply.status);
		if (Reply.ok())
		{
			Out.Payload = TArray<uint8>(reinterpret_cast<const uint8*>(Reply.payload.data()), static_cast<int32>(Reply.payload.size()));
			return Out;
		}
		// The kit keeps only the status and the words, which are all the connection reads these from.
		const crowdy::domains::ExecReply Exec{Reply.status, Reply.payload};
		Out.bRetryable = Exec.retryable();
		Out.bRateLimited = Exec.rateLimited();
		const std::optional<long> RetryAfter = Exec.retryAfterMs();
		Out.RetryAfterMs = RetryAfter ? static_cast<int32>(FMath::Clamp<long>(*RetryAfter, 0, MAX_int32)) : -1;
		Out.Code = CrowdyNativeKitFromUtf8(Reply.code());
		Out.Message = CrowdyNativeKitFromUtf8(Reply.refusal ? Reply.refusal->text : Reply.payload);
		Out.bNoReply = Out.Status == ECrowdyNativeExecStatus::DeadlineExceeded && Out.Message.StartsWith(TEXT("no reply in "), ESearchCase::CaseSensitive);
		return Out;
	}

	struct FCrowdyNativeKitReader
	{
		const uint8* Data = nullptr;
		int32 Num = 0;
		int32 At = 0;

		bool Has(uint64 Count) const { return static_cast<uint64>(Num - At) >= Count; }

		uint64 Big(int32 Bytes)
		{
			uint64 Value = 0;
			for (int32 Index = 0; Index < Bytes; ++Index)
			{
				Value = (Value << 8) | Data[At++];
			}
			return Value;
		}
	};

	bool CrowdyNativeKitReadValue(FCrowdyNativeKitReader& Reader, crowdy::graphql::JVal& Out, int32 Depth);

	bool CrowdyNativeKitReadString(FCrowdyNativeKitReader& Reader, uint64 Length, std::string& Out)
	{
		if (!Reader.Has(Length))
		{
			return false;
		}
		Out.assign(reinterpret_cast<const char*>(Reader.Data + Reader.At), static_cast<size_t>(Length));
		Reader.At += static_cast<int32>(Length);
		return true;
	}

	bool CrowdyNativeKitReadArray(FCrowdyNativeKitReader& Reader, uint64 Count, crowdy::graphql::JVal& Out, int32 Depth)
	{
		// Every element takes at least one byte, which bounds what a forged count can reserve.
		if (!Reader.Has(Count))
		{
			return false;
		}
		crowdy::graphql::JArray Items;
		Items.reserve(static_cast<size_t>(FMath::Min<uint64>(Count, 1024)));
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			crowdy::graphql::JVal Item;
			if (!CrowdyNativeKitReadValue(Reader, Item, Depth + 1))
			{
				return false;
			}
			Items.push_back(std::move(Item));
		}
		Out = crowdy::graphql::JVal(std::move(Items));
		return true;
	}

	bool CrowdyNativeKitReadMap(FCrowdyNativeKitReader& Reader, uint64 Count, crowdy::graphql::JVal& Out, int32 Depth)
	{
		if (!Reader.Has(Count * 2))
		{
			return false;
		}
		crowdy::graphql::JObject Members;
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			crowdy::graphql::JVal Key;
			crowdy::graphql::JVal Value;
			if (!CrowdyNativeKitReadValue(Reader, Key, Depth + 1) || !Key.asStringPtr() || !CrowdyNativeKitReadValue(Reader, Value, Depth + 1))
			{
				return false;
			}
			Members.insert_or_assign(*Key.asStringPtr(), std::move(Value));
		}
		Out = crowdy::graphql::JVal(std::move(Members));
		return true;
	}

	bool CrowdyNativeKitReadValue(FCrowdyNativeKitReader& Reader, crowdy::graphql::JVal& Out, int32 Depth)
	{
		if (Depth > 64 || !Reader.Has(1))
		{
			return false;
		}
		const uint8 Tag = Reader.Data[Reader.At++];
		if (Tag <= 0x7f)
		{
			Out = crowdy::graphql::JVal(static_cast<std::int64_t>(Tag));
			return true;
		}
		if (Tag >= 0xe0)
		{
			Out = crowdy::graphql::JVal(static_cast<std::int64_t>(static_cast<int8>(Tag)));
			return true;
		}
		if ((Tag & 0xf0) == 0x80)
		{
			return CrowdyNativeKitReadMap(Reader, Tag & 0x0f, Out, Depth);
		}
		if ((Tag & 0xf0) == 0x90)
		{
			return CrowdyNativeKitReadArray(Reader, Tag & 0x0f, Out, Depth);
		}
		std::string Text;
		if ((Tag & 0xe0) == 0xa0)
		{
			if (!CrowdyNativeKitReadString(Reader, Tag & 0x1f, Text))
			{
				return false;
			}
			Out = crowdy::graphql::JVal(std::move(Text));
			return true;
		}

		const int32 Width = Tag == 0xcc || Tag == 0xd0 || Tag == 0xd9 ? 1
			: Tag == 0xcd || Tag == 0xd1 || Tag == 0xda || Tag == 0xdc || Tag == 0xde ? 2
			: Tag == 0xce || Tag == 0xd2 || Tag == 0xdb || Tag == 0xdd || Tag == 0xdf || Tag == 0xca ? 4
			: Tag == 0xcf || Tag == 0xd3 || Tag == 0xcb ? 8 : 0;
		if (Width > 0 && !Reader.Has(Width))
		{
			return false;
		}
		switch (Tag)
		{
		case 0xc0:
			Out = crowdy::graphql::JVal(nullptr);
			return true;
		case 0xc2:
		case 0xc3:
			Out = crowdy::graphql::JVal(Tag == 0xc3);
			return true;
		case 0xca:
		{
			const uint32 Bits = static_cast<uint32>(Reader.Big(4));
			float Value;
			std::memcpy(&Value, &Bits, sizeof(Value));
			Out = crowdy::graphql::JVal(static_cast<double>(Value));
			return true;
		}
		case 0xcb:
		{
			const uint64 Bits = Reader.Big(8);
			double Value;
			std::memcpy(&Value, &Bits, sizeof(Value));
			Out = crowdy::graphql::JVal(Value);
			return true;
		}
		case 0xcc:
		case 0xcd:
		case 0xce:
		case 0xcf:
		{
			const uint64 Value = Reader.Big(Width);
			if (Value > static_cast<uint64>(std::numeric_limits<std::int64_t>::max()))
			{
				return false;
			}
			Out = crowdy::graphql::JVal(static_cast<std::int64_t>(Value));
			return true;
		}
		case 0xd0:
		case 0xd1:
		case 0xd2:
		case 0xd3:
		{
			const uint64 Raw = Reader.Big(Width);
			const int32 Shift = 64 - Width * 8;
			Out = crowdy::graphql::JVal(static_cast<std::int64_t>(Raw << Shift) >> Shift);
			return true;
		}
		case 0xd9:
		case 0xda:
		case 0xdb:
			if (!CrowdyNativeKitReadString(Reader, Reader.Big(Width), Text))
			{
				return false;
			}
			Out = crowdy::graphql::JVal(std::move(Text));
			return true;
		case 0xdc:
		case 0xdd:
			return CrowdyNativeKitReadArray(Reader, Reader.Big(Width), Out, Depth);
		case 0xde:
		case 0xdf:
			return CrowdyNativeKitReadMap(Reader, Reader.Big(Width), Out, Depth);
		default:
			return false;
		}
	}
}

struct FCrowdyNativeKit::FImpl : public TSharedFromThis<FImpl, ESPMode::ThreadSafe>
{
	using FReplyCallback = TFunction<void(const FCrowdyNativeKitReply&)>;

	std::unique_ptr<crowdy::kit::GameKitClient> Kit;
	std::shared_ptr<crowdy::domains::ExecConnection> Connection;

	// Completions not yet delivered, by id, so Close can deliver each exactly once whatever CrowdyCPP does later.
	TMap<uint64, FReplyCallback> PendingReplies;

	// Each subscription's flag, cleared when it ends, so a push already queued for it is dropped rather than delivered.
	TMap<uint64, TSharedRef<bool, ESPMode::ThreadSafe>> Subscriptions;
	uint64 NextId = 0;
	bool bClosed = false;

	uint64 TrackReply(FReplyCallback OnDone)
	{
		PendingReplies.Add(++NextId, MoveTemp(OnDone));
		return NextId;
	}

	void CompleteReply(uint64 Id, const FCrowdyNativeKitReply& Reply)
	{
		FReplyCallback OnDone;
		if (!PendingReplies.RemoveAndCopyValue(Id, OnDone) || !OnDone)
		{
			return;
		}
		OnDone(Reply);
	}

	crowdy::kit::KitCallback ReplyTo(uint64 Id)
	{
		TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = AsWeak();
		return [Weak, Id](crowdy::kit::KitReply Reply)
		{
			if (const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin())
			{
				Live->CompleteReply(Id, CrowdyNativeKitReplyFrom(Reply));
			}
		};
	}

	void EndSubscription(uint64 Handle, const TSharedRef<bool, ESPMode::ThreadSafe>& bLive)
	{
		*bLive = false;
		Connection->unsubscribe(Handle);
	}
};

namespace CrowdyNativeKit
{
	TSharedPtr<FCrowdyNativeKit> Make(crowdy::CrowdyClient& Client, std::string AppId,
		std::shared_ptr<crowdy::domains::ExecConnection> Connection, std::string PlayerId)
	{
		if (!Connection)
		{
			return nullptr;
		}
		crowdy::kit::GameKitOptions Options;
		Options.runtime.connection = Connection;
		Options.runtime.playerId = std::move(PlayerId);
		try
		{
			TSharedRef<FCrowdyNativeKit::FImpl, ESPMode::ThreadSafe> Impl = MakeShared<FCrowdyNativeKit::FImpl, ESPMode::ThreadSafe>();
			Impl->Connection = std::move(Connection);
			Impl->Kit = crowdy::kit::makeKit(Client, std::move(AppId), nullptr, std::move(Options));
			return MakeShared<FCrowdyNativeKit>(Impl);
		}
		catch (const std::exception& Ex)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Creating the Game Kit threw: %hs"), Ex.what());
		}
		catch (...)
		{
			UE_LOG(LogCrowdyCpp, Warning, TEXT("Creating the Game Kit threw a non-standard exception"));
		}
		return nullptr;
	}

	bool ArgsFromMsgpack(const TArray<uint8>& Bytes, crowdy::graphql::JVal& Out)
	{
		if (Bytes.IsEmpty())
		{
			Out = crowdy::graphql::JVal(nullptr);
			return true;
		}
		FCrowdyNativeKitReader Reader{Bytes.GetData(), Bytes.Num()};
		return CrowdyNativeKitReadValue(Reader, Out, 0) && Reader.At == Reader.Num;
	}

	bool DecodeActorState(TConstArrayView<uint8> Bytes, FCrowdyNativeKitActorState& Out)
	{
		try
		{
			const std::optional<crowdy::kit::KitActorState> State = crowdy::kit::decodeKitActorState(
				std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())));
			// Any peer can send one, so a kind no starter would name and a place no world holds are refused here.
			constexpr size_t MaxKindBytes = 64;
			constexpr double MaxChunks = 1.0e9;
			const auto InWorld = [MaxChunks](const double Value) { return FMath::IsFinite(Value) && FMath::Abs(Value) <= MaxChunks; };
			if (!State || State->kind.size() > MaxKindBytes || !InWorld(State->x) || !InWorld(State->y) || !InWorld(State->z)
				|| (State->yaw && !FMath::IsFinite(*State->yaw)))
			{
				return false;
			}
			Out = FCrowdyNativeKitActorState();
			Out.Kind = CrowdyNativeKitFromUtf8(State->kind);
			Out.Id = State->id;
			Out.X = State->x;
			Out.Y = State->y;
			Out.Z = State->z;
			Out.Hp = State->hp ? TOptional<int64>(*State->hp) : TOptional<int64>();
			Out.MaxHp = State->maxHp ? TOptional<int64>(*State->maxHp) : TOptional<int64>();
			Out.Mode = State->mode ? TOptional<FString>(CrowdyNativeKitFromUtf8(*State->mode)) : TOptional<FString>();
			Out.Owner = State->owner ? TOptional<int64>(*State->owner) : TOptional<int64>();
			Out.Yaw = State->yaw ? TOptional<double>(*State->yaw) : TOptional<double>();
			Out.Name = State->name ? TOptional<FString>(CrowdyNativeKitFromUtf8(*State->name)) : TOptional<FString>();
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
}

FCrowdyNativeKit::FCrowdyNativeKit(TSharedRef<FImpl> InImpl)
	: Impl(MoveTemp(InImpl))
{
}

FCrowdyNativeKit::~FCrowdyNativeKit()
{
	Close();
}

const FString& FCrowdyNativeKit::CanceledMessage()
{
	static const FString Message = TEXT("the Game Kit is closed");
	return Message;
}

void FCrowdyNativeKit::Call(const FString& NodeType, const FString& Key, const FString& Method, const TArray<uint8>& Args,
	bool bOnce, int32 TimeoutMs, TFunction<void(const FCrowdyNativeKitReply&)> OnDone)
{
	if (Impl->bClosed)
	{
		OnDone(CrowdyNativeKitRefusal(ECrowdyNativeExecStatus::Unavailable, CanceledMessage()));
		return;
	}
	crowdy::graphql::JVal Value;
	if (!CrowdyNativeKit::ArgsFromMsgpack(Args, Value))
	{
		OnDone(CrowdyNativeKitRefusal(ECrowdyNativeExecStatus::BadRequest, TEXT("the call's arguments are not one MessagePack value the Game Kit can send")));
		return;
	}
	// The action id rides inside a map; without one, a resend could run the action twice.
	if (bOnce && !Value.isNull() && !Value.isObject())
	{
		OnDone(CrowdyNativeKitRefusal(ECrowdyNativeExecStatus::BadRequest, TEXT("a call sent once needs its arguments as a map, or none, to carry its action id")));
		return;
	}
	crowdy::kit::KitCallOptions Options;
	Options.action = bOnce;
	Options.timeoutMs = FMath::Max(0, TimeoutMs);
	const uint64 Id = Impl->TrackReply(MoveTemp(OnDone));
	Impl->Kit->runtime().call(CrowdyNativeKitToUtf8(NodeType), CrowdyNativeKitToUtf8(Key), CrowdyNativeKitToUtf8(Method),
		std::move(Value), Impl->ReplyTo(Id), Options);
}

uint64 FCrowdyNativeKit::Subscribe(const FString& NodeType, const FString& Key, const FString& Topic,
	TFunction<void(const FCrowdyNativeExecPush&)> OnPush, TFunction<void(const FCrowdyNativeKitReply&)> OnDone)
{
	if (Impl->bClosed)
	{
		OnDone(CrowdyNativeKitRefusal(ECrowdyNativeExecStatus::Unavailable, CanceledMessage()));
		return 0;
	}
	if (Impl->Subscriptions.Num() >= static_cast<int32>(crowdy::kit::kKitSubscriptionsMax))
	{
		FCrowdyNativeKitReply Refused = CrowdyNativeKitRefusal(ECrowdyNativeExecStatus::AppError, TEXT("the kit holds as many topics as one connection may"));
		Refused.Code = TEXT("limit");
		OnDone(Refused);
		return 0;
	}
	const uint64 Id = Impl->TrackReply(MoveTemp(OnDone));
	TWeakPtr<FImpl, ESPMode::ThreadSafe> Weak = Impl->AsWeak();
	TSharedRef<bool, ESPMode::ThreadSafe> bLive = MakeShared<bool, ESPMode::ThreadSafe>(true);
	crowdy::domains::ExecPushHandler Handler = [Weak, OnPush, bLive](const crowdy::domains::ExecPush& Push)
	{
		const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin();
		if (!Live || !*bLive || !OnPush)
		{
			return;
		}
		FCrowdyNativeExecPush Out;
		Out.NodeType = CrowdyNativeKitFromUtf8(Push.nodeType);
		Out.Key = CrowdyNativeKitFromUtf8(Push.key);
		Out.Topic = CrowdyNativeKitFromUtf8(Push.topic);
		Out.Payload = TArray<uint8>(reinterpret_cast<const uint8*>(Push.payload.data()), static_cast<int32>(Push.payload.size()));
		OnPush(Out);
	};
	// Straight on the connection rather than through the kit's pacer, so a raw topic is never held back by the kit's call budget.
	const crowdy::kit::KitCallback Done = Impl->ReplyTo(Id);
	TSharedRef<uint64, ESPMode::ThreadSafe> Subscribed = MakeShared<uint64, ESPMode::ThreadSafe>(0);
	const uint64 Handle = Impl->Connection->subscribe(CrowdyNativeKitToUtf8(NodeType), CrowdyNativeKitToUtf8(Key),
		CrowdyNativeKitToUtf8(Topic), std::move(Handler),
		[Weak, Done, bLive, Subscribed](crowdy::domains::ExecReply Reply)
		{
			// A refused subscription holds no topic on the connection, so it stops counting toward the kit's limit.
			const TSharedPtr<FImpl, ESPMode::ThreadSafe> Live = Weak.Pin();
			if (!Reply.ok() && Live)
			{
				*bLive = false;
				Live->Subscriptions.Remove(*Subscribed);
			}
			Done(crowdy::kit::KitReply::from(std::move(Reply)));
		});
	*Subscribed = Handle;
	if (Handle != 0 && *bLive)
	{
		Impl->Subscriptions.Add(Handle, bLive);
	}
	return Handle;
}

void FCrowdyNativeKit::Unsubscribe(uint64 Handle)
{
	TSharedRef<bool, ESPMode::ThreadSafe>* bLive = Impl->bClosed ? nullptr : Impl->Subscriptions.Find(Handle);
	if (!bLive)
	{
		return;
	}
	Impl->EndSubscription(Handle, *bLive);
	Impl->Subscriptions.Remove(Handle);
}

int32 FCrowdyNativeKit::NumSubscriptions() const
{
	return Impl->Subscriptions.Num();
}

void FCrowdyNativeKit::SetPlayerId(int64 PlayerId)
{
	if (!Impl->bClosed)
	{
		Impl->Kit->runtime().setPlayerId(PlayerId > 0 ? std::to_string(PlayerId) : std::string());
	}
}

bool FCrowdyNativeKit::IsClosed() const
{
	return Impl->bClosed;
}

void FCrowdyNativeKit::Close()
{
	if (Impl->bClosed)
	{
		return;
	}
	Impl->bClosed = true;

	// The connection outlives the kit, so its subscriptions are ended here rather than left delivering to nobody.
	for (const TPair<uint64, TSharedRef<bool, ESPMode::ThreadSafe>>& Subscription : Impl->Subscriptions)
	{
		Impl->EndSubscription(Subscription.Key, Subscription.Value);
	}
	Impl->Subscriptions.Reset();

	// Released too, so a call its pacer had already handed to the next Poll finds no kit to send it.
	Impl->Kit->close();
	Impl->Kit.reset();

	TMap<uint64, FImpl::FReplyCallback> Replies = MoveTemp(Impl->PendingReplies);
	Impl->PendingReplies.Reset();
	const FCrowdyNativeKitReply Canceled = CrowdyNativeKitRefusal(ECrowdyNativeExecStatus::Unavailable, CanceledMessage());
	for (TPair<uint64, FImpl::FReplyCallback>& Pending : Replies)
	{
		if (Pending.Value)
		{
			Pending.Value(Canceled);
		}
	}
}
