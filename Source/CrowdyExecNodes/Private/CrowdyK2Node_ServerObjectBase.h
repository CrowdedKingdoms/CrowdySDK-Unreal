#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecNodesSpawn.h"
#include "CrowdyServerObjectPins.h"
#include "CrowdyServerObjectTypes.h"
#include "K2Node.h"
#include "CrowdyK2Node_ServerObjectBase.generated.h"

class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class UCrowdyK2Node_ServerObjectBase;
class UCrowdyServerObjectDefinition;
class UEdGraph;

/** One entry the action menu offers for a definition asset. */
struct FCrowdyServerObjectMenuEntry
{
	TSubclassOf<UCrowdyK2Node_ServerObjectBase> NodeClass;
	FName Member;
	FGuid MemberId;
	FText Title;
};

/** What every Server Object node shares: its definition, where the object comes from, its menu entries and rebuilding on edits. */
UCLASS(Abstract)
class UCrowdyK2Node_ServerObjectBase : public UK2Node
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Server Object")
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	/** Finds the object by its definition and Instance instead of taking a Target, so nothing needs to be passed in. */
	UPROPERTY(EditAnywhere, Category = "Server Object", meta = (DisplayName = "Find By Asset"))
	bool bFind = false;

	/** Which instance Find By Asset follows. */
	UPROPERTY(EditAnywhere, Category = "Server Object", meta = (EditCondition = "bFind", EditConditionHides))
	ECrowdyServerObjectFind Instance = ECrowdyServerObjectFind::InstanceId;

	/** The variable or function this node is about. */
	UPROPERTY(VisibleAnywhere, Category = "Server Object")
	FName Member;

	/** The variable's id in a List or Blueprint struct, which keeps the node on it when it is renamed. */
	UPROPERTY()
	FGuid MemberId;

	/** Definition, loaded if a Blueprint compiling on load reaches it first. */
	UCrowdyServerObjectDefinition* GetDefinition() const;

	/** Find By Asset is on, or the definition has Only One Instance and so nothing picks one. */
	bool UsesFind() const;

	/** Whether Class can be a Target: a Server Object, a Crowdy Server Object component, an actor or a Server Object Link. */
	static bool IsTargetClass(const UClass* Class);

	/** Definition's functions, with the built-in member functions when it keeps its own members. */
	static void GatherFunctionNames(const UCrowdyServerObjectDefinition* Definition, TArray<FName>& Out);

	/** The menu entries for Definition: Get and On Changed per variable, Call per function, and Get Server State. */
	static void GatherMenuEntries(const UCrowdyServerObjectDefinition* Definition, TArray<FCrowdyServerObjectMenuEntry>& Out);

	//~ UEdGraphNode
	virtual void PostLoad() override;
	virtual void PostPlacedNewNode() override;
	virtual void PostPasteNode() override;
	virtual void BeginDestroy() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void GetNodeContextMenuActions(class UToolMenu* Menu, class UGraphNodeContextMenuContext* Context) const override;
	virtual bool IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const override;
	//~ End UEdGraphNode

	//~ UK2Node
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual bool HasExternalDependencies(TArray<UStruct*>* OptionalOutput = nullptr) const override;
	virtual ERedirectType DoPinsMatchForReconstruction(const UEdGraphPin* NewPin, int32 NewPinIndex, const UEdGraphPin* OldPin, int32 OldPinIndex) const override;
	//~ End UK2Node

protected:
	/** The Target pin, or Find's Instance pins; none for Only One Instance. */
	void CreateSourcePins();

	/** Value's pin, and its Has Value pin for an optional. Null when Value has no pin. */
	UEdGraphPin* CreateValuePins(EEdGraphPinDirection Direction, const FCrowdyServerValuePin& Value);

	/** An advanced Boolean output, false until the server has sent the object's values. */
	UEdGraphPin* CreateReadyPin(FName Name, const FGuid& Id);

	/** An output hidden until the node's advanced pins are expanded. */
	UEdGraphPin* CreateAdvancedOutput(FName Category, UObject* SubCategoryObject, FName Name, const FText& Tooltip);

	/** Feeds the object's source into each of Inputs, Target-typed pins of intermediate calls. False after reporting why. */
	bool ExpandSource(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, TConstArrayView<UEdGraphPin*> Inputs);

	/** Moves Pin's wires onto To, reporting a wire that no longer fits Pin's type instead of dropping it. */
	bool MoveValueLinks(FKismetCompilerContext& CompilerContext, UEdGraphPin& Pin, UEdGraphPin& To);

	/** Reports Value's refusal as a compile error; false when it has one. */
	bool CheckValue(FKismetCompilerContext& CompilerContext, const FCrowdyServerValuePin& Value) const;

	/** Reports a node with no definition; false when it has none. */
	bool CheckDefinition(FKismetCompilerContext& CompilerContext) const;

	/** The node's variable into Out, or false after reporting a missing definition, a missing variable or a refusal. */
	bool CheckVariable(FKismetCompilerContext& CompilerContext, FCrowdyServerValuePin& Out) const;

	/** The variable Member and MemberId name, following a rename by id. False when the definition has no such variable. */
	bool FindMemberVariable(FCrowdyServerValuePin& Out) const;

	/** Member and MemberId follow the variable they name, so a rename shows in the title. */
	void RefreshMemberVariable();

	FText GetAssetName() const;

	/** Rebuilds the pins and marks the Blueprint as needing a compile, so play never runs the old shape. */
	void ReconstructAndMarkModified();

private:
	bool ReportSourceConnected(FKismetCompilerContext& CompilerContext, bool bConnected) const;
	void ToggleFind();
	void RegisterDefinitionListener();
	void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);

	FDelegateHandle OnPropertyChangedHandle;
};
