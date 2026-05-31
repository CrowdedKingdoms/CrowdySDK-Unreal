#pragma once

#include "CoreMinimal.h"
#include "Shared/Types/Enums/Character/EActionType.h"
#include "Shared/Types/Enums/Character/ECharacterType.h"
#include "Shared/Types/Enums/Character/EGait.h"
#include "GameFramework/Character.h"
#include "Shared/Types/Enums/Character/EAttachmentType.h"
#include "FActorState.generated.h"

USTRUCT(BlueprintType)
struct FActorState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="ActorState")
	uint8 Version = 2;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ActorState")
	FVector Position;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ActorState")
	FRotator Rotation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ActorState")
	FVector Velocity;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ActorState")
	bool bCrouch;

	UPROPERTY(BlueprintReadWrite, Category="ActorState")
	EAttachmentType Attachment;

	
	FActorState()
		: Version(1),
		  Position(FVector::ZeroVector),
		  Rotation(FRotator(0, 0, 0)),
		  Velocity(FVector::ZeroVector),
		  bCrouch(false), Attachment(EAttachmentType::None)
	{
	}

	static TArray<uint8> SerializeActorState(const FActorState& ActorState)
	{
        TArray<uint8> ByteArray;
        ByteArray.SetNumUninitialized(sizeof(ActorState));
        FMemory::Memcpy(ByteArray.GetData(), &ActorState, sizeof(ActorState));
        return ByteArray;
    }

	static FActorState DeserializeActorState(const TArray<uint8>& Payload, const int32 Offset)
	{
        FActorState ActorState;
        if(Payload.Num() >= Offset + sizeof(FActorState))
        {
            FMemory::Memcpy(&ActorState, Payload.GetData() + Offset, sizeof(FActorState));
        }
        
        return ActorState;
    }
    
   
    
};
