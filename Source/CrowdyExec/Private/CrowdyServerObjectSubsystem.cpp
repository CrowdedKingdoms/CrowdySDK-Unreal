#include "CrowdyServerObjectSubsystem.h"

#include "CrowdyCppClient.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLink.h"
#include "CrowdyServerObjectShared.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystem/CrowdyTeams.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace
{
	bool IsValidServerObjectInstanceId(const FString& InstanceId)
	{
		const int32 NumBytes = FTCHARToUTF8(*InstanceId).Length();
		if (NumBytes < 1 || NumBytes > 256)
		{
			return false;
		}
		for (const TCHAR Char : InstanceId)
		{
			if (Char < 0x20 || (Char >= 0x7F && Char <= 0x9F))
			{
				return false;
			}
		}
		return true;
	}
}

void UCrowdyServerObjectSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Collection.InitializeDependency<UCrowdyGameSession>();
	Collection.InitializeDependency<UCrowdyCppClientSubsystem>();
	if (UCrowdySDKSubsystem* SDK = Collection.InitializeDependency<UCrowdySDKSubsystem>())
	{
		SDKSubsystem = SDK;
		SDK->OnLogout.AddDynamic(this, &UCrowdyServerObjectSubsystem::HandleLogout);
		SDK->OnLogin.AddDynamic(this, &UCrowdyServerObjectSubsystem::HandleLogin);
		SDK->OnRegister.AddDynamic(this, &UCrowdyServerObjectSubsystem::HandleLogin);
	}
	if (UCrowdyTeams* Teams = Collection.InitializeDependency<UCrowdyTeams>())
	{
		TeamsSubsystem = Teams;
		Teams->OnMyTeamsCacheChanged.AddDynamic(this, &UCrowdyServerObjectSubsystem::HandleMyTeamsChanged);
	}

	LastTickSeconds = FPlatformTime::Seconds();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UCrowdyServerObjectSubsystem::TickFromTicker), 0.1f);
	PreLoadMapHandle = FCoreUObjectDelegates::PreLoadMapWithContext.AddUObject(this, &UCrowdyServerObjectSubsystem::HandlePreLoadMap);
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UCrowdyServerObjectSubsystem::HandlePostLoadMap);
}

void UCrowdyServerObjectSubsystem::Deinitialize()
{
	FTSTicker::RemoveTicker(TickHandle);
	TickHandle.Reset();
	FCoreUObjectDelegates::PreLoadMapWithContext.Remove(PreLoadMapHandle);
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	if (UCrowdySDKSubsystem* SDK = SDKSubsystem.Get())
	{
		SDK->OnLogout.RemoveDynamic(this, &UCrowdyServerObjectSubsystem::HandleLogout);
		SDK->OnLogin.RemoveDynamic(this, &UCrowdyServerObjectSubsystem::HandleLogin);
		SDK->OnRegister.RemoveDynamic(this, &UCrowdyServerObjectSubsystem::HandleLogin);
	}
	SignInListeners.Reset();
	if (UCrowdyTeams* Teams = TeamsSubsystem.Get())
	{
		Teams->OnMyTeamsCacheChanged.RemoveDynamic(this, &UCrowdyServerObjectSubsystem::HandleMyTeamsChanged);
	}

	TArray<TObjectPtr<UCrowdyServerObjectLink>> Stopped;
	Links.GenerateValueArray(Stopped);
	Links.Reset();
	for (UCrowdyServerObjectLink* Link : Stopped)
	{
		Link->Stop();
	}

	// The game instance is going away: nothing may run game code now, so objects are released without notifying.
	const TArray<TObjectPtr<UCrowdyServerObject>> Released = SnapshotObjects();
	Objects.Empty();
	for (UCrowdyServerObject* Object : Released)
	{
		if (Object)
		{
			Object->MarkReleased(false);
		}
	}
	CloseConnection(TEXT(" (shutting down)"));
	bRedialing = false;

	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyServerObjectSubsystem::InitializeForTest(UCrowdyCppClientSubsystem* InClientHost, UCrowdyGameSession* InSession)
{
	ClientHostOverride = InClientHost;
	SessionOverride = InSession;
}
#endif

UCrowdyCppClientSubsystem* UCrowdyServerObjectSubsystem::GetClientHost() const
{
	if (UCrowdyCppClientSubsystem* Override = ClientHostOverride.Get())
	{
		return Override;
	}
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyCppClientSubsystem>() : nullptr;
}

int64 UCrowdyServerObjectSubsystem::GetSignedInUserId() const
{
	const UCrowdyGameSession* Session = GetSession();
	return Session ? Session->GetUserID() : 0;
}

double UCrowdyServerObjectSubsystem::GetChunkSize() const
{
	const UCrowdyGameSession* Session = GetSession();
	return Session ? Session->GetChunkSize() : 0.0;
}

bool UCrowdyServerObjectSubsystem::FindGroundChunk(const FVector& Location, int64& OutX, int64& OutY) const
{
	return CrowdyServerObjectShared::GroundChunkAt(Location, GetChunkSize(), OutX, OutY);
}

UCrowdyGameSession* UCrowdyServerObjectSubsystem::GetSession() const
{
	if (UCrowdyGameSession* Override = SessionOverride.Get())
	{
		return Override;
	}
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
}

UCrowdyServerObject* UCrowdyServerObjectSubsystem::Acquire(UCrowdyServerObjectDefinition* Definition, const FString& InstanceId, const UObject* Owner, FString& OutError)
{
	if (!Definition)
	{
		OutError = TEXT("Acquire needs a Server Object definition");
		return nullptr;
	}
	if (!Definition->ResolveLayout(OutError))
	{
		return nullptr;
	}
	const FString UsedId = Definition->bOnlyOneInstance ? FString(UCrowdyServerObjectDefinition::OnlyInstanceId) : InstanceId;
	if (!IsValidServerObjectInstanceId(UsedId))
	{
		OutError = TEXT("An Instance Id must be 1 to 256 bytes with no control characters");
		return nullptr;
	}
	if (!Owner)
	{
		OutError = TEXT("Acquire needs an owner");
		return nullptr;
	}
	OutError.Reset();

	FCrowdyServerObjectKey Key;
	Key.Definition = Definition;
	Key.InstanceId = UsedId;

	if (UCrowdyServerObject* Existing = Objects.FindRef(Key))
	{
		if (Existing->GetStatus() == ECrowdyServerObjectStatus::Failed)
		{
			Existing->Restart();
		}
		Existing->AddOwner(Owner);
		Existing->GraceRemaining = -1.f;
		if (ConnectRetryIn <= 0.f)
		{
			EnsureConnection();
		}
		return Existing;
	}

	UCrowdyServerObject* Object = NewObject<UCrowdyServerObject>(this);
	Object->Setup(this, Definition, UsedId);
	Object->AddOwner(Owner);
	Objects.Add(Key, Object);
	// While a reconnect backoff runs, Tick dials when it ends.
	if (ConnectRetryIn <= 0.f)
	{
		EnsureConnection();
	}
	if (GetConnection())
	{
		Object->HandleConnected();
	}
	return Object;
}

UCrowdyServerObjectLink* UCrowdyServerObjectSubsystem::FindLink(UObject* Owner, UCrowdyServerObjectDefinition* Definition, ECrowdyServerObjectFind Find, const FString& InstanceId, int64 TeamId)
{
	if (!Owner || !Definition)
	{
		return nullptr;
	}
	// Arguments the instance does not use are evened out, so every way of asking for it shares one Link.
	const bool bOnlyOne = Definition->bOnlyOneInstance;
	FCrowdyServerObjectLinkKey Key;
	Key.Owner = Owner;
	Key.Definition = Definition;
	Key.Find = bOnlyOne ? ECrowdyServerObjectFind::InstanceId : Find;
	Key.InstanceId = Key.Find != ECrowdyServerObjectFind::InstanceId ? FString() : bOnlyOne ? UCrowdyServerObjectDefinition::OnlyInstanceId : InstanceId;
	Key.TeamId = Key.Find == ECrowdyServerObjectFind::PlayersTeam ? TeamId : 0;
	if (UCrowdyServerObjectLink* Found = Links.FindRef(Key))
	{
		Found->SinceFound = 0.f;
		return Found;
	}
	UCrowdyServerObjectLink* Link = NewObject<UCrowdyServerObjectLink>(this);
	Links.Add(Key, Link);
	Link->Start(Owner, Definition, Key.Find, Key.InstanceId, Key.TeamId);
	return Link;
}

UObject* UCrowdyServerObjectSubsystem::MoveBinding(const UObject* Handler, FName Function, UObject* Holder)
{
	for (auto It = BindingHolders.CreateIterator(); It; ++It)
	{
		if (!It.Key().Key.IsValid())
		{
			It.RemoveCurrent();
		}
	}
	TWeakObjectPtr<UObject>& Slot = BindingHolders.FindOrAdd({TWeakObjectPtr<const UObject>(Handler), Function});
	UObject* Previous = Slot.Get();
	Slot = Holder;
	return Previous;
}

void UCrowdyServerObjectSubsystem::TickLinks(float DeltaSeconds, float GraceStep)
{
	TArray<UCrowdyServerObjectLink*, TInlineAllocator<16>> Kept;
	for (auto It = Links.CreateIterator(); It; ++It)
	{
		UCrowdyServerObjectLink* Link = It.Value();
		Link->SinceFound += GraceStep;
		// A Link only an On Changed node uses is never asked for again, so a live handler keeps it.
		const bool bUnused = Link->SinceFound > GracePeriodSeconds && !Link->GetVariableBindings().HasLiveHandler();
		if (Link->IsOwnerAlive() && !bUnused)
		{
			Kept.Add(Link);
			continue;
		}
		It.RemoveCurrent();
		Link->Stop();
	}
	for (UCrowdyServerObjectLink* Link : Kept)
	{
		Link->Tick(DeltaSeconds);
	}
}

void UCrowdyServerObjectSubsystem::Release(UCrowdyServerObject* Object, const UObject* Owner)
{
	if (!Object)
	{
		return;
	}
	Object->RemoveOwner(Owner);
}

FCrowdyNativeExecConnection* UCrowdyServerObjectSubsystem::GetConnection() const
{
	return bConnected ? Connection.Get() : nullptr;
}

bool UCrowdyServerObjectSubsystem::TickFromTicker(float FrameDeltaSeconds)
{
	// The core ticker passes the frame's delta, not the time since this ticker last ran.
	const double Now = FPlatformTime::Seconds();
	const float Elapsed = static_cast<float>(Now - LastTickSeconds);
	LastTickSeconds = Now;
	return Tick(Elapsed);
}

bool UCrowdyServerObjectSubsystem::Tick(float DeltaSeconds)
{
	const float GraceStep = bLoadingMap || IsSeamlessTravelling() ? 0.f : FMath::Min(DeltaSeconds, 1.f);
	TickLinks(DeltaSeconds, GraceStep);

	// Iterates a copy: releasing an object or ticking it can run game code that acquires or releases others.
	for (const TPair<FCrowdyServerObjectKey, TObjectPtr<UCrowdyServerObject>>& Entry : Objects.Array())
	{
		UCrowdyServerObject* Object = Entry.Value;
		if (!Object || Objects.FindRef(Entry.Key) != Object)
		{
			continue;
		}
		if (Object->PruneOwners())
		{
			Object->GraceRemaining = -1.f;
			Object->Tick(DeltaSeconds);
			continue;
		}

		Object->GraceRemaining = (Object->GraceRemaining < 0.f ? GracePeriodSeconds : Object->GraceRemaining) - GraceStep;
		if (Object->GraceRemaining <= 0.f)
		{
			Objects.Remove(Entry.Key);
			Object->MarkReleased(true);
			continue;
		}
		Object->Tick(DeltaSeconds);
	}

	if (!FindLiveObject())
	{
		CloseConnection(TEXT(" (no Server Object in use)"));
		bRedialing = false;
		ConnectAttempts = 0;
		ConnectRetryIn = 0.f;
		FailedDialTypes.Reset();
		return true;
	}
	if (Connection.IsValid() && IsDialStale())
	{
		CloseConnection();
		ConnectRetryIn = 0.f;
		FailedDialTypes.Reset();
	}
	if (Connection.IsValid())
	{
		// The connection redials a dropped socket itself and reports only the reconnect, so the drop is seen here.
		const bool bDropped = bConnected && !bTracedDrop && CrowdyExecTrace::Exec() && !Connection->IsConnected();
		UE_CLOG(bDropped, LogCrowdyExec, Log, TEXT("exec: connection closed"));
		bTracedDrop |= bDropped;
		return true;
	}
	ConnectRetryIn -= DeltaSeconds;
	if (ConnectRetryIn <= 0.f)
	{
		EnsureConnection();
	}
	return true;
}

const UCrowdyServerObject* UCrowdyServerObjectSubsystem::FindLiveObject() const
{
	for (const TPair<FCrowdyServerObjectKey, TObjectPtr<UCrowdyServerObject>>& Entry : Objects)
	{
		const UCrowdyServerObject* Object = Entry.Value;
		if (!Object)
		{
			continue;
		}
		const ECrowdyServerObjectStatus Status = Object->GetStatus();
		if (Status == ECrowdyServerObjectStatus::Connecting || Status == ECrowdyServerObjectStatus::Ready)
		{
			return Object;
		}
	}
	return nullptr;
}

const UCrowdyServerObject* UCrowdyServerObjectSubsystem::FindDialObject() const
{
	for (const TPair<FCrowdyServerObjectKey, TObjectPtr<UCrowdyServerObject>>& Entry : Objects)
	{
		const UCrowdyServerObject* Object = Entry.Value;
		const ECrowdyServerObjectStatus Status = Object ? Object->GetStatus() : ECrowdyServerObjectStatus::Released;
		if ((Status == ECrowdyServerObjectStatus::Connecting || Status == ECrowdyServerObjectStatus::Ready) && !FailedDialTypes.Contains(Object->GetDefinition()->TypeName))
		{
			return Object;
		}
	}
	return nullptr;
}

void UCrowdyServerObjectSubsystem::EnsureConnection()
{
	if (Connection.IsValid())
	{
		return;
	}
	// The dial places the shared connection with one object's type, so a type that cannot be placed must not block the rest.
	const UCrowdyServerObject* First = FindDialObject();
	if (!First)
	{
		FailedDialTypes.Reset();
		First = FindLiveObject();
	}
	if (!First)
	{
		return;
	}

	UCrowdyGameSession* Session = GetSession();
	UCrowdyCppClientSubsystem* Host = GetClientHost();
	if (!Session || !Host || Session->GetGameToken().IsEmpty())
	{
		ConnectRetryIn = 1.f;
		return;
	}

	FCrowdyCppClient* Client = Host->GetExistingClient();
	if (!Client)
	{
		Client = Host->GetClient(GetDefault<UCrowdySDKDeveloperSettings>()->MakeClientConfig());
	}
	if (!Client)
	{
		ScheduleReconnect();
		return;
	}

	FCrowdyNativeExecOptions Options;
	Options.NodeType = First->GetDefinition()->TypeName;
	Options.Key = First->GetInstanceId();

	// Read on every dial, so a reconnect after the token rotates carries the current one.
	const TWeakObjectPtr<UCrowdyGameSession> WeakSession = Session;
	Connection = Client->CreateExecConnection(Session->GetAppID(), Options, [WeakSession]()
	{
		const UCrowdyGameSession* Live = WeakSession.Get();
		return Live ? Live->GetGameToken() : FString();
	});
	if (!Connection.IsValid())
	{
		ScheduleReconnect();
		return;
	}
	DialedClient = Client;
	DialedAppId = Session->GetAppID();
	DialedType = Options.NodeType;

	const TWeakObjectPtr<UCrowdyServerObjectSubsystem> WeakThis = this;
	const TWeakPtr<FCrowdyNativeExecConnection> Dialed = Connection;
	Connection->OnReconnect([WeakThis, Dialed](const FString&)
	{
		UCrowdyServerObjectSubsystem* This = WeakThis.Get();
		if (This && This->IsCurrentConnection(Dialed))
		{
			This->HandleReconnect();
		}
	});

	bConnecting = true;
	Connection->Connect([WeakThis, Dialed](bool bOk, const FString& Error)
	{
		UCrowdyServerObjectSubsystem* This = WeakThis.Get();
		if (This && This->IsCurrentConnection(Dialed))
		{
			This->HandleConnectDone(bOk, Error);
		}
	});
}

bool UCrowdyServerObjectSubsystem::IsCurrentConnection(const TWeakPtr<FCrowdyNativeExecConnection>& Dialed) const
{
	return Connection.IsValid() && Dialed.Pin() == Connection;
}

bool UCrowdyServerObjectSubsystem::IsDialStale() const
{
	const UCrowdyCppClientSubsystem* Host = GetClientHost();
	const UCrowdyGameSession* Session = GetSession();
	return !Host || !Session || Host->GetExistingClient() != DialedClient || Session->GetAppID() != DialedAppId;
}

void UCrowdyServerObjectSubsystem::HandleConnectDone(bool bOk, const FString& Error)
{
	bConnecting = false;
	if (!bOk)
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("Server Objects could not connect to the server: %s"), *Error);
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s failed %s"), bRedialing ? TEXT("redial") : TEXT("dial"), *CrowdyExec::SafeText(Error, 256));
		FailedDialTypes.Add(DialedType);
		const FString Reason = FString::Printf(TEXT("Could not open the connection to the server for %s: %s"), *DialedType, *CrowdyExec::SafeText(Error, 256));
		for (UCrowdyServerObject* Object : SnapshotObjects())
		{
			if (Object)
			{
				Object->SetConnectProblem(Reason);
			}
		}
		CloseConnection();
		ScheduleReconnect();
		return;
	}

	UE_CLOG(CrowdyExecTrace::Exec() && bRedialing, LogCrowdyExec, Log, TEXT("exec: redial ok"));
	bRedialing = false;
	bConnected = true;
	ConnectAttempts = 0;
	ConnectRetryIn = 0.f;
	FailedDialTypes.Reset();

	// Handles are numbered per connection, so every old one is forgotten before anything subscribes on this one.
	const TArray<TObjectPtr<UCrowdyServerObject>> Snapshot = SnapshotObjects();
	TArray<UCrowdyServerObject*, TInlineAllocator<8>> Resubscribe;
	for (UCrowdyServerObject* Object : Snapshot)
	{
		if (!Object || Object->SubscribeHandle == 0)
		{
			continue;
		}
		Object->SubscribeHandle = 0;
		Object->bSubscribeConfirmed = false;
		if (Object->GetStatus() != ECrowdyServerObjectStatus::Failed)
		{
			Resubscribe.Add(Object);
		}
	}
	for (UCrowdyServerObject* Object : Resubscribe)
	{
		Object->TraceReread(TEXT("reconnect"));
		Object->Refresh();
	}
	for (UCrowdyServerObject* Object : Snapshot)
	{
		if (Object && Object->GetStatus() == ECrowdyServerObjectStatus::Connecting)
		{
			Object->SetConnectProblem(FString());
			Object->HandleConnected();
		}
	}
}

void UCrowdyServerObjectSubsystem::ScheduleReconnect()
{
	++ConnectAttempts;
	ConnectRetryIn = FMath::Min(FMath::Pow(2.f, static_cast<float>(ConnectAttempts - 1)), 30.f);
}

void UCrowdyServerObjectSubsystem::HandleReconnect()
{
	// A redial faster than the drop poll would otherwise print redial ok with no close before it.
	UE_CLOG(CrowdyExecTrace::Exec() && !bTracedDrop, LogCrowdyExec, Log, TEXT("exec: connection closed"));
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: redial ok"));
	bTracedDrop = false;
	for (UCrowdyServerObject* Object : SnapshotObjects())
	{
		if (Object)
		{
			Object->HandleReconnected();
		}
	}
}

void UCrowdyServerObjectSubsystem::CloseConnection(const TCHAR* Why)
{
	// Detached before closing, so completions the close delivers are recognised as belonging to an old connection.
	const TSharedPtr<FCrowdyNativeExecConnection> Closing = MoveTemp(Connection);
	UE_CLOG(CrowdyExecTrace::Exec() && Closing.IsValid() && bConnected && !bTracedDrop, LogCrowdyExec, Log, TEXT("exec: connection closed%s"), Why);
	bRedialing |= Closing.IsValid() && bConnected;
	bTracedDrop = false;
	DialedClient = nullptr;
	DialedAppId = 0;
	bConnected = false;
	bConnecting = false;
	if (Closing.IsValid())
	{
		Closing->Close();
	}
}

void UCrowdyServerObjectSubsystem::HandleLogout(bool bSuccess)
{
	for (UCrowdyServerObject* Object : SnapshotObjects())
	{
		if (Object && Object->GetStatus() != ECrowdyServerObjectStatus::Failed)
		{
			Object->Fail(TEXT("The player signed out"), true);
		}
	}
	CloseConnection(TEXT(" (signed out)"));
	bRedialing = false;
	FailedDialTypes.Reset();
}

void UCrowdyServerObjectSubsystem::AddSignInListener(UCrowdyServerObjectComponent* Component)
{
	SignInListeners.Add(Component);
}

void UCrowdyServerObjectSubsystem::RemoveSignInListener(UCrowdyServerObjectComponent* Component)
{
	SignInListeners.Remove(Component);
}

void UCrowdyServerObjectSubsystem::HandleLogin(bool bSuccess, FString Message)
{
	if (!bSuccess)
	{
		return;
	}
	// A listener's sign-in handling may add or remove listeners, so it runs over a copy.
	for (const TWeakObjectPtr<UCrowdyServerObjectComponent>& Listener : SignInListeners.Array())
	{
		UCrowdyServerObjectComponent* Component = Listener.Get();
		if (!Component)
		{
			SignInListeners.Remove(Listener);
			continue;
		}
		Component->HandleLogin(bSuccess);
	}
}

void UCrowdyServerObjectSubsystem::HandleMyTeamsChanged(const TArray<FCrowdyTeamMembership>& Memberships)
{
	TArray<int64> TeamIds;
	TeamIds.Reserve(Memberships.Num());
	for (const FCrowdyTeamMembership& Membership : Memberships)
	{
		TeamIds.Add(Membership.Team.TeamId);
	}
	TeamIds.Sort();
	if (TeamIds == MyTeamIds)
	{
		return;
	}
	MyTeamIds = MoveTemp(TeamIds);
	for (UCrowdyServerObject* Object : SnapshotObjects())
	{
		if (Object)
		{
			Object->HandleCrowdyTeamsChanged();
		}
	}
}

void UCrowdyServerObjectSubsystem::HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName)
{
	if (WorldContext.OwningGameInstance == GetGameInstance())
	{
		bLoadingMap = true;
	}
}

void UCrowdyServerObjectSubsystem::HandlePostLoadMap(UWorld* World)
{
	// A failed load reports a null world; loads are synchronous, so no other instance is mid-load then.
	if (!World || World->GetGameInstance() == GetGameInstance())
	{
		bLoadingMap = false;
	}
}

bool UCrowdyServerObjectSubsystem::IsSeamlessTravelling() const
{
	if (SeamlessTravelOverride.IsSet())
	{
		return SeamlessTravelOverride.GetValue();
	}
	const UGameInstance* GameInstance = GetGameInstance();
	const FWorldContext* Context = GameInstance ? GameInstance->GetWorldContext() : nullptr;
	return Context && Context->SeamlessTravelHandler.IsInTransition();
}

TArray<TObjectPtr<UCrowdyServerObject>> UCrowdyServerObjectSubsystem::SnapshotObjects() const
{
	TArray<TObjectPtr<UCrowdyServerObject>> Snapshot;
	Objects.GenerateValueArray(Snapshot);
	return Snapshot;
}
