#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CrowdyServerObjectTypes.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyServerObjectSubsystem.generated.h"

class FCrowdyNativeExecConnection;
class UCrowdyCppClientSubsystem;
class UCrowdyGameSession;
class UCrowdyServerObject;
class UCrowdyServerObjectComponent;
class UCrowdyServerObjectDefinition;
class UCrowdyServerObjectLink;
class UWorld;
struct FWorldContext;

/**
 * Hands out Server Objects and keeps them connected. Every owner that acquires the same definition and Instance Id
 * shares one object, which is given back a short while after its last owner is destroyed. Game thread only.
 */
UCLASS()
class CROWDYEXEC_API UCrowdyServerObjectSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** How long an object whose owners are all gone stays alive, so a quick re-acquire reuses it. */
	static constexpr float GracePeriodSeconds = 10.f;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * The Server Object for Definition and InstanceId, held for Owner until Owner is destroyed or released. Only One
	 * Instance types always give their only instance. Null, with OutError saying why, when it cannot be used.
	 */
	UCrowdyServerObject* Acquire(UCrowdyServerObjectDefinition* Definition, const FString& InstanceId, const UObject* Owner, FString& OutError);

	/** Stops Owner holding Object. An object left without owners is given back after the grace period. */
	void Release(UCrowdyServerObject* Object, const UObject* Owner);

	/** The open connection, or null while there is none. */
	FCrowdyNativeExecConnection* GetConnection() const;

	/** The signed-in player's user id, or 0 when nobody is signed in. */
	int64 GetSignedInUserId() const;

	/** The session's chunk size in world units, which actor updates place a location in a chunk by; 0 without a session. */
	double GetChunkSize() const;

	/** The chunk holding Location on the ground, as actor updates place it: Unreal's X and Y over the chunk size, rounded down. False without a session. */
	bool FindGroundChunk(const FVector& Location, int64& OutX, int64& OutY) const;

	/**
	 * The Link following Definition's instance for Owner: made on first use, the same one for the same arguments. Dropped
	 * when Owner is destroyed, or when it was not asked for during the grace period and no live handler is bound to it.
	 */
	UCrowdyServerObjectLink* FindLink(UObject* Owner, UCrowdyServerObjectDefinition* Definition, ECrowdyServerObjectFind Find, const FString& InstanceId, int64 TeamId);

	/** Has Component hear each sign-in and registration until RemoveSignInListener; adding and removing cost the same however many there are. */
	void AddSignInListener(UCrowdyServerObjectComponent* Component);
	void RemoveSignInListener(UCrowdyServerObjectComponent* Component);

	/** Records that Handler's Function is now bound through Holder, and gives the holder it was bound through before, if any. */
	UObject* MoveBinding(const UObject* Handler, FName Function, UObject* Holder);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a subsystem collection. Registers no ticker or delegates. */
	void InitializeForTest(UCrowdyCppClientSubsystem* InClientHost, UCrowdyGameSession* InSession);
	void TickForTest(float DeltaSeconds) { Tick(DeltaSeconds); }
	void SignOutForTest() { HandleLogout(true); }
	void SignInForTest() { HandleLogin(true, FString()); }
	void MyTeamsChangedForTest(const TArray<FCrowdyTeamMembership>& Memberships) { HandleMyTeamsChanged(Memberships); }
	void SetMapLoadingForTest(bool bLoading) { bLoadingMap = bLoading; }
	void PostLoadMapForTest(UWorld* World) { HandlePostLoadMap(World); }
	void SetSeamlessTravelForTest(bool bTravelling) { SeamlessTravelOverride = bTravelling; }
	bool IsSeamlessTravellingForTest() const { return IsSeamlessTravelling(); }
	int32 NumObjectsForTest() const { return Objects.Num(); }
	int32 NumLinksForTest() const { return Links.Num(); }
#endif

private:
	bool Tick(float DeltaSeconds);

	/** Opens the connection if objects need one and it is not open or opening. */
	void EnsureConnection();
	void HandleConnectDone(bool bConnected, const FString& Error);
	void HandleReconnect();
	/** Why names a close made on purpose, which the trace prints after "connection closed"; empty for a drop. */
	void CloseConnection(const TCHAR* Why = TEXT(""));

	UCrowdyCppClientSubsystem* GetClientHost() const;
	UCrowdyGameSession* GetSession() const;

	UFUNCTION()
	void HandleLogout(bool bSuccess);

	UFUNCTION()
	void HandleLogin(bool bSuccess, FString Message);

	TSet<TWeakObjectPtr<UCrowdyServerObjectComponent>> SignInListeners;

	/** The signed-in player's Crowdy Teams changed: records whose teams are Crowdy Teams are read again, as their Team Only values may now differ. */
	UFUNCTION()
	void HandleMyTeamsChanged(const TArray<FCrowdyTeamMembership>& Memberships);

	/** The signed-in player's Crowdy Team ids, sorted, as last heard. */
	TArray<int64> MyTeamIds;
	TWeakObjectPtr<class UCrowdyTeams> TeamsSubsystem;

	void HandlePreLoadMap(const FWorldContext& WorldContext, const FString& MapName);
	void HandlePostLoadMap(UWorld* World);
	/** Seamless travel reports no map load start, so it is read from the game instance's travel handler instead. */
	bool IsSeamlessTravelling() const;

	UPROPERTY()
	TMap<FCrowdyServerObjectKey, TObjectPtr<UCrowdyServerObject>> Objects;

	UPROPERTY()
	TMap<FCrowdyServerObjectLinkKey, TObjectPtr<UCrowdyServerObjectLink>> Links;

	/** Which holder each handler function is bound through, so binding it elsewhere moves it. */
	TMap<TPair<TWeakObjectPtr<const UObject>, FName>, TWeakObjectPtr<UObject>> BindingHolders;

	/** Drops Links whose owner is gone or that are no longer used, and ticks the rest. */
	void TickLinks(float DeltaSeconds, float GraceStep);

	TSharedPtr<FCrowdyNativeExecConnection> Connection;
	bool bConnected = false;
	bool bConnecting = false;
	/** The next dial follows the close of an open connection, so its result is traced as a redial; idle, sign-out and shutdown clear it. */
	bool bRedialing = false;
	/** The trace already reported the open connection's socket dropping. */
	bool bTracedDrop = false;
	int32 ConnectAttempts = 0;
	float ConnectRetryIn = 0.f;

	/** Grace periods do not run while this game instance loads a map. */
	bool bLoadingMap = false;

	TWeakObjectPtr<UCrowdyCppClientSubsystem> ClientHostOverride;
	TWeakObjectPtr<UCrowdyGameSession> SessionOverride;
	TOptional<bool> SeamlessTravelOverride;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle PreLoadMapHandle;
	FDelegateHandle PostLoadMapHandle;

	bool TickFromTicker(float FrameDeltaSeconds);

	/** The first object that is Connecting or Ready, or null when none needs the connection. */
	const UCrowdyServerObject* FindLiveObject() const;

	/** The first live object of a type no dial has failed with since the last good one, or null when every live type has. */
	const UCrowdyServerObject* FindDialObject() const;

	/** The type the connection was last dialled with, and those whose dial failed since the last good one. */
	FString DialedType;
	TSet<FString> FailedDialTypes;

	bool IsCurrentConnection(const TWeakPtr<FCrowdyNativeExecConnection>& Dialed) const;

	/** The shared client was rebuilt or the app changed since the connection was dialed; closing the client closed it. */
	bool IsDialStale() const;
	void ScheduleReconnect();
	TArray<TObjectPtr<UCrowdyServerObject>> SnapshotObjects() const;

	TWeakObjectPtr<class UCrowdySDKSubsystem> SDKSubsystem;
	double LastTickSeconds = 0.0;

	/** Compared by address only, never dereferenced. */
	const class FCrowdyCppClient* DialedClient = nullptr;
	int64 DialedAppId = 0;
};
