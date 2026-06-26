#include "CrowdyBlueprintCompilerExtension.h"

#include "Baking/CrowdyRegistryBaker.h"
#include "Core/UDP/Structures/FCrowdyEventContext.h"
#include "CrowdySDKEditor.h"
#include "CrowdyEditorEventMeta.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "KismetCompiler.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/CrowdyReplicatedEventLibrary.h"

namespace
{
	// Blueprint-added components live in the construction script of each
	// Blueprint class in the parent chain; native components live on the first
	// native ancestor's CDO. This runs mid-compile (ProcessBlueprintCompiled),
	// where the compiled class's own CDO does not exist yet — forcing its
	// creation here trips the blueprint compilation manager, so only CDOs that
	// already exist are consulted.
	static bool ClassHasEntityComponent(const UClass* Class)
	{
		for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
		{
			if (const UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(Current))
			{
				if (!BPClass->SimpleConstructionScript) continue;

				for (const USCS_Node* Node : BPClass->SimpleConstructionScript->GetAllNodes())
				{
					if (Node && Node->ComponentClass && Node->ComponentClass->IsChildOf<UCrowdyEntityComponent>())
						return true;
				}
			}
			else
			{
				// First native ancestor — its CDO exists without compiling anything
				const AActor* CDO = Cast<AActor>(Current->GetDefaultObject(false));
				return CDO && CDO->FindComponentByClass<UCrowdyEntityComponent>() != nullptr;
			}
		}

		return false;
	}

	// Mirrors UCrowdyEventRouter::ResolveHandlerSignature — valid handlers take
	// (payload struct) or (payload struct, FCrowdyEventContext). Returns an empty
	// string when the signature is valid, otherwise a description of the problem.
	static FString DescribeHandlerSignatureProblem(const UFunction* Function)
	{
		const FStructProperty* PayloadProp = nullptr;
		int32 ParamIndex = 0;

		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_Parm))      continue;
			if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

			const FStructProperty* StructProp = CastField<FStructProperty>(*It);
			if (!StructProp)
			{
				return FString::Printf(
					TEXT("parameter '%s' must be a struct"), *It->GetName());
			}

			switch (ParamIndex)
			{
			case 0:
				PayloadProp = StructProp;
				break;
			case 1:
				if (StructProp->Struct != FCrowdyEventContext::StaticStruct())
				{
					return FString::Printf(
						TEXT("second parameter '%s' must be a Crowdy Event Context"),
						*It->GetName());
				}
				break;
			default:
				return TEXT("too many parameters — expected (Payload) or (Payload, Crowdy Event Context)");
			}

			++ParamIndex;
		}

		if (!PayloadProp)
		{
			return TEXT("has no parameters — add a struct input (the event payload)");
		}

		// Handling the struct here is what registers it as an event payload —
		// no struct tagging required.
		return FString();
	}

	bool HasReplicatedCompileError(const UFunction* Function)
	{
		return Function && Function->HasAnyFunctionFlags(
			FUNC_Private | FUNC_Protected | FUNC_NetFuncFlags);
	}

	// The routing an author picked on a "Crowdy Replicates" event, copied off the node so the
	// generated function can be stamped with it. Empty strings keep the runtime defaults.
	struct FCrowdyReplicatedEventMeta
	{
		FString Recipient;
		FString Decay;
		FString Distance;
		FString Channel;
	};

	// Returns an empty string when the function is a valid replicated event, otherwise a
	// description of the problem. The parameter rules (one-way, serializable types) are the
	// same ones the runtime scan enforces, so both report identically from a single source;
	// Blueprint-only access and Unreal replication clashes are checked here.
	FString DescribeReplicatedSignatureProblem(const UFunction* Function)
	{
		if (Function->HasAnyFunctionFlags(FUNC_Private))
		{
			return TEXT("must be public; private Blueprint events cannot be used with Crowdy Replicates");
		}

		if (Function->HasAnyFunctionFlags(FUNC_Protected))
		{
			return TEXT("must be public; protected Blueprint events cannot be used with Crowdy Replicates");
		}

		if (Function->HasAnyFunctionFlags(FUNC_NetFuncFlags))
		{
			return TEXT("cannot also use Unreal replication — disable Run On Server/Client/Multicast");
		}

		return FCrowdyRPC::DescribeSignatureProblem(Function);
	}

	void ApplyReplicatedMeta(UFunction* Function, const FCrowdyReplicatedEventMeta& Info)
	{
		// CrowdyEvent makes the function discoverable/bakeable as a receiver; CrowdyReplicates
		// marks it as RPC-style so the router does not also bind it as a struct handler.
		Function->SetMetaData(CrowdyMetaKeys::CrowdyEvent, TEXT(""));
		Function->SetMetaData(CrowdyRpcMetaKeys::Replicates, TEXT(""));
		Function->RemoveMetaData(CrowdyRpcMetaKeys::LegacyReplicate);
		if (!Info.Recipient.IsEmpty()) Function->SetMetaData(CrowdyRpcMetaKeys::Recipient, *Info.Recipient);
		if (!Info.Decay.IsEmpty())     Function->SetMetaData(CrowdyRpcMetaKeys::Decay, *Info.Decay);
		if (!Info.Distance.IsEmpty())  Function->SetMetaData(CrowdyRpcMetaKeys::Distance, *Info.Distance);
		if (!Info.Channel.IsEmpty())   Function->SetMetaData(CrowdyRpcMetaKeys::Channel, *Info.Channel);
	}

	void ApplyReplicatedMeta(FKismetUserDeclaredFunctionMetadata& Meta, const FCrowdyReplicatedEventMeta& Info)
	{
		Meta.SetMetaData(CrowdyMetaKeys::CrowdyEvent, FString());
		Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Replicates), FString());
		Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::LegacyReplicate));
		if (!Info.Recipient.IsEmpty()) Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Recipient), Info.Recipient);
		if (!Info.Decay.IsEmpty())     Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Decay), Info.Decay);
		if (!Info.Distance.IsEmpty())  Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Distance), Info.Distance);
		if (!Info.Channel.IsEmpty())   Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Channel), Info.Channel);
	}

	// Splices the dispatch gate into a marked event's intermediate graph: Entry -> Gate -> Branch.
	// The gate routes the call over the network and returns true (skip the body) or recognises a
	// replay and returns false (run the body). The event's original body is rewired onto the
	// branch's false exec; the true exec is left open so an originated call returns without running
	// it. The gate reads the event's parameters from its own live frame, so no pins are wired.
	bool InjectReplicateGate(FCompilerResultsLog& MessageLog, UEdGraph* Graph, UK2Node_FunctionEntry* Entry)
	{
		if (!Graph || !Entry) return false;

		UEdGraphPin* EntryThen = Entry->GetThenPin();
		if (!EntryThen) return false;

		// Capture the body the entry currently leads to before the link is broken.
		const TArray<UEdGraphPin*> BodyTargets = EntryThen->LinkedTo;

		UK2Node_CallFunction* GateNode = NewObject<UK2Node_CallFunction>(Graph);
		Graph->AddNode(GateNode, /*bFromUI*/false, /*bSelectNewNode*/false);
		MessageLog.NotifyIntermediateObjectCreation(GateNode, Entry);
		GateNode->CreateNewGuid();
		GateNode->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UCrowdyReplicatedEventLibrary, CrowdyDispatchReplicatedEvent),
			UCrowdyReplicatedEventLibrary::StaticClass());
		GateNode->AllocateDefaultPins();

		UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
		Graph->AddNode(BranchNode, /*bFromUI*/false, /*bSelectNewNode*/false);
		MessageLog.NotifyIntermediateObjectCreation(BranchNode, Entry);
		BranchNode->CreateNewGuid();
		BranchNode->AllocateDefaultPins();

		UEdGraphPin* GateExec = GateNode->GetExecPin();
		UEdGraphPin* GateThen = GateNode->GetThenPin();
		UEdGraphPin* GateReturn = GateNode->GetReturnValuePin();
		UEdGraphPin* BranchExec = BranchNode->GetExecPin();
		UEdGraphPin* BranchCondition = BranchNode->GetConditionPin();
		UEdGraphPin* BranchElse = BranchNode->GetElsePin();

		if (!GateExec || !GateThen || !GateReturn || !BranchExec || !BranchCondition || !BranchElse)
		{
			return false;
		}

		EntryThen->BreakAllPinLinks();
		EntryThen->MakeLinkTo(GateExec);
		GateThen->MakeLinkTo(BranchExec);
		GateReturn->MakeLinkTo(BranchCondition);

		// False (not routed -> replay): run the original body. True (routed) is left unconnected,
		// so an originated call returns without executing the body locally.
		for (UEdGraphPin* BodyTarget : BodyTargets)
		{
			if (BodyTarget)
			{
				BranchElse->MakeLinkTo(BodyTarget);
			}
		}

		return true;
	}
}

void UCrowdyBlueprintCompilerExtension::ProcessBlueprintCompiled(
	const FKismetCompilerContext& CompilationContext,
	const FBlueprintCompiledData& Data)
{
	UBlueprint* Blueprint = CompilationContext.Blueprint;
	if (!Blueprint) return;

	// An actor that carries a Crowdy entity component is implicitly an entity, so the router's
	// ScanExistingObjects finds it via HasMetaData at runtime. The entity component is the only
	// opt-in — there is no separate class flag.
	if (UClass* NewClass = CompilationContext.NewClass)
	{
		const bool bHasEntityComponent = NewClass->IsChildOf<AActor>() && ClassHasEntityComponent(NewClass);

		if (bHasEntityComponent)
		{
			NewClass->SetMetaData(CrowdyMetaKeys::CrowdyEntity, TEXT(""));
		}
		else
		{
			NewClass->RemoveMetaData(CrowdyMetaKeys::CrowdyEntity);
		}
	}

	TArray<UEdGraph*> SourceGraphs;
	Blueprint->GetAllGraphs(SourceGraphs);

	// ── Crowdy Replicates (RPC-style) events ─────────────────────────────────
	// Collect the events the author flagged and the routing they chose, then stamp the matching
	// generated function and splice in the dispatch gate so calling the event replicates it.
	TMap<FName, FCrowdyReplicatedEventMeta> ReplicatedEvents;
	for (UEdGraph* Graph : SourceGraphs)
	{
		if (!Graph) continue;

		TArray<UK2Node_CustomEvent*> CustomEvents;
		Graph->GetNodesOfClass(CustomEvents);

		for (UK2Node_CustomEvent* CustomEvent : CustomEvents)
		{
			if (!CustomEvent) continue;
			if (CustomEvent->CustomFunctionName == NAME_None) continue;

			const FKismetUserDeclaredFunctionMetadata& NodeMeta = CustomEvent->GetUserDefinedMetaData();
			if (!HasCrowdyReplicatesMeta(NodeMeta)) continue;

			// A custom event's access specifier is stored on the node, not copied onto the
			// generated function (CreateFunctionStubForEvent never forwards it to the stub's
			// entry node), so the generated-function check below cannot see it. Enforce public
			// access here off the same node flags the details panel reads, and hard-fail the
			// compile when it isn't — a private/protected event silently never replicates otherwise.
			if ((CustomEvent->FunctionFlags & (FUNC_Private | FUNC_Protected)) != 0)
			{
				CompilationContext.MessageLog.Error(
					TEXT("[CrowdySDK] Crowdy Replicated event @@ must have its Access Specifier set to Public; ")
					TEXT("private and protected events cannot replicate."),
					CustomEvent);
			}

			FCrowdyReplicatedEventMeta Info;
			if (NodeMeta.HasMetaData(FName(CrowdyRpcMetaKeys::Recipient)))
				Info.Recipient = NodeMeta.GetMetaData(FName(CrowdyRpcMetaKeys::Recipient));
			if (NodeMeta.HasMetaData(FName(CrowdyRpcMetaKeys::Decay)))
				Info.Decay = NodeMeta.GetMetaData(FName(CrowdyRpcMetaKeys::Decay));
			if (NodeMeta.HasMetaData(FName(CrowdyRpcMetaKeys::Distance)))
				Info.Distance = NodeMeta.GetMetaData(FName(CrowdyRpcMetaKeys::Distance));
			if (NodeMeta.HasMetaData(FName(CrowdyRpcMetaKeys::Channel)))
				Info.Channel = NodeMeta.GetMetaData(FName(CrowdyRpcMetaKeys::Channel));

			ReplicatedEvents.Add(CustomEvent->CustomFunctionName, Info);
		}
	}

	if (!ReplicatedEvents.IsEmpty())
	{
		for (UEdGraph* IntermediateGraph : Data.IntermediateGraphs)
		{
			if (!IntermediateGraph) continue;

			TArray<UK2Node_FunctionEntry*> FunctionEntries;
			IntermediateGraph->GetNodesOfClass(FunctionEntries);

			for (UK2Node_FunctionEntry* Entry : FunctionEntries)
			{
				const FName FunctionName = GetFunctionEntryName(Entry);
				const FCrowdyReplicatedEventMeta* Info = ReplicatedEvents.Find(FunctionName);
				if (!Info) continue;

				ApplyReplicatedMeta(Entry->MetaData, *Info);

				if (UClass* NewClass = CompilationContext.NewClass)
				{
					if (UFunction* Function = NewClass->FindFunctionByName(FunctionName))
					{
						ApplyReplicatedMeta(Function, *Info);

						const FString Problem = DescribeReplicatedSignatureProblem(Function);
						if (!Problem.IsEmpty())
						{
							const FString Message = FString::Printf(
								TEXT("[CrowdySDK] Replicated event '%s' %s."),
								*Function->GetName(), *Problem);

							if (HasReplicatedCompileError(Function))
							{
								CompilationContext.MessageLog.Error(*Message);
							}
							else
							{
								CompilationContext.MessageLog.Warning(*FString::Printf(
									TEXT("%s It will not replicate correctly."),
									*Message));
							}
						}
					}
				}

				if (!InjectReplicateGate(CompilationContext.MessageLog, IntermediateGraph, Entry))
				{
					CompilationContext.MessageLog.Warning(*FString::Printf(
						TEXT("[CrowdySDK] Could not install the replication gate for event '%s'; it will not auto-replicate."),
						*FunctionName.ToString()));
				}
			}
		}
	}

	// Validate every stamped handler on the freshly compiled class so a bad
	// signature surfaces as a compile warning instead of a silent runtime no-op.
	// Covers both function-graph handlers (stamped pre-compile) and the custom
	// events stamped above. Parent-class handlers were validated when the parent
	// compiled, so supers are excluded.
	if (UClass* NewClass = CompilationContext.NewClass)
	{
		for (TFieldIterator<UFunction> It(NewClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			UFunction* Function = *It;
			if (!Function->HasMetaData(CrowdyMetaKeys::CrowdyEvent)) continue;

			// Replicated (RPC) events also carry CrowdyEvent but are validated by the
			// replicated-signature check above, not the struct-handler rules here.
			if (CrowdyRpcMetaKeys::HasReplicatesMeta(Function)) continue;

			const FString Problem = DescribeHandlerSignatureProblem(Function);
			if (Problem.IsEmpty()) continue;

			CompilationContext.MessageLog.Warning(*FString::Printf(
				TEXT("[CrowdySDK] Event handler '%s' %s. It will never be invoked at runtime."),
				*Function->GetName(), *Problem));
		}

		// Keep the cooked registry in step with the just-stamped metadata so the
		// packaged build discovers this class. Marks the asset dirty; the next
		// save (or a full 'Rebuild Crowdy Registry') persists it.
		UCrowdyRegistryBaker::UpdateForClass(NewClass);

		// And keep any live (PIE) runtime registry in step, incrementally. Recording the class
		// here lets OnBlueprintCompiled refresh just this class at batch end instead of resweeping
		// every loaded class — the fix for the per-compile editor hitch.
		FCrowdySDKEditorModule::NotePendingRpcRescan(NewClass);
	}
}
