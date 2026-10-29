#include "Messages/Actor/FCrowdyKitActorState.h"

#include "CrowdyNativeKit.h"

FCrowdyKitActorState FCrowdyKitActorState::FromNative(const FCrowdyNativeKitActorState& Native)
{
	FCrowdyKitActorState State;
	// Found rather than added: the kinds a project draws are already names, from its starters' Kit Actors, and any peer chooses the rest.
	State.Kind = FName(*Native.Kind, FNAME_Find);
	State.Id = Native.Id;
	State.ChunkPosition = FVector(Native.X, Native.Y, Native.Z);
	State.Location = State.ChunkPosition * ChunkWorldSize;
	State.Rotation = FRotator(0.0, Native.Yaw.Get(0.0), 0.0);
	State.bHasHp = Native.Hp.IsSet();
	State.Hp = Native.Hp.Get(0);
	State.MaxHp = Native.MaxHp.Get(0);
	State.Mode = Native.Mode.Get(FString());
	State.Owner = Native.Owner.Get(0);
	State.Name = Native.Name.Get(FString());
	return State;
}

FString FCrowdyKitActorState::ReadInstanceId(const TConstArrayView<uint8> ActorIdOctets)
{
	if (ActorIdOctets.Num() < 3 || ActorIdOctets[0] < 'A' || ActorIdOctets[0] > 'Z')
	{
		return FString();
	}
	for (int32 End = 1; End < ActorIdOctets.Num(); ++End)
	{
		const uint8 Octet = ActorIdOctets[End];
		if (Octet == '@')
		{
			return End > 1 ? FString::ConstructFromPtrSize(reinterpret_cast<const ANSICHAR*>(&ActorIdOctets[1]), End - 1) : FString();
		}
		if (Octet < 0x21 || Octet > 0x7E)
		{
			return FString();
		}
	}
	return FString();
}

bool FCrowdyKitActorState::Decode(TConstArrayView<uint8> Bytes, FCrowdyKitActorState& Out)
{
	FCrowdyNativeKitActorState Native;
	if (!CrowdyNativeKit::DecodeActorState(Bytes, Native))
	{
		return false;
	}
	Out = FromNative(Native);
	return true;
}
