#include "Subsystem/CrowdyKitEmotes.h"

#include "CrowdyReplicationLog.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/CrowdyKitEvents.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Subscription/FCrowdyDelivery.h"
#include "Core/UDP/Subscription/FCrowdySubscriptionOptions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Kismet/GameplayStatics.h"
#include "Messages/GameObjects/FKitEventRequest.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/HelperFunctions.h"
#include "Utils/SerializationFunctionLibrary.h"

namespace
{
	TAutoConsoleVariable<float> CVarCrowdyKitEmotesMaxPerSecond(
		TEXT("crowdy.kit.emotes.maxpersecond"),
		64.0f,
		TEXT("How many inbound emotes per second are read, shared by every sender (default 64). Emotes over the budget are dropped before they are parsed."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarCrowdyKitEmotesBurst(
		TEXT("crowdy.kit.emotes.burst"),
		16,
		TEXT("How many inbound emotes may arrive at once before the per-second budget applies (default 16)."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarCrowdyKitEmotesSenderMaxPerSecond(
		TEXT("crowdy.kit.emotes.persender.maxpersecond"),
		4.0f,
		TEXT("How many inbound emotes per second are read from one sender (default 4). The shared budget still caps all senders together."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarCrowdyKitEmotesSenderBurst(
		TEXT("crowdy.kit.emotes.persender.burst"),
		4,
		TEXT("How many inbound emotes one sender may send at once before its per-second budget applies (default 4)."),
		ECVF_Default);

	void AppendKitEmoteAscii(TArray<uint8>& Out, const ANSICHAR* Text)
	{
		for (const ANSICHAR* Cursor = Text; *Cursor; ++Cursor)
		{
			Out.Add(static_cast<uint8>(*Cursor));
		}
	}

	// Escapes the way the kit's own writer does, so the same text gives the same octets on every client library.
	void AppendKitEmoteJsonString(TArray<uint8>& Out, const FString& Text)
	{
		static constexpr ANSICHAR HexDigits[] = "0123456789abcdef";

		const FTCHARToUTF8 Utf8(*Text);
		const uint8* Octets = reinterpret_cast<const uint8*>(Utf8.Get());

		Out.Add('"');
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			const uint8 Octet = Octets[Index];
			switch (Octet)
			{
			case '"':
				AppendKitEmoteAscii(Out, "\\\"");
				break;
			case '\\':
				AppendKitEmoteAscii(Out, "\\\\");
				break;
			case '\b':
				AppendKitEmoteAscii(Out, "\\b");
				break;
			case '\f':
				AppendKitEmoteAscii(Out, "\\f");
				break;
			case '\n':
				AppendKitEmoteAscii(Out, "\\n");
				break;
			case '\r':
				AppendKitEmoteAscii(Out, "\\r");
				break;
			case '\t':
				AppendKitEmoteAscii(Out, "\\t");
				break;
			default:
				if (Octet < 0x20)
				{
					AppendKitEmoteAscii(Out, "\\u00");
					Out.Add(static_cast<uint8>(HexDigits[Octet >> 4]));
					Out.Add(static_cast<uint8>(HexDigits[Octet & 0xF]));
				}
				else
				{
					Out.Add(Octet);
				}
				break;
			}
		}
		Out.Add('"');
	}
}

bool UCrowdyKitEmotes::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

void UCrowdyKitEmotes::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance)
	{
		return;
	}

	const UCrowdySDKBridgeSubsystem* Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (!Bridge || !Bridge->ServiceRegistry)
	{
		UE_LOG(LogCrowdyReplication, Verbose, TEXT("[CrowdyKitEmotes] No service registry yet, so emotes will not be received."));
		return;
	}

	// Handled, so an emote does not also reach the event fallbacks that would fire an empty Blueprint event.
	EmoteSubscription = Bridge->ServiceRegistry->SubscribeToEventPayload(
		static_cast<FCrowdyTypeID>(CrowdyKitEvent::Emote),
		{ ECrowdySubscriptionRole::Handle, false, TEXT("CrowdyKitEmotes") },
		[this](const FCrowdyDelivery& Delivery) { HandleDelivery(Delivery); });
}

void UCrowdyKitEmotes::Deinitialize()
{
	EmoteSubscription.Release();
	Super::Deinitialize();
}

UCrowdyKitEmotes* UCrowdyKitEmotes::GetKitEmotes(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UCrowdyKitEmotes>() : nullptr;
}

bool UCrowdyKitEmotes::SendEmoteInWorld(const UObject* WorldContextObject, const FString& Emote, AActor* Target, FString& Reason)
{
	UCrowdyKitEmotes* Emotes = GetKitEmotes(WorldContextObject);
	if (!Emotes)
	{
		Reason = TEXT("Emotes are only available in a running game world.");
		return false;
	}

	return Emotes->SendEmote(Emote, Target, Reason);
}

bool UCrowdyKitEmotes::SendEmote(const FString& Emote, AActor* Target, FString& Reason)
{
	Reason.Reset();

	FString TargetUuid;
	TArray<uint8> Body;

	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance)
	{
		Reason = TEXT("There is no game instance to send from.");
		return false;
	}

	if (!ResolveTargetUuid(Target, TargetUuid, Reason) || !EncodeEmoteBody(Emote, TargetUuid, Body, Reason))
	{
		return false;
	}

	const UCrowdyGameSession* Session = GameInstance->GetSubsystem<UCrowdyGameSession>();
	FCrowdyActorId Sender;
	if (!Session || !FCrowdyActorId::TryFromString(Session->GetUUID(), Sender))
	{
		Reason = TEXT("The player is not signed in, so there is no actor to send the emote from.");
		return false;
	}

	UCrowdyCppReplicationSubsystem* Replication = GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>();
	if (!Replication || !Replication->IsRouting())
	{
		Reason = TEXT("The game is not connected to the realtime server.");
		return false;
	}

	const APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, 0);
	if (!Pawn)
	{
		Reason = TEXT("There is no local player pawn to send the emote from.");
		return false;
	}

	const FVector Location = Pawn->GetActorLocation();
	if (!(FMath::Abs(Location.X) < CrowdyMaxAddressableWorldCoordinate
		&& FMath::Abs(Location.Y) < CrowdyMaxAddressableWorldCoordinate
		&& FMath::Abs(Location.Z) < CrowdyMaxAddressableWorldCoordinate))
	{
		Reason = TEXT("The local player pawn is too far from the origin to address a chunk.");
		return false;
	}

	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;
	UHelperFunctions::GetChunkCoordinateAtLocation(this, Location, ChunkX, ChunkY, ChunkZ);

	FKitEventRequest Request;
	FillEmoteRequest(Request, Session->GetAppID(), Sender, ChunkX, ChunkY, ChunkZ, MoveTemp(Body));

	switch (Replication->TrySendMessage(Request))
	{
	case ECrowdyCppSendOutcome::Sent:
		return true;
	case ECrowdyCppSendOutcome::NotRouted:
		Reason = TEXT("The game is not connected to the realtime server.");
		return false;
	default:
		Reason = TEXT("The realtime connection refused the emote.");
		return false;
	}
}

bool UCrowdyKitEmotes::ResolveTargetUuid(AActor* Target, FString& OutTargetUuid, FString& Reason) const
{
	OutTargetUuid.Reset();
	if (!Target)
	{
		return true;
	}

	const UWorld* World = GetWorld();
	const UCrowdyEntitySubsystem* Entities = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
	const FGuid TargetId = Entities ? Entities->FindEntityID(Target) : FGuid();
	if (!TargetId.IsValid())
	{
		Reason = TEXT("The target is not a replicated actor, so it has no id to send.");
		return false;
	}

	// The local player goes as the id text it signed in with, every other actor as the text its updates arrived under.
	const UCrowdyGameSession* Session = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UCrowdyGameSession>() : nullptr;
	if (Session && TargetId == Entities->GetLocalPlayerID())
	{
		OutTargetUuid = Session->GetUUID();
		return true;
	}

	const UCrowdyActorTracker* Tracker = World->GetSubsystem<UCrowdyActorTracker>();
	FCrowdyActorId WireId;
	if (!Tracker || !Tracker->FindWireId(TargetId, WireId))
	{
		Reason = TEXT("The target has not been heard from yet, so its id is not known.");
		return false;
	}

	OutTargetUuid = WireId.ToString();
	return true;
}

bool UCrowdyKitEmotes::EncodeEmoteBody(const FString& Emote, const FString& TargetUuid, TArray<uint8>& OutBody, FString& Reason)
{
	OutBody.Reset();

	if (Emote.IsEmpty())
	{
		Reason = TEXT("The emote is empty.");
		return false;
	}

	// Every character is at least one octet, so a longer string cannot fit and need not be converted to find out.
	if (Emote.Len() > CrowdyKitEvent::MaxEmoteStateBytes)
	{
		Reason = FString::Printf(TEXT("The emote and its target do not fit in %d bytes."), CrowdyKitEvent::MaxEmoteStateBytes);
		return false;
	}

	OutBody.Reserve(Emote.Len() + TargetUuid.Len() + 32);
	AppendKitEmoteAscii(OutBody, "{\"emote\":");
	AppendKitEmoteJsonString(OutBody, Emote);
	AppendKitEmoteAscii(OutBody, ",\"targetUuid\":");
	if (TargetUuid.IsEmpty())
	{
		AppendKitEmoteAscii(OutBody, "null");
	}
	else
	{
		AppendKitEmoteJsonString(OutBody, TargetUuid);
	}
	AppendKitEmoteAscii(OutBody, "}");

	if (OutBody.Num() > CrowdyKitEvent::MaxEmoteStateBytes)
	{
		Reason = FString::Printf(TEXT("The emote and its target do not fit in %d bytes: the message is %d."),
			CrowdyKitEvent::MaxEmoteStateBytes, OutBody.Num());
		OutBody.Reset();
		return false;
	}

	return true;
}

bool UCrowdyKitEmotes::DecodeEmoteBody(const TConstArrayView<uint8> Body, FString& OutEmote, FString& OutTargetUuid)
{
	OutEmote.Reset();
	OutTargetUuid.Reset();

	if (Body.IsEmpty() || Body.Num() > CrowdyKitEvent::MaxEmoteStateBytes)
	{
		return false;
	}

	const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
	const FString Json(Text.Length(), Text.Get());
	if (!CrowdyJsonSafety::IsNestingWithinLimit(Json))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonValue>* EmoteField = Object->Values.Find(TEXT("emote"));
	if (!EmoteField || !EmoteField->IsValid() || (*EmoteField)->Type != EJson::String)
	{
		return false;
	}
	OutEmote = (*EmoteField)->AsString();

	const TSharedPtr<FJsonValue>* TargetField = Object->Values.Find(TEXT("targetUuid"));
	if (TargetField && TargetField->IsValid() && (*TargetField)->Type == EJson::String)
	{
		OutTargetUuid = (*TargetField)->AsString();
	}

	return true;
}

void UCrowdyKitEmotes::FillEmoteRequest(FKitEventRequest& Request, const int64 AppId, const FCrowdyActorId& Sender,
	const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ, TArray<uint8>&& Body)
{
	Request.AppID = AppId;
	Request.UUID = Sender;
	Request.ChunkX = ChunkX;
	Request.ChunkY = ChunkY;
	Request.ChunkZ = ChunkZ;
	Request.ReplicationDistance = ECrowdyReplicationDistance::Two_Chunks;
	Request.DecayRate = ECrowdyDecayRate::No_Decay;
	Request.EventType = CrowdyKitEvent::Emote;
	Request.State = MoveTemp(Body);
}

void UCrowdyKitEmotes::HandleDelivery(const FCrowdyDelivery& Delivery)
{
	if (!OnEmote.IsBound() || Delivery.Opcode != ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION)
	{
		return;
	}

	// The sender id is self-claimed, so a forger can rotate it: the shared budget stays as the cap over every sender.
	const FCrowdyActorId& Sender = Delivery.HeaderActorId();
	const double Now = FPlatformTime::Seconds();
	if (!TryAdmit(Sender, Now))
	{
		return;
	}

	ReceiveEmote(Sender, Delivery.PayloadBytes);
}

bool UCrowdyKitEmotes::TryAdmit(const FCrowdyActorId& Sender, const double Now)
{
	if (!TryAdmitSender(Sender, Now))
	{
		return false;
	}

	if (TryAdmitEmote(Now))
	{
		return true;
	}

	if (FEmoteSenderBucket* Bucket = SenderBuckets.Find(Sender))
	{
		Bucket->Tokens += 1.0;
	}
	return false;
}

void UCrowdyKitEmotes::EvictStalestSender()
{
	FCrowdyActorId Stalest;
	double StalestAt = TNumericLimits<double>::Max();
	for (const TPair<FCrowdyActorId, FEmoteSenderBucket>& Pair : SenderBuckets)
	{
		if (Pair.Value.At < StalestAt)
		{
			StalestAt = Pair.Value.At;
			Stalest = Pair.Key;
		}
	}
	SenderBuckets.Remove(Stalest);
}

bool UCrowdyKitEmotes::TryAdmitSender(const FCrowdyActorId& Sender, const double Now)
{
	const double Rate = FMath::Max(0.0, static_cast<double>(CVarCrowdyKitEmotesSenderMaxPerSecond.GetValueOnAnyThread()));
	const double Burst = FMath::Max(1, CVarCrowdyKitEmotesSenderBurst.GetValueOnAnyThread());

	FEmoteSenderBucket* Bucket = SenderBuckets.Find(Sender);
	if (Bucket)
	{
		Bucket->Tokens = FMath::Min(Burst, Bucket->Tokens + FMath::Max(0.0, Now - Bucket->At) * Rate);
		Bucket->At = Now;
	}
	else
	{
		if (SenderBuckets.Num() >= MaxEmoteSenders)
		{
			EvictStalestSender();
		}
		Bucket = &SenderBuckets.Add(Sender, FEmoteSenderBucket{ Burst, Now });
	}

	if (Bucket->Tokens >= 1.0)
	{
		Bucket->Tokens -= 1.0;
		return true;
	}

	++EmotesDropped;
	if (!bReportedSenderFlood)
	{
		bReportedSenderFlood = true;
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyKitEmotes] Dropping inbound emotes from a sender over its own budget of %.0f per second (burst %.0f). Raise crowdy.kit.emotes.persender.maxpersecond or crowdy.kit.emotes.persender.burst if this is expected traffic. Reported once; later drops are only counted."),
			Rate, Burst);
	}
	return false;
}

bool UCrowdyKitEmotes::TryAdmitEmote(const double Now)
{
	const double Rate = FMath::Max(0.0, static_cast<double>(CVarCrowdyKitEmotesMaxPerSecond.GetValueOnAnyThread()));
	const double Burst = FMath::Max(1, CVarCrowdyKitEmotesBurst.GetValueOnAnyThread());

	EmoteTokens = FMath::Min(Burst, EmoteTokens + FMath::Max(0.0, Now - EmoteTokensAt) * Rate);
	EmoteTokensAt = Now;

	if (EmoteTokens >= 1.0)
	{
		EmoteTokens -= 1.0;
		return true;
	}

	++EmotesDropped;
	if (!bReportedEmoteFlood)
	{
		bReportedEmoteFlood = true;
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyKitEmotes] Dropping inbound emotes over the budget of %.0f per second (burst %.0f). Raise crowdy.kit.emotes.maxpersecond or crowdy.kit.emotes.burst if this is expected traffic. Reported once; later drops are only counted."),
			Rate, Burst);
	}
	return false;
}

void UCrowdyKitEmotes::ReceiveEmote(const FCrowdyActorId& Sender, const TConstArrayView<uint8> Body)
{
	FString Emote;
	FString TargetUuid;
	if (!DecodeEmoteBody(Body, Emote, TargetUuid))
	{
		UE_LOG(LogCrowdyReplication, Verbose, TEXT("[CrowdyKitEmotes] Dropped an emote of %d bytes: it is not a JSON object with a text emote."), Body.Num());
		return;
	}

	const UWorld* World = GetWorld();
	const UCrowdyEntitySubsystem* Entities = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;

	AActor* SenderActor = Entities ? Entities->FindEntity(USerializationFunctionLibrary::ToGuid(Sender)) : nullptr;
	AActor* TargetActor = (Entities && !TargetUuid.IsEmpty()) ? Entities->FindEntity(USerializationFunctionLibrary::ToGuid(TargetUuid)) : nullptr;

	OnEmote.Broadcast(SenderActor, Sender.ToString(), Emote, TargetActor, TargetUuid);
}
