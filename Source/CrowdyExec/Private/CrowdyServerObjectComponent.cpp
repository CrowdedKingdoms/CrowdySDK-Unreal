#include "CrowdyServerObjectComponent.h"

#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectShared.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "TimerManager.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyTeams.h"
#include "UObject/ObjectKey.h"
#include "UObject/UnrealType.h"

namespace CrowdyServerObjectComponentDetail
{
	using CrowdyServerObjectShared::WaitingForSignIn;
	using CrowdyServerObjectShared::WaitingForTeams;

	const TCHAR* const PickDefinition = TEXT("Pick a Server Object definition on the Crowdy Server Object component");
	const TCHAR* const SetSourceVariable = TEXT("Set a Source Variable on the Crowdy Server Object component");

	template <typename TSubsystem>
	TSubsystem* FindGameInstanceSubsystem(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<TSubsystem>() : nullptr;
	}

	int64 FindSignedInUserId(const UWorld* World)
	{
		const UCrowdyGameSession* Session = FindGameInstanceSubsystem<UCrowdyGameSession>(World);
		return Session ? Session->GetUserID() : 0;
	}

	/** True the first time an actor class has a component with no definition, so a crowd of them warns once. */
	bool FirstMissingDefinition(const AActor* Owner)
	{
		static TSet<TObjectKey<UClass>> Warned;
		bool bAlready = false;
		Warned.Add(TObjectKey<UClass>(Owner ? Owner->GetClass() : nullptr), &bAlready);
		return !bAlready;
	}

	const FDoubleProperty* FindPositionAxis(const UScriptStruct* Struct, FName Axis)
	{
		return CastField<FDoubleProperty>(CrowdyExec::FindTravellingField(Struct, Axis));
	}

	FString WaitingForVariable(FName Variable)
	{
		return FString::Printf(TEXT("Waiting for %s"), *Variable.ToString());
	}

	void RequestMyTeams(const UWorld* World)
	{
		CrowdyServerObjectShared::RequestMyTeams(FindGameInstanceSubsystem<UCrowdyTeams>(World));
	}

	/** Values' Name as an Instance Id: a String or Name as is, an integer in decimal, empty for 0. False for any other type. */
	bool ReadInstanceIdValue(const FInstancedStruct& Values, FName Name, FString& OutValue)
	{
		const FProperty* Field = UCrowdyServerObjectLibrary::FindServerValue(Values, Name);
		const uint8* Memory = Values.GetMemory();
		if (!Field || !Memory)
		{
			return false;
		}
		const void* Value = Field->ContainerPtrToValuePtr<void>(Memory);
		if (const FStrProperty* String = CastField<FStrProperty>(Field))
		{
			OutValue = String->GetPropertyValue(Value);
			return true;
		}
		if (const FNameProperty* NameField = CastField<FNameProperty>(Field))
		{
			const FName Read = NameField->GetPropertyValue(Value);
			OutValue = Read.IsNone() ? FString() : Read.ToString();
			return true;
		}
		const FNumericProperty* Numeric = CastField<FNumericProperty>(Field);
		if (!Numeric || !Numeric->IsInteger() || Numeric->IsEnum())
		{
			return false;
		}
		OutValue = Numeric->IsA<FUInt64Property>() ? LexToString(Numeric->GetUnsignedIntPropertyValue(Value)) : LexToString(Numeric->GetSignedIntPropertyValue(Value));
		if (OutValue.Equals(TEXT("0"), ESearchCase::CaseSensitive))
		{
			OutValue.Reset();
		}
		return true;
	}
}

UCrowdyServerObjectComponent::UCrowdyServerObjectComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UCrowdyServerObjectComponent::TickTeamsRetry(float DeltaSeconds)
{
	if (Object || !JoinError.Equals(CrowdyServerObjectComponentDetail::WaitingForTeams, ESearchCase::CaseSensitive))
	{
		SetTeamsRetryArmed(false);
		return;
	}
	TeamsRetryIn -= DeltaSeconds;
	if (TeamsRetryIn <= 0.f)
	{
		RequestTeams();
	}
}

void UCrowdyServerObjectComponent::SetTeamsRetryArmed(bool bArmed)
{
	bTeamsRetryArmed = bArmed;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FTimerManager& Timers = World->GetTimerManager();
	if (!bArmed)
	{
		Timers.ClearTimer(TeamsRetryTimer);
		return;
	}
	if (!Timers.IsTimerActive(TeamsRetryTimer))
	{
		Timers.SetTimer(TeamsRetryTimer, FTimerDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::TickTeamsRetry, TeamsRetrySeconds), TeamsRetrySeconds, true);
	}
}

void UCrowdyServerObjectComponent::RequestTeams()
{
	TeamsRetryIn = TeamsRetrySeconds;
	++TeamsRequests;
	CrowdyServerObjectComponentDetail::RequestMyTeams(GetWorld());
}

void UCrowdyServerObjectComponent::OnRegister()
{
	Super::OnRegister();

	// A cooked actor drops its placement guid once all its components are registered, so only a component registered with it reads one, in every build.
	const AActor* Owner = GetOwner();
	if (!PlacementGuid.IsValid() && Owner && !Owner->HasActorRegisteredAllComponents())
	{
		PlacementGuid = UCrowdyEntityComponent::ResolveActorInstanceGuid(Owner);
	}
}

FString UCrowdyServerObjectComponent::ResolveInstanceId(FString& OutError) const
{
	OutError.Reset();
	if (Definition && Definition->HasOnlyOneInstance())
	{
		return Definition->GetOnlyInstanceId();
	}
	switch (InstanceMode)
	{
	case ECrowdyServerObjectInstanceMode::InstanceId:
		if (!FollowsSignedInPlayer())
		{
			if (InstanceId.IsEmpty())
			{
				OutError = TEXT("Set an Instance Id on the Crowdy Server Object component");
			}
			return InstanceId;
		}
		[[fallthrough]];
	case ECrowdyServerObjectInstanceMode::SignedInPlayer:
	{
		const int64 UserId = CrowdyServerObjectComponentDetail::FindSignedInUserId(GetWorld());
		if (UserId <= 0)
		{
			OutError = TEXT("Nobody is signed in yet");
			return FString();
		}
		return LexToString(UserId);
	}
	case ECrowdyServerObjectInstanceMode::ThisActor:
	{
		const AActor* Owner = GetOwner();
		if (!Owner || !Owner->IsNetStartupActor() || !PlacementGuid.IsValid())
		{
			OutError = TEXT("This Actor needs an actor placed in the level with the component on it; otherwise give it an Instance Id");
			return FString();
		}
		return PlacementGuid.ToString(EGuidFormats::Digits);
	}
	case ECrowdyServerObjectInstanceMode::PlayersTeam:
	{
		const UCrowdyTeams* Teams = TeamId != 0 ? nullptr : CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyTeams>(GetWorld());
		return CrowdyServerObjectShared::ResolvePlayersTeam(Teams, TeamId, OutError);
	}
	case ECrowdyServerObjectInstanceMode::FromServerValue:
	{
		if (SourceVariable.IsNone())
		{
			OutError = CrowdyServerObjectComponentDetail::SetSourceVariable;
			return FString();
		}
		if (SourceValue.IsEmpty())
		{
			OutError = CrowdyServerObjectComponentDetail::WaitingForVariable(SourceVariable);
		}
		return SourceValue;
	}
	case ECrowdyServerObjectInstanceMode::ThisActorsArea:
		return ResolveAreaKey(OutError);
	}
	OutError = TEXT("Unknown Instance Mode on the Crowdy Server Object component");
	return FString();
}

FString UCrowdyServerObjectComponent::ResolveAreaKey(FString& OutError) const
{
	const AActor* Owner = GetOwner();
	const UCrowdyServerObjectSubsystem* ServerObjects = FindServerObjects();
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	const bool bPlaced = Owner && ServerObjects && CrowdyServerObjectShared::GroundChunkAt(Owner->GetActorLocation(), ServerObjects->GetChunkSize(), ChunkX, ChunkY);
	if (!Definition || !bPlaced)
	{
		OutError = TEXT("This Actor's Area needs the component on an actor in a game, at a location in the world");
		return FString();
	}
	return Definition->GetAreaKey(ChunkX, ChunkY);
}

bool UCrowdyServerObjectComponent::ReadsRecord() const
{
	return Definition && !Definition->IsFunctionLibrary() && Definition->Holds == ECrowdyServerObjectHolds::RecordPerEntity;
}

FInstancedStruct UCrowdyServerObjectComponent::GetVariables() const
{
	const UCrowdyServerObject* Read = GetReadObject();
	if (!Read)
	{
		return FInstancedStruct();
	}
	const FInstancedStruct* Values = Read->FindValues(ReadsRecord() ? &RecordId : nullptr);
	return Values ? *Values : Read->GetState();
}

UCrowdyServerObject* UCrowdyServerObjectComponent::GetReadObject() const
{
	const bool bHandingOver = Previous && Object && !HoldsOwnRecord(Object) && HoldsOwnRecord(Previous);
	return bHandingOver ? Previous.Get() : Object.Get();
}

bool UCrowdyServerObjectComponent::HoldsOwnRecord(const UCrowdyServerObject* Held) const
{
	return Held && Held->GetStatus() == ECrowdyServerObjectStatus::Ready && Held->GetRecord(RecordId);
}

bool UCrowdyServerObjectComponent::NeedsFollow() const
{
	const bool bArea = InstanceMode == ECrowdyServerObjectInstanceMode::ThisActorsArea && !(Definition && Definition->HasOnlyOneInstance());
	return bArea || (ReadsRecord() && !RecordNetId.IsValid());
}

bool UCrowdyServerObjectComponent::MovedChunk()
{
	const AActor* Owner = GetOwner();
	const UCrowdyServerObjectSubsystem* ServerObjects = FindServerObjects();
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	if (!Owner || !ServerObjects || !CrowdyServerObjectShared::GroundChunkAt(Owner->GetActorLocation(), ServerObjects->GetChunkSize(), ChunkX, ChunkY))
	{
		return false;
	}
	const bool bMoved = !bFollowedChunk || ChunkX != FollowedChunkX || ChunkY != FollowedChunkY;
	FollowedChunkX = ChunkX;
	FollowedChunkY = ChunkY;
	bFollowedChunk = true;
	return bMoved;
}

void UCrowdyServerObjectComponent::SetFollowArmed(bool bArmed)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FTimerManager& Timers = World->GetTimerManager();
	if (!bArmed)
	{
		Timers.ClearTimer(FollowTimer);
		return;
	}
	if (!Timers.IsTimerActive(FollowTimer))
	{
		Timers.SetTimer(FollowTimer, FTimerDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::TickFollow), FollowSeconds, true);
	}
}

void UCrowdyServerObjectComponent::TickFollow()
{
	if (bJoining || !Object)
	{
		return;
	}
	if (Previous && (HoldsOwnRecord(Object) || Previous->GetStatus() != ECrowdyServerObjectStatus::Ready))
	{
		Previous = nullptr;
	}
	// The key is worked out only when the actor reached another chunk.
	const bool bArea = InstanceMode == ECrowdyServerObjectInstanceMode::ThisActorsArea && !(Definition && Definition->HasOnlyOneInstance());
	if (bArea && MovedChunk())
	{
		FString Error;
		const FString Key = ResolveAreaKey(Error);
		if (!Key.IsEmpty() && !Key.Equals(Object->GetInstanceId(), ESearchCase::CaseSensitive))
		{
			// The area left keeps answering Get Variables until the new one holds this actor's record.
			Previous = HoldsOwnRecord(Object) ? Object.Get() : Previous.Get();
			Join();
			return;
		}
	}
	// The record's first values run Blueprint code, which must not start a Join inside the watch.
	TGuardValue<bool> JoiningGuard(bJoining, true);
	FollowRecordId();
	if (!NeedsFollow())
	{
		SetFollowArmed(false);
	}
}

void UCrowdyServerObjectComponent::FollowRecordId()
{
	UCrowdyServerObject* Held = Object;
	const UCrowdyEntityComponent* Found = Entity.Get();
	const FGuid NetId = Found ? Found->GetNetID() : FGuid();
	if (!Held || !ReadsRecord() || NetId == RecordNetId)
	{
		return;
	}
	if (RecordHandle.IsValid())
	{
		Held->UnwatchRecord(RecordId, RecordHandle);
		RecordHandle.Reset();
	}
	RecordNetId = NetId;
	RecordId = NetId.IsValid() ? NetId.ToString(EGuidFormats::Digits) : FString();
	VariableBindings.ReadRecord(true, RecordId);
	if (RecordId.IsEmpty())
	{
		return;
	}
	const FString Watched = RecordId;
	const FDelegateHandle Watch = Held->WatchRecord(Watched, FOnCrowdyServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::HandleVariables));
	if (Object != Held || !RecordId.Equals(Watched, ESearchCase::CaseSensitive))
	{
		// A handler of the first values let go of it, or moved to another record.
		Held->UnwatchRecord(Watched, Watch);
		return;
	}
	RecordHandle = Watch;
}

double UCrowdyServerObjectComponent::PositionNow() const
{
	const UWorld* World = GetWorld();
#if WITH_DEV_AUTOMATION_TESTS
	if (!World)
	{
		return PositionTimeForTest;
	}
#endif
	return World ? World->GetRealTimeSeconds() : 0.0;
}

bool UCrowdyServerObjectComponent::ResolvePositionAxes()
{
	PositionInX = nullptr;
	PositionInY = nullptr;
	PositionReplyX = nullptr;
	PositionReplyY = nullptr;
	PositionReplyStruct = nullptr;
	PositionInputs.Reset();
	const UCrowdyServerObjectDefinition* Held = Object ? Object->GetDefinition() : nullptr;
	if (!Held || !Held->ReportsPosition())
	{
		return false;
	}
	const FCrowdyServerFunction* Function = Held->FindFunction(Held->KitPositionFunction);
	const UScriptStruct* Inputs = Function ? Function->GetParamsStruct() : nullptr;
	PositionInX = CrowdyServerObjectComponentDetail::FindPositionAxis(Inputs, TEXT("X"));
	PositionInY = CrowdyServerObjectComponentDetail::FindPositionAxis(Inputs, TEXT("Y"));
	if (!PositionInX || !PositionInY)
	{
		PositionInX = nullptr;
		PositionInY = nullptr;
		return false;
	}
	PositionFunction = Held->KitPositionFunction;
	PositionPeriod = 1.f / FMath::Clamp(Held->KitPositionRate, 1.f, 10.f);
	PositionGeneration = CrowdyExec::GetStructGeneration();
	Function->InitializeParams(PositionInputs);
	PositionReplyStruct = Function->GetReplyStruct();
	PositionReplyX = CrowdyServerObjectComponentDetail::FindPositionAxis(PositionReplyStruct, TEXT("X"));
	PositionReplyY = CrowdyServerObjectComponentDetail::FindPositionAxis(PositionReplyStruct, TEXT("Y"));
	return PositionInputs.IsValid();
}

void UCrowdyServerObjectComponent::SetPositionArmed(bool bArmed)
{
	if (!bArmed)
	{
		ReleasePositionClaim();
		bPositionInFlight = false;
		bPositionSent = false;
		bPositionHoldCoded = false;
		PositionHoldUntil = 0.0;
		++PositionSerial;
		PositionInX = nullptr;
		PositionInY = nullptr;
		PositionInputs.Reset();
		FTSTicker::RemoveTicker(PositionTicker);
		PositionTicker.Reset();
		return;
	}
	if (!ResolvePositionAxes())
	{
		return;
	}
	PositionPawn = Cast<APawn>(GetOwner());
	bool bCanTick = GetWorld() != nullptr;
#if WITH_DEV_AUTOMATION_TESTS
	bCanTick |= bPositionTickerForTest;
#endif
	if (!bCanTick || PositionTicker.IsValid())
	{
		return;
	}
	PositionTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::TickPosition), PositionPeriod);
}

bool UCrowdyServerObjectComponent::TickPosition(float DeltaSeconds)
{
	ReportPosition();
	return true;
}

bool UCrowdyServerObjectComponent::ShouldReportOwner() const
{
	if (PositionReport != ECrowdyPositionReport::PlayerPawn)
	{
		return PositionReport == ECrowdyPositionReport::ThisActor;
	}
	const APawn* Pawn = PositionPawn.Get();
	return Pawn && Pawn->IsLocallyControlled() && Pawn->IsPlayerControlled();
}

void UCrowdyServerObjectComponent::ReleasePositionClaim()
{
	if (Object && Object->PositionReporter.Get() == this)
	{
		Object->PositionReporter.Reset();
	}
}

void UCrowdyServerObjectComponent::ReportPosition()
{
	UCrowdyServerObject* Held = Object;
	const AActor* Owner = GetOwner();
	if (!Held || !Owner || bPositionInFlight || !PositionInX || Held->GetStatus() != ECrowdyServerObjectStatus::Ready)
	{
		return;
	}
	if (!ShouldReportOwner())
	{
		ReleasePositionClaim();
		return;
	}
	if (PositionGeneration != CrowdyExec::GetStructGeneration() && !ResolvePositionAxes())
	{
		return;
	}
	const UObject* Reporter = Held->PositionReporter.Get();
	if (Reporter && Reporter != this)
	{
		return;
	}
	const double Now = PositionNow();
	const FVector Location = Owner->GetActorLocation();
	const bool bStill = bPositionSent && FVector::DistSquared(Location, PositionLast) < 1.0;
	if (Now < PositionHoldUntil || (bStill && Now - PositionSentAt < PositionStillSeconds))
	{
		return;
	}

	// The call encodes the inputs before it returns, and may answer before it does.
	PositionInX->SetPropertyValue_InContainer(PositionInputs.GetMutableMemory(), Location.X / 100.0);
	PositionInY->SetPropertyValue_InContainer(PositionInputs.GetMutableMemory(), Location.Y / 100.0);
	Held->PositionReporter = this;
	bPositionInFlight = true;
	bPositionSent = true;
	PositionSentAt = Now;
	PositionLast = Location;
	const uint32 Serial = ++PositionSerial;
	Held->Call(PositionFunction, PositionInputs, [Weak = TWeakObjectPtr<UCrowdyServerObjectComponent>(this), Serial, Location](const FCrowdyServerCallResult& Result)
	{
		if (UCrowdyServerObjectComponent* Self = Weak.Get())
		{
			Self->HandlePositionAnswer(Serial, Location, Result);
		}
	}, FMath::Max(1.f, 3.f * PositionPeriod));
}

void UCrowdyServerObjectComponent::HandlePositionAnswer(uint32 Serial, const FVector& Sent, const FCrowdyServerCallResult& Result)
{
	if (Serial != PositionSerial)
	{
		return;
	}
	bPositionInFlight = false;
	bPositionHoldCoded = false;
	if (!Result.IsSuccess())
	{
		bPositionHoldCoded = !Result.Code.IsNone() && !Result.bRetryable;
		PositionRefusedAt = PositionNow();
		PositionHoldUntil = PositionRefusedAt + (bPositionHoldCoded ? PositionCodedRefusalSeconds : PositionRefusalSeconds);
		return;
	}
	AActor* Owner = GetOwner();
	if (!Owner || !PositionReplyX || !PositionReplyY || Result.Reply.GetScriptStruct() != PositionReplyStruct)
	{
		return;
	}
	const double KeptX = PositionReplyX->GetPropertyValue_InContainer(Result.Reply.GetMemory()) * 100.0;
	const double KeptY = PositionReplyY->GetPropertyValue_InContainer(Result.Reply.GetMemory()) * 100.0;
	if (!FMath::IsFinite(KeptX) || !FMath::IsFinite(KeptY))
	{
		return;
	}
	const double CutX = KeptX - Sent.X;
	const double CutY = KeptY - Sent.Y;
	if (!(FMath::Square(CutX) + FMath::Square(CutY) > 25.0))
	{
		return;
	}
	// The correction, not the point: the owner may have moved while the answer was on its way.
	FVector Corrected = Owner->GetActorLocation();
	Corrected.X += CutX;
	Corrected.Y += CutY;
	if (bSnapToKeptPosition)
	{
		Owner->SetActorLocation(Corrected, false, nullptr, ETeleportType::TeleportPhysics);
	}
	OnPositionKept.Broadcast(Corrected);
}

void UCrowdyServerObjectComponent::BeginPlay()
{
	Super::BeginPlay();

	// The subsystem hears sign-ins once for every component, so thousands of them do not each bind to the SDK's event.
	if (UCrowdyServerObjectSubsystem* ServerObjects = FindServerObjects())
	{
		ServerObjects->AddSignInListener(this);
		bLoginBound = true;
	}

	UCrowdyTeams* Teams = InstanceMode == ECrowdyServerObjectInstanceMode::PlayersTeam ? CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyTeams>(GetWorld()) : nullptr;
	if (Teams)
	{
		Teams->OnMyTeamsCacheChanged.AddUniqueDynamic(this, &UCrowdyServerObjectComponent::HandleMyTeamsChanged);
		bTeamsBound = true;
	}

	if (FollowsSourceValue())
	{
		WatchSource();
		return;
	}
	Join();
}

bool UCrowdyServerObjectComponent::FollowsSourceValue() const
{
	return InstanceMode == ECrowdyServerObjectInstanceMode::FromServerValue && !(Definition && Definition->HasOnlyOneInstance());
}

void UCrowdyServerObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UCrowdyServerObjectSubsystem* ServerObjects = bLoginBound ? FindServerObjects() : nullptr;
	if (ServerObjects)
	{
		ServerObjects->RemoveSignInListener(this);
	}
	bLoginBound = false;
	Previous = nullptr;

	UCrowdyTeams* Teams = bTeamsBound ? CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyTeams>(GetWorld()) : nullptr;
	if (Teams)
	{
		Teams->OnMyTeamsCacheChanged.RemoveDynamic(this, &UCrowdyServerObjectComponent::HandleMyTeamsChanged);
	}
	bTeamsBound = false;
	TeamsRetryIn = 0.f;
	SetTeamsRetryArmed(false);
	SetFollowArmed(false);

	StopWatchingSource();
	Leave();
	Super::EndPlay(EndPlayReason);
}

void UCrowdyServerObjectComponent::Join()
{
	if (bJoining)
	{
		return;
	}
	TGuardValue<bool> JoiningGuard(bJoining, true);
	Leave();
	SetTeamsRetryArmed(false);

	if (!Definition)
	{
		Refuse(CrowdyServerObjectComponentDetail::PickDefinition, CrowdyServerObjectComponentDetail::FirstMissingDefinition(GetOwner()));
		return;
	}

	FString Error;
	const FString Id = ResolveInstanceId(Error);
	const bool bTeamsWait = Error.Equals(CrowdyServerObjectComponentDetail::WaitingForTeams, ESearchCase::CaseSensitive);
	if (bTeamsWait && TeamsRetryIn <= 0.f)
	{
		RequestTeams();
	}

	// Not a failure: it joins when the player signs in, their teams arrive or the source variable is set.
	const bool bSignInWait = FollowsSignedInPlayer();
	const bool bSourceWait = FollowsSourceValue() && !SourceVariable.IsNone() && SourceValue.IsEmpty();
	if (!Error.IsEmpty() && (bSignInWait || bTeamsWait || bSourceWait))
	{
		JoinError = bSignInWait ? FString(CrowdyServerObjectComponentDetail::WaitingForSignIn) : Error;
		SetTeamsRetryArmed(bTeamsWait);
		return;
	}
	if (!Error.IsEmpty())
	{
		Refuse(Error);
		return;
	}

	// The record is that of this actor's entity, whose id may arrive after BeginPlay.
	const AActor* Owner = GetOwner();
	const UCrowdyEntityComponent* FoundEntity = Owner ? Owner->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
	if (ReadsRecord() && !FoundEntity)
	{
		Refuse(TEXT("A Server Object with a record per entity needs a Crowdy Entity component on this actor; its id names the record"));
		return;
	}

	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (!Subsystem)
	{
		Refuse(TEXT("Server Objects are not available: there is no game instance"));
		return;
	}

	UCrowdyServerObject* Acquired = Subsystem->Acquire(Definition, Id, this, Error);
	if (!Acquired)
	{
		Refuse(Error);
		return;
	}

	Object = Acquired;
	JoinError.Reset();
	MessageBindings.Follow(Acquired, FOnCrowdyServerMessage::FDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::HandleMessage));
	StatusHandle = Acquired->OnStatusChanged.AddUObject(this, &UCrowdyServerObjectComponent::HandleStatus);
	Entity = FoundEntity;
	RecordNetId.Invalidate();
	RecordId.Reset();
	VariableBindings.ReadRecord(ReadsRecord(), RecordId);
	if (ReadsRecord())
	{
		FollowRecordId();
	}
	else
	{
		ValuesHandle = Acquired->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::HandleVariables));
	}
	if (Object != Acquired)
	{
		// A handler of the first values let go of it.
		Acquired->UnwatchValues(ValuesHandle);
		ValuesHandle.Reset();
		return;
	}
	SetFollowArmed(NeedsFollow());

	// A shared object that is already Ready sends no status change of its own.
	if (Acquired->GetStatus() == ECrowdyServerObjectStatus::Ready)
	{
		SetPositionArmed(true);
		OnStatusChanged.Broadcast(Acquired, ECrowdyServerObjectStatus::Ready);
	}
}

void UCrowdyServerObjectComponent::Leave()
{
	UCrowdyServerObject* Held = Object;
	if (!Held)
	{
		return;
	}

	SetPositionArmed(false);
	Object = nullptr;
	MessageBindings.Unfollow();
	Held->OnStatusChanged.Remove(StatusHandle);
	Held->UnwatchValues(ValuesHandle);
	Held->UnwatchRecord(RecordId, RecordHandle);
	StatusHandle.Reset();
	ValuesHandle.Reset();
	RecordHandle.Reset();
	RecordNetId.Invalidate();

	// A source that is also the held object is given back once, when it stops being watched; at shutdown the subsystem may be gone already.
	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (Held == SourceObject || !Subsystem)
	{
		return;
	}
	Subsystem->Release(Held, this);
}

UCrowdyServerObjectSubsystem* UCrowdyServerObjectComponent::FindServerObjects() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyServerObjectSubsystem* ForTest = ServerObjectsForTest.Get())
	{
		return ForTest;
	}
#endif
	return CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyServerObjectSubsystem>(GetWorld());
}

void UCrowdyServerObjectComponent::Refuse(const FString& Reason, bool bWarn)
{
	// A Failed handler that calls Rejoin would otherwise refuse again inside this broadcast, without end.
	TGuardValue<bool> JoiningGuard(bJoining, true);
	JoinError = Reason;
	SetTeamsRetryArmed(false);
	UE_CLOG(bWarn, LogCrowdyExec, Warning, TEXT("Crowdy Server Object on %s could not join: %s"), *GetNameSafe(GetOwner()), *Reason);
	OnStatusChanged.Broadcast(nullptr, ECrowdyServerObjectStatus::Failed);
}

void UCrowdyServerObjectComponent::WatchSource()
{
	if (bJoining)
	{
		return;
	}
	StopWatchingSource();
	Leave();

	if (!Definition)
	{
		Refuse(CrowdyServerObjectComponentDetail::PickDefinition, CrowdyServerObjectComponentDetail::FirstMissingDefinition(GetOwner()));
		return;
	}
	if (!SourceDefinition)
	{
		Refuse(TEXT("Pick a Source Definition on the Crowdy Server Object component"));
		return;
	}
	if (SourceVariable.IsNone())
	{
		Refuse(CrowdyServerObjectComponentDetail::SetSourceVariable);
		return;
	}

	const bool bPerPlayer = SourceInstance == ECrowdyServerObjectSourceInstance::SignedInPlayer;
	const int64 UserId = bPerPlayer ? CrowdyServerObjectComponentDetail::FindSignedInUserId(GetWorld()) : 0;
	if (bPerPlayer && UserId <= 0)
	{
		// Not a failure: it watches the source when the player signs in.
		JoinError = CrowdyServerObjectComponentDetail::WaitingForSignIn;
		return;
	}
	const FString SourceId = bPerPlayer ? LexToString(UserId) : SourceInstanceId;
	if (SourceId.IsEmpty())
	{
		Refuse(TEXT("Set a Source Instance Id on the Crowdy Server Object component"));
		return;
	}

	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (!Subsystem)
	{
		Refuse(TEXT("Server Objects are not available: there is no game instance"));
		return;
	}

	FString Error;
	UCrowdyServerObject* Acquired = Subsystem->Acquire(SourceDefinition, SourceId, this, Error);
	if (!Acquired)
	{
		Refuse(Error);
		return;
	}

	SourceObject = Acquired;
	JoinError = CrowdyServerObjectComponentDetail::WaitingForVariable(SourceVariable);
	SourceStatusHandle = Acquired->OnStatusChanged.AddUObject(this, &UCrowdyServerObjectComponent::HandleSourceStatus);
	const FDelegateHandle Watch = Acquired->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::HandleSourceVariables));
	if (SourceObject != Acquired)
	{
		// A handler of the first values let go of it.
		Acquired->UnwatchValues(Watch);
		return;
	}
	SourceValuesHandle = Watch;
}

void UCrowdyServerObjectComponent::StopWatchingSource()
{
	SourceValue.Reset();
	UCrowdyServerObject* Source = SourceObject;
	if (!Source)
	{
		return;
	}

	SourceObject = nullptr;
	Source->UnwatchValues(SourceValuesHandle);
	Source->OnStatusChanged.Remove(SourceStatusHandle);
	SourceValuesHandle.Reset();
	SourceStatusHandle.Reset();

	// A source that is also the held object is given back when the component leaves it.
	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (Source == Object || !Subsystem)
	{
		return;
	}
	Subsystem->Release(Source, this);
}

void UCrowdyServerObjectComponent::HandleSourceStatus(UCrowdyServerObject* InObject, ECrowdyServerObjectStatus Status)
{
	// A source that is also the held object passes its status on through HandleStatus.
	if (InObject != SourceObject || InObject == Object || Status != ECrowdyServerObjectStatus::Failed || Status != InObject->GetStatus())
	{
		return;
	}
	Leave();
	SourceValue.Reset();
	Refuse(InObject->GetFailureReason());
}

void UCrowdyServerObjectComponent::HandleSourceVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields)
{
	if (InObject != SourceObject)
	{
		return;
	}

	FString Value;
	if (!CrowdyServerObjectComponentDetail::ReadInstanceIdValue(InObject->GetState(), SourceVariable, Value))
	{
		Leave();
		SourceValue.Reset();
		const UCrowdyServerObjectDefinition* Source = InObject->GetDefinition();
		Refuse(FString::Printf(TEXT("Source Variable %s is not a String or an integer on %s"), *SourceVariable.ToString(), Source ? *Source->TypeName : TEXT("None")));
		return;
	}
	if (Value.IsEmpty())
	{
		Leave();
		SourceValue.Reset();
		JoinError = CrowdyServerObjectComponentDetail::WaitingForVariable(SourceVariable);
		return;
	}
	if (Value.Equals(SourceValue, ESearchCase::CaseSensitive))
	{
		return;
	}

	SourceValue = Value;
	Join();
}

void UCrowdyServerObjectComponent::HandleLogin(bool bSuccess)
{
	if (!bSuccess || !HasBegunPlay())
	{
		return;
	}
	HandleSignedIn();
}

void UCrowdyServerObjectComponent::HandleSignedIn()
{
	// The teams cache may still hold the last account's teams, so this account's are asked for and joined when they arrive.
	const bool bOnlyOne = Definition && Definition->HasOnlyOneInstance();
	if (InstanceMode == ECrowdyServerObjectInstanceMode::PlayersTeam && TeamId == 0 && !bOnlyOne)
	{
		Leave();
		JoinError = CrowdyServerObjectComponentDetail::WaitingForTeams;
		SetTeamsRetryArmed(true);
		RequestTeams();
		return;
	}

	const bool bFromSource = FollowsSourceValue();
	const ECrowdyServerObjectStatus SourceStatus = SourceObject ? SourceObject->GetStatus() : ECrowdyServerObjectStatus::Failed;
	const bool bSourceStale = SourceInstance == ECrowdyServerObjectSourceInstance::SignedInPlayer || SourceStatus == ECrowdyServerObjectStatus::Failed || SourceStatus == ECrowdyServerObjectStatus::Released;
	if (bFromSource && bSourceStale)
	{
		WatchSource();
		return;
	}

	// A different account may have signed in, so the signed-in player's object is always joined again.
	const bool bPerPlayer = FollowsSignedInPlayer() && !bOnlyOne;
	const ECrowdyServerObjectStatus Status = Object ? Object->GetStatus() : ECrowdyServerObjectStatus::Failed;
	if (bPerPlayer || Status == ECrowdyServerObjectStatus::Failed || Status == ECrowdyServerObjectStatus::Released)
	{
		Join();
	}
}

void UCrowdyServerObjectComponent::HandleMyTeamsChanged(const TArray<FCrowdyTeamMembership>& Memberships)
{
	if (!HasBegunPlay() || InstanceMode != ECrowdyServerObjectInstanceMode::PlayersTeam)
	{
		return;
	}

	// A cleared cache, as on sign-out, is no answer: the next sign-in asks for the teams again.
	FString Error;
	const FString Id = ResolveInstanceId(Error);
	if (Error.Equals(CrowdyServerObjectComponentDetail::WaitingForTeams, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (Object && Id.Equals(Object->GetInstanceId(), ESearchCase::CaseSensitive))
	{
		return;
	}
	Join();
}

void UCrowdyServerObjectComponent::HandleStatus(UCrowdyServerObject* InObject, ECrowdyServerObjectStatus Status)
{
	// A handler that ran first may have moved the object on already; only its current status is passed on.
	if (InObject != Object || Status != InObject->GetStatus())
	{
		return;
	}
	SetPositionArmed(Status == ECrowdyServerObjectStatus::Ready);
	OnStatusChanged.Broadcast(InObject, Status);
}

void UCrowdyServerObjectComponent::HandleVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields)
{
	if (InObject != Object)
	{
		return;
	}
	if (bPositionHoldCoded)
	{
		bPositionHoldCoded = false;
		PositionHoldUntil = FMath::Max(PositionNow(), PositionRefusedAt + PositionRefusalSeconds);
	}
	if (Previous && HoldsOwnRecord(InObject))
	{
		Previous = nullptr;
	}
	// The names are made only for a listener.
	if (OnVariablesChanged.IsBound())
	{
		OnVariablesChanged.Broadcast(InObject, InObject->ToVariableNames(Fields));
	}
	if (InObject != Object)
	{
		return;
	}
	VariableBindings.Notify(Object, Fields);
	// A pooled actor given another entity loses its old record, which is when its new id is looked for.
	if (InObject == Object && ReadsRecord() && !bJoining && !InObject->GetRecord(RecordId))
	{
		TGuardValue<bool> JoiningGuard(bJoining, true);
		FollowRecordId();
	}
}

void UCrowdyServerObjectComponent::HandleMessage(UCrowdyServerObject* InObject, FName Message, const FInstancedStruct& Payload)
{
	if (InObject == Object)
	{
		MessageBindings.Notify(Object, Message, Payload);
	}
}

bool UCrowdyServerObjectComponent::FollowsSignedInPlayer() const
{
	const bool bPlayersOwn = InstanceMode == ECrowdyServerObjectInstanceMode::InstanceId && InstanceId.IsEmpty() && Definition && Definition->IsKeyedByPlayer();
	return InstanceMode == ECrowdyServerObjectInstanceMode::SignedInPlayer || bPlayersOwn;
}

FString UCrowdyServerObjectComponent::GetFailureReason() const
{
	if (Object)
	{
		return Object->GetFailureReason();
	}
	if (SourceObject && SourceObject->GetStatus() == ECrowdyServerObjectStatus::Failed)
	{
		return SourceObject->GetFailureReason();
	}
	return JoinError;
}

void UCrowdyServerObjectComponent::Rejoin()
{
	if (!HasBegunPlay() || bJoining)
	{
		return;
	}

	// A retry asks for the teams again, in case the last request failed.
	TeamsRetryIn = 0.f;
	Previous = nullptr;
	StopWatchingSource();
	if (FollowsSourceValue())
	{
		WatchSource();
		return;
	}
	Join();
}
