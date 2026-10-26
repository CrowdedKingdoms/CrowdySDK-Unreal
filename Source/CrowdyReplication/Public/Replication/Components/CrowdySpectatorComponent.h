#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Containers/UnrealString.h"
#include "Engine/TimerHandle.h"
#include "Math/IntVector.h"
#include "Templates/Function.h"
#include "CrowdySpectatorComponent.generated.h"

struct FActorUpdateRequestMessage;
enum class ECrowdyCppSendOutcome : uint8;

namespace CrowdySpectator
{
	constexpr float MinIntervalSeconds = 0.25f;
	constexpr int32 MaxDistance = 8;

	/** How often the owner's chunk is checked, which bounds how late a move into another chunk is noticed. */
	constexpr float WatchSeconds = 0.25f;

	CROWDYREPLICATION_API float ClampInterval(float Seconds);
	CROWDYREPLICATION_API int32 ClampDistance(int32 Distance);

	/** A beat counts as sent only when the connection accepted it, so a dropped one stays owed. */
	CROWDYREPLICATION_API bool CountsAsSent(ECrowdyCppSendOutcome Outcome);

	/**
	 * The id a viewer beats under: the session's id when it has one, else the one the local player's pawn would take
	 * from the account. Empty while the account is not known. 32 characters, the same text the pawn sends under.
	 */
	CROWDYREPLICATION_API FString ViewerUuid(const FString& SessionUuid, int64 UserId);

	/** One viewer beat as it goes out. */
	struct FBeat
	{
		FInt64Vector Chunk = FInt64Vector::ZeroValue;
		FString Uuid;
		int32 Distance = MaxDistance;
	};

	/** Fills Message as the actor update a viewer sends: the header for Beat and no state at all. */
	CROWDYREPLICATION_API void FillBeatMessage(const FBeat& Beat, int64 AppId, FActorUpdateRequestMessage& Message);

	/**
	 * The beat cadence with no world, clock or socket in it. Start and a move into another chunk beat at once; Pulse
	 * is the periodic beat. A beat that could not go out (no identity yet, no connection) is owed and goes at the
	 * next Watch.
	 */
	class CROWDYREPLICATION_API FCore
	{
	public:
		/** The id to beat under now, or empty when there is none or the viewer should stay quiet. */
		TFunction<FString()> ResolveUuid;

		/** Puts one beat on the wire. Answers false when it could not. */
		TFunction<bool(const FBeat&)> Send;

		int32 Distance = MaxDistance;

		bool IsRunning() const { return bRunning; }
		bool IsOwingBeat() const { return bOwesBeat; }

		void Start(const FInt64Vector& Chunk);
		void Stop();

		/** Beats at the last known chunk. */
		void Pulse();

		/** Beats at once, and answers true, when Chunk is another chunk or a beat is owed. */
		bool Watch(const FInt64Vector& Chunk);

	private:
		void Emit();

		FInt64Vector LastChunk = FInt64Vector::ZeroValue;
		bool bRunning = false;
		bool bOwesBeat = false;
	};
}

/**
 * Keeps this actor's position known to the realtime tier while its owner has no replicated avatar, so a match's
 * realtime events keep arriving. It sends the platform's viewer beat: an actor update with no state, now, every
 * Interval and at once when the owner moves into another chunk. Other clients draw nothing for it.
 */
UCLASS(ClassGroup = (Crowdy), meta = (BlueprintSpawnableComponent, DisplayName = "Crowdy Spectator"))
class CROWDYREPLICATION_API UCrowdySpectatorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCrowdySpectatorComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy|Game Kit|Spectate",
		meta = (ClampMin = "0.25", UIMin = "0.25", Units = "Seconds",
			ToolTip = "Seconds between beats, at least 0.25. Watching the match itself is Get Server Object on the session without Join; this only keeps its realtime events arriving."))
	float Interval = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy|Game Kit|Spectate",
		meta = (ClampMin = "0", ClampMax = "8", UIMin = "0", UIMax = "8",
			ToolTip = "How many chunks around the owner the beat reaches, 0 to 8. Watching the match itself is Get Server Object on the session without Join; this only keeps its realtime events arriving."))
	int32 Distance = CrowdySpectator::MaxDistance;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy|Game Kit|Spectate",
		meta = (ToolTip = "Start spectating when play begins. Watching the match itself is Get Server Object on the session without Join; this only keeps its realtime events arriving."))
	bool bAutoStart = true;

	/** Starts beating the owner's position. Does nothing without a world and a Crowdy session, and a beat that cannot go out yet is sent as soon as it can. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy|Game Kit|Spectate")
	void StartSpectating();

	/** Stops beating. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy|Game Kit|Spectate")
	void StopSpectating();

	UFUNCTION(BlueprintPure, Category = "Crowdy|Game Kit|Spectate")
	bool IsSpectating() const { return Core.IsRunning(); }

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_DEV_AUTOMATION_TESTS
	CrowdySpectator::FCore& GetCoreForTest() { return Core; }
#endif

private:
	void OnBeatTimer();
	void OnWatchTimer();
	void RestartBeatTimer();
	bool ChunkOfOwner(FInt64Vector& OutChunk);
	FString ResolveBeatUuid() const;
	bool SendBeat(const CrowdySpectator::FBeat& Beat) const;

	CrowdySpectator::FCore Core;
	FTimerHandle BeatTimer;
	FTimerHandle WatchTimer;
};
