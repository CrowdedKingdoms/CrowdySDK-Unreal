#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/DataAsset.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/SoftObjectPath.h"
#include "CrowdyServerObjectDefinition.generated.h"

struct FCrowdyExecLayout;
struct FInstancedStruct;

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
	ServerOnly UMETA(DisplayName = "Server Only")
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
	After UMETA(DisplayName = "Once After")
};

/** A timer on the server; logic.rs gets a function named after it, which runs each time it fires. */
USTRUCT(BlueprintType)
struct CROWDYEXEC_API FCrowdyServerTimer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer")
	FName Name;

	/** Every Time, or once after Time. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer", meta = (DisplayName = "Runs"))
	ECrowdyServerTimerRepeat Repeat = ECrowdyServerTimerRepeat::Every;

	/** How long between runs, or before the one run. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timer", meta = (DisplayName = "Time", ClampMin = 0.01, Units = "s"))
	float Seconds = 60.f;

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
	OwnFile UMETA(DisplayName = "My Own File")
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

	/** How long one player waits between calls that succeeded; 0 for no wait. Checked on the server. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Function", meta = (DisplayName = "Cooldown", ClampMin = 0, Units = "s"))
	float CooldownSeconds = 0.f;

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

/** One Server Object type (a hub type on the platform): its state, the values players can watch, and the functions they can call. */
UCLASS(BlueprintType)
class CROWDYEXEC_API UCrowdyServerObjectDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The type's name on the server (the node type): lowercase letters, digits and underscores, starting with a letter. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object")
	FString TypeName;

	/** Every player, only the player who owns the instance, or only its members. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Readable By"))
	ECrowdyServerObjectVisibility Visibility = ECrowdyServerObjectVisibility::Public;

	/** One shared instance for every player, such as a registry or a world event; nothing picks an instance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Object", meta = (DisplayName = "Only One Instance"))
	bool bOnlyOneInstance = false;

	/** The Instance Id of a type with Only One Instance. */
	static const TCHAR* const OnlyInstanceId;

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

	/** What players, or other server code, can call on an instance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Server Functions")
	TArray<FCrowdyServerFunction> Functions;

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

#if WITH_EDITORONLY_DATA
	/** Other Server Object types this one's server code calls. Used when server code is generated and deployed; a game never loads them. */
	UPROPERTY(EditAnywhere, Category = "Can Call")
	TArray<TObjectPtr<UCrowdyServerObjectDefinition>> CanCall;
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

	UPROPERTY(EditAnywhere, Category = "Server Code")
	ECrowdyServerCodeSource CodeSource = ECrowdyServerCodeSource::Generated;

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

	/** Rebuilds the field tables from the structs, and each function's method name. On a problem it clears the tables and returns every problem found. */
	bool Bake(TArray<FString>& OutErrors);

	/** Builds the field tables without storing them. */
	bool BuildTables(TArray<FCrowdyExecBakedStruct>& OutStructs, TArray<FCrowdyExecBakedEnum>& OutEnums, TArray<FString>& OutErrors) const;

	/** The tables resolved against the loaded structs, built on first use and again after a struct changes. */
	const FCrowdyExecLayout* ResolveLayout(FString& OutError) const;

	/** The State: State, or StateList's struct. Null when none is chosen. */
	const UScriptStruct* GetStateStruct() const;

	/** The State's variable called Variable, ignoring case, spaces and punctuation; null when there is none that travels. */
	const FProperty* FindVariable(FName Variable) const;

	/** Out becomes Struct's starting values: a List's values as set here, otherwise the struct's defaults. Null Struct empties Out; a function's own values come from its InitializeParams or InitializeReply. */
	void InitializeValues(const UScriptStruct* Struct, FInstancedStruct& Out) const;

	/** The name a List's values go by in messages and server code, such as AddTipParams; empty when Struct is not one of this definition's Lists. */
	FString FindListName(const UScriptStruct* Struct) const;

	/** The struct of the List that FindListName calls Name. */
	const UScriptStruct* FindListStruct(const FString& Name) const;

	/** Every List in use, by the name FindListName gives it: the State's first, then each function's inputs and outputs. */
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
#endif

private:
	mutable TSharedPtr<FCrowdyExecLayout> Layout;
	mutable uint32 LayoutGeneration = 0;
};
