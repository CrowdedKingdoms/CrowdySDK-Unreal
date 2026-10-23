#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/DataAsset.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/SoftObjectPath.h"
#include "CrowdyServerObjectDefinition.generated.h"

struct FCrowdyExecLayout;
struct FInstancedStruct;

/** What a type is on the server. */
UENUM(BlueprintType)
enum class ECrowdyServerObjectKind : uint8
{
	/** Saves its variables, one instance per Instance Id, and runs one call at a time. */
	ServerObject UMETA(DisplayName = "Server Object"),
	/** Functions only, like a Blueprint Function Library: it saves nothing between calls, and several instances answer calls at once. */
	FunctionLibrary UMETA(DisplayName = "Server Function Library")
};

/** How a set of values is described. */
UENUM()
enum class ECrowdyServerValuesForm : uint8
{
	/** A C++ or Blueprint struct of your own. */
	Struct,
	/** Variables added right here, such as Amount (Integer), with no struct to make. */
	List UMETA(DisplayName = "Variables")
};

UENUM(BlueprintType)
enum class ECrowdyServerObjectVisibility : uint8
{
	/** Every player can read the variables visible to players and is told of each change. */
	Public UMETA(DisplayName = "Every Player"),
	/** The Instance Id is the owning player's user id, and only that player can read its variables or call its functions. */
	OwnerOnly UMETA(DisplayName = "Owner Only"),
	/** Only members read the variables; anyone can still see who the members are, and join. Needs Members From. */
	Members
};

/** What one instance keeps. */
UENUM(BlueprintType)
enum class ECrowdyServerObjectHolds : uint8
{
	/** One set of the variables, as most types keep. */
	OneSet UMETA(DisplayName = "One Set of Variables"),
	/** The variables once for each entity, keyed by the entity's id, such as health for every member of a crowd. */
	RecordPerEntity UMETA(DisplayName = "A Record per Entity")
};

/** Who receives a variable of a record that players can see. */
UENUM(BlueprintType)
enum class ECrowdyServerFieldAudience : uint8
{
	Everyone,
	/** Only the player who owns the record's entity. */
	Owner UMETA(DisplayName = "Owner Only"),
	/** Only players on the record's team, as Team From decides it. */
	Team UMETA(DisplayName = "Team Only")
};

/** Who is on a record's team, for its Team Only variables. */
UENUM(BlueprintType)
enum class ECrowdyServerTeamSource : uint8
{
	/** Players who own a record in the same instance with the same team; a player on no record there sees no Team Only values. */
	ThisObjectsRecords UMETA(DisplayName = "This Object's Records"),
	/** A record's team is a Crowdy Team id, and its members see the Team Only values in every instance. Deploying asks for players.read. */
	CrowdyTeam UMETA(DisplayName = "Crowdy Team")
};

UENUM(BlueprintType)
enum class ECrowdyServerFunctionCaller : uint8
{
	/** Players can call it, and so can other server code and developer tools. */
	Players,
	/** Only members, and other server code. Needs Members From. */
	Members,
	/** Only the leader, and other server code. Needs Members From. */
	Leader,
	/** Only other server code and developer tools can call it; a player's call is refused. */
	ServerOnly UMETA(DisplayName = "Server Only"),
	/** Only developer tools can call it, such as Call as Developer in the editor; players and other server code are refused. */
	DevelopersOnly UMETA(DisplayName = "Developers Only")
};

/** Who belongs to a Server Object. */
UENUM(BlueprintType)
enum class ECrowdyServerMembersSource : uint8
{
	None,
	/** The object keeps its own members: players Join and Leave, up to Max Members; the first to join leads. */
	ThisObject UMETA(DisplayName = "This Object"),
	/** The members of a Crowdy Team, whose id is the Instance Id; the team's leader role leads. */
	CrowdyTeam UMETA(DisplayName = "Crowdy Team")
};

UENUM(BlueprintType)
enum class ECrowdyServerTimerRepeat : uint8
{
	/** Runs again and again, Time apart. */
	Every,
	/** Runs once, Time after it is started. */
	After UMETA(DisplayName = "Once After"),
	/** Runs at the times a cron expression names, such as every six hours or each day at midnight. */
	Schedule UMETA(DisplayName = "On Schedule")
};

/** A timer on the server; logic.rs gets a function named after it, which runs each time it fires. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerTimer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer")
	FName Name;

	/** Every Time, once after Time, or on a Cron schedule. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer", meta = (DisplayName = "Runs"))
	ECrowdyServerTimerRepeat Repeat = ECrowdyServerTimerRepeat::Every;

	/** How long between runs, or before the one run. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer", meta = (DisplayName = "Time", ClampMin = 0.01, Units = "s", EditCondition = "Repeat != ECrowdyServerTimerRepeat::Schedule", EditConditionHides))
	float Seconds = 60.f;

	// The tooltip is in the meta because its example holds a star-slash, which would end a comment.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer", meta = (ToolTip = "When it runs: five fields (minute hour day-of-month month day-of-week), or six with seconds first, or @hourly, @daily, @weekly, @monthly or @yearly. 0 */6 * * * is every six hours on the hour. Times are UTC unless UTC Offset says otherwise.", EditCondition = "Repeat == ECrowdyServerTimerRepeat::Schedule", EditConditionHides))
	FString Cron;

	/** Reads the schedule's times this many minutes ahead of UTC: 60 runs 0 9 * * * at 09:00 in UTC+1. A fixed offset with no daylight saving. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer", meta = (DisplayName = "UTC Offset", ClampMin = -840, ClampMax = 840, EditCondition = "Repeat == ECrowdyServerTimerRepeat::Schedule", EditConditionHides))
	int32 UtcOffsetMinutes = 0;

	/** Starts when an instance first starts; otherwise server code starts it with timers::start. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer")
	bool bStartAutomatically = true;
};

/** The input of the built-in Add Member, Remove Member and Make Leader functions. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerMemberInputs
{
	GENERATED_BODY()

	/** The player's user id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Server Object")
	int64 Player = 0;
};

/** The input of the built-in Set Open for Joining function. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerOpenInputs
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Server Object")
	bool bOpen = true;
};

/** Where a type's Server Functions come from. */
UENUM()
enum class ECrowdyServerCodeSource : uint8
{
	/** Server/<Type Name>/src/logic.rs, which Generate Server Code writes once and never replaces. */
	Generated,
	/** A .rs file of your own, sent as the type's logic.rs. Generate Server Code writes no logic.rs for it. */
	OwnFile UMETA(DisplayName = "My Own File"),
	/** One of the platform's Game Kit starters: its Rust is the starter's own, and players reach it over the Game Kit's protocol. */
	GameKitStarter UMETA(DisplayName = "Game Kit Starter")
};

/** A function on a Server Object that players, or other server code, can call (a hub method on the platform). */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerFunction
{
	GENERATED_BODY()

	/** The function's name in Unreal. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function")
	FName Name;

	/** The function's name on the server (the method name). Empty uses Name in snake case: AttackBoss becomes attack_boss. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", AdvancedDisplay)
	FString ServerName;

	/** Whether the inputs are variables added here or a struct of your own. */
	UPROPERTY(EditAnywhere, Category = "Server Function", meta = (DisplayName = "Inputs From"))
	ECrowdyServerValuesForm ParamsForm = ECrowdyServerValuesForm::Struct;

	/** The struct the caller sends. None sends nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", meta = (DisplayName = "Input Struct",
		EditCondition = "ParamsForm == ECrowdyServerValuesForm::Struct", EditConditionHides))
	TObjectPtr<UScriptStruct> Params;

	/** The inputs the caller sends, with their default values. No inputs sends nothing. */
	UPROPERTY(EditAnywhere, Category = "Server Function", meta = (DisplayName = "Inputs", DefaultType = "Int32", IsPinTypeAccepted = "IsServerValueTypeAccepted",
		EditCondition = "ParamsForm == ECrowdyServerValuesForm::List", EditConditionHides))
	FInstancedPropertyBag ParamsList;

	/** Whether the outputs are variables added here or a struct of your own. */
	UPROPERTY(EditAnywhere, Category = "Server Function", meta = (DisplayName = "Outputs From"))
	ECrowdyServerValuesForm ReplyForm = ECrowdyServerValuesForm::Struct;

	/** The struct the server replies with. None replies nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", meta = (DisplayName = "Output Struct",
		EditCondition = "ReplyForm == ECrowdyServerValuesForm::Struct", EditConditionHides))
	TObjectPtr<UScriptStruct> Reply;

	/** The outputs the server replies with, with their default values. No outputs replies nothing. */
	UPROPERTY(EditAnywhere, Category = "Server Function", meta = (DisplayName = "Outputs", DefaultType = "Int32", IsPinTypeAccepted = "IsServerValueTypeAccepted",
		EditCondition = "ReplyForm == ECrowdyServerValuesForm::List", EditConditionHides))
	FInstancedPropertyBag ReplyList;

	/** Players, members, the leader, or only other server code and developer tools. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", meta = (DisplayName = "Callable By"))
	ECrowdyServerFunctionCaller WhoCanCall = ECrowdyServerFunctionCaller::Players;

	/** A feature key the calling player's access tier must grant, or the call is refused; matched exactly, capitals included. Features are set on the Access page in Crowdy Studio. Empty means none; other server code and developer tools are not checked. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", meta = (DisplayName = "Required Feature"))
	FString RequiredFeature;

	/** How long one player waits between calls that succeeded; 0 for no wait. Checked on the server. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", meta = (DisplayName = "Cooldown", ClampMin = 0, Units = "s"))
	float CooldownSeconds = 0.f;

	/** Game Kit starters only: sends an action id with each call, so the server runs it once however often it is sent again. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", meta = (DisplayName = "Send Once"))
	bool bSendOnce = false;

	/** The method name as of the last bake, spelled as in the editor, which a packaged game cannot read back from Name. */
	UPROPERTY()
	FString BakedMethodName;

	/** The method name: BakedMethodName, or worked out from ServerName and Name when the definition was never baked. */
	FString GetMethodName() const;

	/** What the caller sends: Params, or ParamsList's struct. Null when it sends nothing. */
	const UScriptStruct* GetParamsStruct() const;

	/** What the server replies with: Reply, or ReplyList's struct. Null when it replies nothing. */
	const UScriptStruct* GetReplyStruct() const;

	/** Out becomes a fresh set of this function's inputs: ParamsList's own values, or Params' defaults. Empty when it sends nothing. */
	void InitializeParams(FInstancedStruct& Out) const;

	/** Out becomes a fresh set of this function's outputs: ReplyList's own values, or Reply's defaults. Empty when it replies nothing. */
	void InitializeReply(FInstancedStruct& Out) const;

	/** The name ParamsList goes by in server code, such as AddTipParams; empty when the function sends a struct. */
	FString GetParamsListName() const;

	/** The name ReplyList goes by in server code, such as AddTipReply; empty when the function replies with a struct. */
	FString GetReplyListName() const;
};

/** A message a Server Object's server code sends to everyone listening: players and other Server Object types. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerMessage
{
	GENERATED_BODY()

	/** The message's name in Unreal. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Message")
	FName Name;

	/** The topic on the server. Empty uses Name in snake case: BossDefeated becomes boss_defeated. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Message", AdvancedDisplay)
	FString ServerName;

	/** Whether the payload is variables added here or a struct of your own. */
	UPROPERTY(EditAnywhere, Category = "Server Message", meta = (DisplayName = "Payload From"))
	ECrowdyServerValuesForm PayloadForm = ECrowdyServerValuesForm::Struct;

	/** Players can listen to this message. Off: only other Server Object types that Listen To this one hear it, and it does not open the type to players. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Message", meta = (DisplayName = "Players Can Listen"))
	bool bPlayersCanListen = true;

	/** The struct each message carries. None carries nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Message", meta = (DisplayName = "Payload Struct",
		EditCondition = "PayloadForm == ECrowdyServerValuesForm::Struct", EditConditionHides))
	TObjectPtr<UScriptStruct> Payload;

	/** The variables each message carries, with their default values. No variables carries nothing. */
	UPROPERTY(EditAnywhere, Category = "Server Message", meta = (DisplayName = "Payload", DefaultType = "Int32", IsPinTypeAccepted = "IsServerValueTypeAccepted",
		EditCondition = "PayloadForm == ECrowdyServerValuesForm::List", EditConditionHides))
	FInstancedPropertyBag PayloadList;

	/** The topic as of the last bake, spelled as in the editor, which a packaged game cannot read back from Name. */
	UPROPERTY()
	FString BakedTopic;

	/** The topic: BakedTopic, or worked out from ServerName and Name when the definition was never baked. */
	FString GetTopic() const;

	/** What each message carries: Payload, or PayloadList's struct. Null when it carries nothing. */
	const UScriptStruct* GetPayloadStruct() const;

	/** Out becomes a fresh payload: PayloadList's own values, or Payload's defaults. Empty when the message carries nothing. */
	void InitializePayload(FInstancedStruct& Out) const;

	/** The name PayloadList goes by in server code, such as BossDefeatedMessage; empty when the message carries a struct. */
	FString GetPayloadListName() const;
};

/** A Game Kit starter's value only the signed-in player reads, such as a player's wallet: answered by its own call, and read again when the starter says it changed for that player. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyKitOwnerValue
{
	GENERATED_BODY()

	/** The call that answers it, with no inputs, such as wallet. */
	UPROPERTY(EditAnywhere, Category = "Game Kit")
	FString Method;

	/** What the starter's changed messages call it, such as wallet. */
	UPROPERTY(EditAnywhere, Category = "Game Kit")
	FString What;

	/** The State's variable it fills, such as Wallet. */
	UPROPERTY(EditAnywhere, Category = "Game Kit")
	FName Variable;
};

/** Keeps a field's name on the server when the field is renamed in Unreal. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerFieldName
{
	GENERATED_BODY()

	/** The struct the field belongs to. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	TObjectPtr<UScriptStruct> Struct;

	/** The field as the struct names it in the editor. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	FName Field;

	/** The field's name on the server, kept when the field is renamed. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	FString ServerName;
};

/** Keeps an enum value's name on the server when the value is renamed in Unreal. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerEnumValueName
{
	GENERATED_BODY()

	/** The enum the value belongs to. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	TObjectPtr<UEnum> Enum;

	/** The value as the enum names it in the editor. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	FName Value;

	/** The value's name on the server, kept when the value is renamed. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	FString ServerName;
};

/** Keeps a List value's name on the server when the value is renamed in Unreal. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerListValueName
{
	GENERATED_BODY()

	/** The List's name, such as AddTipParams; renaming its function changes it, and the entry no longer applies. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	FName List;

	/** The value's id in the List, which stays the same when the value is renamed. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	FGuid ValueId;

	/** The value's name on the server, kept when the value is renamed. */
	UPROPERTY(EditAnywhere, Category = "Server Name")
	FString ServerName;
};

/** One of a definition's Lists: the name messages and server code know it by, and its values. */
struct FCrowdyServerNamedList
{
	FString Name;
	const FInstancedPropertyBag* Values = nullptr;
};

/** One field of a struct as it travels: its name on the server and how to find it again at runtime. */
USTRUCT()
struct CROWDYEXEC_API FCrowdyExecBakedField
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Baked")
	FString ServerName;

	UPROPERTY(VisibleAnywhere, Category = "Baked")
	FName Property;

	/** Set for Blueprint struct fields, which keep their id when renamed. */
	UPROPERTY(VisibleAnywhere, Category = "Baked")
	FGuid PropertyGuid;

	UPROPERTY(VisibleAnywhere, Category = "Baked")
	bool bWatched = false;

	UPROPERTY(VisibleAnywhere, Category = "Baked")
	ECrowdyServerFieldAudience Audience = ECrowdyServerFieldAudience::Everyone;

	/** A Game Kit starter's bool that records whether this field arrived with a value (bHas<Field>); it is no field of its own, so it has no entry. */
	UPROPERTY(VisibleAnywhere, Category = "Baked")
	FName PresenceProperty;
};

USTRUCT()
struct CROWDYEXEC_API FCrowdyExecBakedStruct
{
	GENERATED_BODY()

	/** Empty for a List, whose struct is made afresh on every load and is found by List instead. */
	UPROPERTY(VisibleAnywhere, Category = "Baked")
	TObjectPtr<UScriptStruct> Struct;

	/** The List's name, such as AddTipParams, when the values are a List. */
	UPROPERTY(VisibleAnywhere, Category = "Baked")
	FString List;

	/** In the order they travel: base struct first, then declaration order. */
	UPROPERTY(VisibleAnywhere, Category = "Baked")
	TArray<FCrowdyExecBakedField> Fields;
};

USTRUCT()
struct CROWDYEXEC_API FCrowdyExecBakedEnum
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Baked")
	TObjectPtr<UEnum> Enum;

	UPROPERTY(VisibleAnywhere, Category = "Baked")
	TArray<int64> Values;

	UPROPERTY(VisibleAnywhere, Category = "Baked")
	TArray<FString> ServerNames;

	/** Each value's enumerator, which keeps its name when values are renumbered, so a changed enum is noticed. */
	UPROPERTY(VisibleAnywhere, Category = "Baked")
	TArray<FName> EnumeratorNames;
};

/** One Server Object type (a hub type on the platform, or a spoke when it is a Server Function Library): its state, the values players can watch, and the functions they can call. */
UCLASS(BlueprintType)
class CROWDYEXEC_API UCrowdyServerObjectDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The type's name on the server (the node type): lowercase letters, digits and underscores, starting with a letter. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object")
	FString TypeName;

	/** A Server Object saves its variables; a Server Function Library only has functions, saves nothing between calls, and answers many calls at once. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object")
	ECrowdyServerObjectKind Kind = ECrowdyServerObjectKind::ServerObject;

	bool IsFunctionLibrary() const { return Kind == ECrowdyServerObjectKind::FunctionLibrary; }

	/** Whether nothing picks an instance: Only One Instance is on, or this is a Server Function Library. */
	bool HasOnlyOneInstance() const { return bOnlyOneInstance || IsFunctionLibrary(); }

	/** Every player, only the player who owns the instance, or only its members. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Readable By"))
	ECrowdyServerObjectVisibility Visibility = ECrowdyServerObjectVisibility::Public;

	/** One shared instance for every player, such as a registry or a world event; nothing picks an instance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Only One Instance"))
	bool bOnlyOneInstance = false;

	/** The Instance Id of a type with Only One Instance. */
	static const TCHAR* const OnlyInstanceId;

	/** The platform's root type, every app's parent of the others; its one instance's Instance Id is empty. */
	static const TCHAR* const RootTypeName;

	/** The Instance Id a type that HasOnlyOneInstance is reached by: empty for the root, a Server Function Library and a Game Kit starter, else Only Instance Id. */
	FString GetOnlyInstanceId() const;

	/** Its server code may read players: their names, tier features, group membership and app state; deploying asks the platform for players.read. Crowdy Team members or records and a Required Feature ask for it too. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Reads Players"))
	bool bReadsPlayers = false;

	/** Its server code may replace a player's app state; deploying asks the platform for players.write. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Changes Players' App State"))
	bool bWritesPlayerState = false;

	/** Its server code may read world data, such as a chunk's voxels or which actors are near a chunk; deploying asks the platform for the world.read permission. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Reads World Data"))
	bool bReadsWorld = false;

	/** Its server code may write voxels and a chunk's state; deploying asks the platform for world.write. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Changes the World"))
	bool bWritesWorld = false;

	/** Its server code may read the app's grids: which grid covers a chunk, its owner, and whether a player holds a permission on it; deploying asks the platform for grids.read. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Reads Grids"))
	bool bReadsGrids = false;

	/** Its server code may create and delete grids, give a group keys on one, and open one to every player, as an app admin can in Studio; deploying asks the platform for grids.write. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Manages Grids"))
	bool bManagesGrids = false;

	/** Its server code may grant and revoke players' permissions on the app's grids; deploying asks the platform for permissions.write. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Grants Grid Permissions"))
	bool bGrantsGridPermissions = false;

	/** Its server code may read a player's access to the app: status, tier, expiry and suspension; deploying asks the platform for access.read. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Reads Players' Access"))
	bool bReadsAccess = false;

	/** Its server code may grant, revoke and suspend players' access to the app, as an app admin can in Studio; deploying asks the platform for access.write. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Manages Players' Access"))
	bool bManagesAccess = false;

	/** One set of the variables, or a record of them for each entity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Holds"))
	ECrowdyServerObjectHolds Holds = ECrowdyServerObjectHolds::OneSet;

	/** Whether the variables are added here or come from a struct of your own. */
	UPROPERTY(EditAnywhere, Category = "Server Object", meta = (DisplayName = "Variables From"))
	ECrowdyServerValuesForm StateForm = ECrowdyServerValuesForm::Struct;

	/** Everything the server keeps for one instance (the hub's state, saved in its snapshots). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "State Struct",
		EditCondition = "StateForm == ECrowdyServerValuesForm::Struct", EditConditionHides))
	TObjectPtr<UScriptStruct> State;

	/** Everything the server keeps for one instance, with the values a new instance starts from. */
	UPROPERTY(EditAnywhere, Category = "Server Object", meta = (DisplayName = "Variables", DefaultType = "Int32", IsPinTypeAccepted = "IsServerValueTypeAccepted",
		EditCondition = "StateForm == ECrowdyServerValuesForm::List", EditConditionHides))
	FInstancedPropertyBag StateList;

	/** The variables players can read and watch (sent on the state topic). The others never leave the server. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Visible to Players"))
	TArray<FName> WatchedFields;

	/** The Visible to Players variables that only the player who owns the record's entity receives. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Visible to Owner Only",
		EditCondition = "Holds == ECrowdyServerObjectHolds::RecordPerEntity", EditConditionHides))
	TArray<FName> OwnerFields;

	/** The Visible to Players variables that only players on the record's team receive. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Visible to Team Only",
		EditCondition = "Holds == ECrowdyServerObjectHolds::RecordPerEntity", EditConditionHides))
	TArray<FName> TeamFields;

	/** The most records one instance keeps. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Records", meta = (DisplayName = "Max Records", ClampMin = 1, ClampMax = 4096,
		EditCondition = "Holds == ECrowdyServerObjectHolds::RecordPerEntity", EditConditionHides))
	int32 MaxRecords = 2048;

	/** Changes made in between are sent together; the last value wins. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Records", meta = (DisplayName = "Send Changes Every", ClampMin = 50, ClampMax = 5000, ForceUnits = "ms",
		EditCondition = "Holds == ECrowdyServerObjectHolds::RecordPerEntity", EditConditionHides))
	int32 PushIntervalMs = 250;

	/** Who receives a record's Team Only variables besides its owner. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Records", meta = (DisplayName = "Team From",
		EditCondition = "Holds == ECrowdyServerObjectHolds::RecordPerEntity", EditConditionHides))
	ECrowdyServerTeamSource TeamFrom = ECrowdyServerTeamSource::ThisObjectsRecords;

	/** Whether this holds records whose teams are Crowdy Team ids. */
	bool HasCrowdyTeamRecords() const { return !IsFunctionLibrary() && Holds == ECrowdyServerObjectHolds::RecordPerEntity && TeamFrom == ECrowdyServerTeamSource::CrowdyTeam; }

	/** When instances are keyed by area, each covers Area Size by Area Size chunks on the ground and its Instance Id is the area's x,y. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Areas", meta = (DisplayName = "Area Size", ClampMin = 1, ClampMax = 64))
	int32 AreaSizeChunks = 4;

	/** When instances are keyed by area, how many areas out from a player's own are followed: 1 follows the 3 by 3 around it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Areas", meta = (DisplayName = "Areas Followed Around a Player", ClampMin = 0, ClampMax = 3))
	int32 InterestRadiusAreas = 1;

	/** The key of the area holding the chunk: x,y, the ground pair (Unreal's X and Y, not its height), each divided by Area Size and rounded down, such as -1,2. */
	static FString MakeAreaKey(int64 ChunkX, int64 ChunkY, int32 AreaSize);

	/** MakeAreaKey with this definition's Area Size. */
	FString GetAreaKey(int64 ChunkX, int64 ChunkY) const;

	/** What players, or other server code, can call on an instance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Functions")
	TArray<FCrowdyServerFunction> Functions;

	/** What the server code sends to everyone listening: players, and other Server Object types that Listen To this one. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Messages")
	TArray<FCrowdyServerMessage> Messages;

	/** The message called Name, or null. */
	const FCrowdyServerMessage* FindMessage(FName Name) const;

	/** The message sent on Topic, or null. */
	const FCrowdyServerMessage* FindMessageByTopic(const FString& Topic) const;

	/** Who belongs to an instance: nobody, members it keeps itself, or a Crowdy Team's members. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Members", meta = (DisplayName = "Members From"))
	ECrowdyServerMembersSource MembersFrom = ECrowdyServerMembersSource::None;

	/** The most members an instance takes; 0 for no limit, which needs Show Members to Players off. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Members", meta = (ClampMin = 0,
		EditCondition = "MembersFrom == ECrowdyServerMembersSource::ThisObject", EditConditionHides))
	int32 MaxMembers = 100;

	/** Players can see who the members are (Get Members). Turn it off for large groups: then only the member count travels. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Members", meta = (DisplayName = "Show Members to Players",
		EditCondition = "MembersFrom == ECrowdyServerMembersSource::ThisObject", EditConditionHides))
	bool bShowMembers = true;

	/** A member whose last connection to the instance closes (they left the game) stops being a member. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Members", meta = (
		EditCondition = "MembersFrom == ECrowdyServerMembersSource::ThisObject", EditConditionHides))
	bool bRemoveMembersWhoLeave = true;

	/** Timers the server runs for each instance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timers & Events")
	TArray<FCrowdyServerTimer> Timers;

	/** logic.rs gets on_player_joined, run when a player's first connection to the instance opens. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timers & Events", meta = (DisplayName = "On Player Joined"))
	bool bOnPlayerJoined = false;

	/** logic.rs gets on_player_left, run when a player's last connection to the instance closes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timers & Events", meta = (DisplayName = "On Player Left"))
	bool bOnPlayerLeft = false;

	/** Whether players can do anything with an instance: call a function, watch a value, read records, be a member, listen to a message that players can listen to, or be heard joining or leaving. Deploying closes a type that is not to players, and acquiring one is refused. */
	bool IsReachableByPlayers() const;

	/** Whether any message lets players listen. */
	bool HasPlayerMessages() const;

	/** Whether some players may read less than every player can: Readable By is not Every Player, or some variables are Owner Only or Team Only. Messages still reach every player who connects. */
	bool HasRestrictedReaders() const;

#if WITH_EDITORONLY_DATA
	/** Other Server Object types this one's server code calls. Used when server code is generated and deployed; a game never loads them. */
	UPROPERTY(EditAnywhere, Category = "Can Call")
	TArray<TObjectPtr<UCrowdyServerObjectDefinition>> CanCall;

	/** Server code may call, and listen to, every type in the app, including types added later. Generated helpers are still made only for the types listed in Can Call. */
	UPROPERTY(EditAnywhere, Category = "Can Call", meta = (DisplayName = "Can Call Any Type"))
	bool bCanCallAnyType = false;

	/** Instances of a Server Function Library kept running at all times (replicas). Each counts toward the app's instance limit. */
	UPROPERTY(EditAnywhere, Category = "Scaling", meta = (DisplayName = "Min Instances", ClampMin = 1, ClampMax = 64, NoSpinbox = true,
		EditCondition = "Kind == ECrowdyServerObjectKind::FunctionLibrary", EditConditionHides))
	int32 MinInstances = 1;

	/** The server adds instances up to this many once calls have waited 2 seconds, and removes one after about 30 idle seconds (max_replicas). Equal to Min Instances, it never adds any. */
	UPROPERTY(EditAnywhere, Category = "Scaling", meta = (DisplayName = "Max Instances", ClampMin = 1, ClampMax = 64, NoSpinbox = true,
		EditCondition = "Kind == ECrowdyServerObjectKind::FunctionLibrary", EditConditionHides))
	int32 MaxInstances = 1;

	/** Calls each instance answers at the same time (concurrency). */
	UPROPERTY(EditAnywhere, Category = "Scaling", meta = (DisplayName = "Parallel Calls per Instance", ClampMin = 1, ClampMax = 256, NoSpinbox = true,
		EditCondition = "Kind == ECrowdyServerObjectKind::FunctionLibrary", EditConditionHides))
	int32 ParallelCallsPerInstance = 16;

	/** Other Server Object types whose messages this one's server code can listen to; each is also callable. */
	UPROPERTY(EditAnywhere, Category = "Listen To")
	TArray<TObjectPtr<UCrowdyServerObjectDefinition>> ListenTo;

	/** Memory each running instance may use (memory_mb). Every instance reserves it from the app's memory limit, so a smaller type fits more instances. */
	UPROPERTY(EditAnywhere, Category = "Server Limits", meta = (DisplayName = "Memory per Instance", ClampMin = 1, ClampMax = 512, ForceUnits = "MB", NoSpinbox = true))
	int32 MemoryMegabytes = 64;

	/** How long one call may run before the server stops it and the caller gets Timeout (deadline_ms). */
	UPROPERTY(EditAnywhere, Category = "Server Limits", meta = (DisplayName = "Time per Call", ClampMin = 10, ClampMax = 10000, ForceUnits = "ms", NoSpinbox = true))
	int32 CallTimeLimitMs = 2000;

	/** How much work one call may do, in millions of fuel, before the server stops it (fuel_per_call). */
	UPROPERTY(EditAnywhere, Category = "Server Limits", meta = (DisplayName = "Fuel per Call (Millions)", ClampMin = 1, ClampMax = 1000, NoSpinbox = true))
	int32 FuelPerCallMillions = 50;

	/** Calls that may wait at one instance; past it a call is answered Busy (mailbox). */
	UPROPERTY(EditAnywhere, Category = "Server Limits", meta = (DisplayName = "Calls Waiting", ClampMin = 1, ClampMax = 4096, NoSpinbox = true))
	int32 MaxWaitingCalls = 1024;
#endif

	/** How often the server saves a running instance (persist_every_ms). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Save Every", ClampMin = 5, ClampMax = 60, ForceUnits = "s", NoSpinbox = true))
	int32 SaveIntervalSeconds = 30;

	/** How long an unused instance keeps running before the server stops it (evict_after_ms). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Stop When Idle For", ClampMin = 1, ClampMax = 1800, ForceUnits = "s", NoSpinbox = true))
	int32 IdleTimeoutSeconds = 300;

#if WITH_EDITORONLY_DATA
	/** What this type is for, shown beside it in Server Compute. */
	UPROPERTY(EditAnywhere, Category = "Server Object", meta = (MultiLine = true))
	FString Description;
#endif

	/** Kept in a packaged game too, which reaches a Game Kit starter over the Game Kit's protocol. */
	UPROPERTY(EditAnywhere, Category = "Server Code")
	ECrowdyServerCodeSource CodeSource = ECrowdyServerCodeSource::Generated;

	bool IsGameKitStarter() const { return CodeSource == ECrowdyServerCodeSource::GameKitStarter; }

	/** The topic whose snapshots are a Game Kit starter's variables, such as world or queue. Empty when players watch none. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "State Topic"))
	FString KitStateTopic;

	/** The topic whose members are a Game Kit starter's records, such as nodes for world: Find Record and On Records Changed read them by member id. Empty when it has none. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "Collection Topic"))
	FString KitCollectionTopic;

	/** One member of the collection, the values Find Record gives; a field whose server name is id takes the member's id. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "Record Struct"))
	TObjectPtr<UScriptStruct> KitRecord;

	/** Whether this is a Game Kit starter whose Collection Topic's members are its records. */
	bool HasKitCollection() const { return IsGameKitStarter() && !KitCollectionTopic.IsEmpty() && KitRecord; }

	/** Values only the signed-in player reads, each filling a variable of the State the State Topic does not carry: a player's inventory, wallet and progress, a market's orders and trades. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "Owner Values"))
	TArray<FCrowdyKitOwnerValue> KitOwnerValues;

	/** The Instance Id is a player's user id, as for the player starter: an empty one means the signed-in player, and Owner Values are read only on that player's own. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "Keyed By Player"))
	bool bKitKeyedByPlayer = false;

	/** Whether this is a Game Kit starter whose Instance Id is a player's user id. */
	bool IsKeyedByPlayer() const { return IsGameKitStarter() && bKitKeyedByPlayer; }

	/** The function that reports where the player is, such as Move for the arena: its inputs' X and Y take the owner's location in metres (Unreal units over 100). A Crowdy Server Object component sends it while the Server Object is Ready. None for a starter that does not track players. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit", meta = (DisplayName = "Position Function"))
	FName KitPositionFunction;

	/** How many times a second a component reports, from 1 to 10. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit", meta = (DisplayName = "Position Reports Per Second", ClampMin = 1, ClampMax = 10))
	float KitPositionRate = 5.f;

	/** Whether this is a Game Kit starter that tracks where its players are. */
	bool ReportsPosition() const { return IsGameKitStarter() && !KitPositionFunction.IsNone(); }

	/** The actors this starter shows the players near it, by kind (mob, npc, pet, ball), and the class each is drawn as; each gets a Crowdy Kit Actor component. While it has a kind, the definition is listed in the Game Kit Actor Starters project setting. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit", meta = (DisplayName = "Kit Actors"))
	TMap<FName, TSoftClassPtr<AActor>> KitActors;

#if WITH_EDITORONLY_DATA
	/** The Game Kit type this one runs under, such as world for a matchmaker; under the app's root when the project has no type of that name. Empty makes this starter the app's root, as world is. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "Parent"))
	FString KitParent;

	/** The types this starter's server code calls, such as session for a matchmaker; Deploy names those the project has. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "Calls"))
	TArray<FString> KitCalls;

	/** The starter's JSON config, its spawn seed, such as {"day_ms": 600000} for world or {"board": "weekly"} for a session that posts its scores to that leaderboard; empty keeps the starter's defaults. Read only where its Rust's spawn reads the seed (world's, session's, player's and market's do; the matchmaker's and leaderboard's do not), and only when an instance first starts. */
	UPROPERTY(EditAnywhere, Category = "Game Kit", meta = (DisplayName = "Config", MultiLine = true))
	FString KitConfig;
#endif

#if WITH_EDITORONLY_DATA
	/** Your own server code (the logic.rs Server Functions), used when Code Source is My Own File. */
	UPROPERTY(EditAnywhere, Category = "Server Code", meta = (FilePathFilter = "Rust source (*.rs)|*.rs", RelativeToGameDir,
		EditCondition = "CodeSource == ECrowdyServerCodeSource::OwnFile", EditConditionHides))
	FFilePath LogicFile;
#endif

	/** Keeps a struct field's name on the server when you rename the field in Unreal. */
	UPROPERTY(EditAnywhere, Category = "Server Names", AdvancedDisplay)
	TArray<FCrowdyServerFieldName> FieldNames;

	/** Keeps an enum value's name on the server when you rename the value in Unreal. */
	UPROPERTY(EditAnywhere, Category = "Server Names", AdvancedDisplay)
	TArray<FCrowdyServerEnumValueName> EnumValueNames;

	/** Keeps a List value's name on the server when you rename the value in Unreal. */
	UPROPERTY(EditAnywhere, Category = "Server Names", AdvancedDisplay)
	TArray<FCrowdyServerListValueName> ListValueNames;

	/** The structs' field tables, rebuilt from the structs on every edit and save. */
	UPROPERTY(VisibleAnywhere, Category = "Baked", AdvancedDisplay)
	TArray<FCrowdyExecBakedStruct> BakedStructs;

	/** The enums' value tables, rebuilt from the enums on every edit and save. */
	UPROPERTY(VisibleAnywhere, Category = "Baked", AdvancedDisplay)
	TArray<FCrowdyExecBakedEnum> BakedEnums;

	/** Rebuilds the field tables from the structs, and each function's method name and message's topic. On a problem it clears the tables and returns every problem found. */
	bool Bake(TArray<FString>& OutErrors);

	/** Builds the field tables without storing them. */
	bool BuildTables(TArray<FCrowdyExecBakedStruct>& OutStructs, TArray<FCrowdyExecBakedEnum>& OutEnums, TArray<FString>& OutErrors) const;

	/** The tables resolved against the loaded structs, built on first use and again after a struct changes. */
	const FCrowdyExecLayout* ResolveLayout(FString& OutError) const;

	/** The State: State, or StateList's struct. Null when none is chosen. */
	const UScriptStruct* GetStateStruct() const;

	/** The struct one record holds: a Game Kit starter's Record Struct when it has a collection, else the State. */
	const UScriptStruct* GetRecordStruct() const { return HasKitCollection() ? KitRecord.Get() : GetStateStruct(); }

	/** The State's variable called Variable, ignoring case, spaces and punctuation; null when there is none that travels. */
	const FProperty* FindVariable(FName Variable) const;

	/** Out becomes Struct's starting values: a List's values as set here, otherwise the struct's defaults. Null Struct empties Out; a function's own values come from its InitializeParams or InitializeReply, a message's from its InitializePayload. */
	void InitializeValues(const UScriptStruct* Struct, FInstancedStruct& Out) const;

	/** The name a List's values go by in messages and server code, such as AddTipParams; empty when Struct is not one of this definition's Lists. */
	FString FindListName(const UScriptStruct* Struct) const;

	/** The struct of the List that FindListName calls Name. */
	const UScriptStruct* FindListStruct(const FString& Name) const;

	/** Every List in use, by the name FindListName gives it: the State's first, then each function's inputs and outputs, then each message's payload. */
	void GetLists(TArray<FCrowdyServerNamedList>& Out) const;

	/** The function called Name: one of Functions, or a built-in member function when Members From is This Object. */
	const FCrowdyServerFunction* FindFunction(FName Name) const;

	/** Join, Leave, Add Member, Remove Member, Make Leader and Set Open for Joining, as This Object members offer them. */
	static TConstArrayView<FCrowdyServerFunction> GetMemberFunctions();

	/** Whether Name is a built-in member function. */
	static bool IsMemberFunction(FName Name);

#if WITH_EDITOR
	/** Offers only the types a Server Object can carry in the editor's variable type picker. */
	UFUNCTION()
	bool IsServerValueTypeAccepted(FEdGraphPinType PinType, bool bIsChild) const;

	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
	/** Keeps ServerName as the server name of the value ValueId in List, adding or updating its List Value Names entry, then re-bakes. */
	void KeepListValueServerName(FName List, const FGuid& ValueId, const FString& ServerName);

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;

	/** Lists this definition in the Game Kit Actor Starters project setting while its Kit Actors has a kind, and takes it out once it has none. */
	void SyncGameKitActorStarters() const;
#endif

private:
	mutable TSharedPtr<FCrowdyExecLayout> Layout;
	mutable uint32 LayoutGeneration = 0;
};
