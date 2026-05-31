#include "CrowdyBlueprintCompilerExtension.h"

#include "CrowdySDKEditor.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"

namespace
{
	static FName GetFunctionEntryName(const UK2Node_FunctionEntry* Entry)
	{
		if (!Entry) return NAME_None;

		return Entry->CustomGeneratedFunctionName != NAME_None
			? Entry->CustomGeneratedFunctionName
			: Entry->FunctionReference.GetMemberName();
	}
}

void UCrowdyBlueprintCompilerExtension::ProcessBlueprintCompiled(
	const FKismetCompilerContext& CompilationContext,
	const FBlueprintCompiledData& Data)
{
	UBlueprint* Blueprint = CompilationContext.Blueprint;
	if (!Blueprint) return;

	TSet<FName> StampedCustomEvents;

	TArray<UEdGraph*> SourceGraphs;
	Blueprint->GetAllGraphs(SourceGraphs);

	for (UEdGraph* Graph : SourceGraphs)
	{
		if (!Graph) continue;

		TArray<UK2Node_CustomEvent*> CustomEvents;
		Graph->GetNodesOfClass(CustomEvents);

		for (UK2Node_CustomEvent* CustomEvent : CustomEvents)
		{
			if (!CustomEvent) continue;
			if (CustomEvent->CustomFunctionName == NAME_None) continue;
			if (!CustomEvent->GetUserDefinedMetaData().HasMetaData(
				CrowdyMetaKeys::CrowdyEvent))
			{
				continue;
			}

			StampedCustomEvents.Add(CustomEvent->CustomFunctionName);
		}
	}

	if (StampedCustomEvents.IsEmpty()) return;

	for (UEdGraph* IntermediateGraph : Data.IntermediateGraphs)
	{
		if (!IntermediateGraph) continue;

		TArray<UK2Node_FunctionEntry*> FunctionEntries;
		IntermediateGraph->GetNodesOfClass(FunctionEntries);

		for (UK2Node_FunctionEntry* Entry : FunctionEntries)
		{
			const FName FunctionName = GetFunctionEntryName(Entry);
			if (FunctionName == NAME_None) continue;
			if (!StampedCustomEvents.Contains(FunctionName)) continue;

			Entry->MetaData.SetMetaData(
				CrowdyMetaKeys::CrowdyEvent,
				FString());

			if (UClass* NewClass = CompilationContext.NewClass)
			{
				if (UFunction* Function =
					NewClass->FindFunctionByName(FunctionName))
				{
					Function->SetMetaData(
						CrowdyMetaKeys::CrowdyEvent,
						TEXT(""));
				}
			}

			UE_LOG(LogCrowdyEditor, Verbose,
				TEXT("[CrowdySDK] Propagated Custom Event CrowdyEvent stamp to '%s'"),
				*FunctionName.ToString());
		}
	}
}
