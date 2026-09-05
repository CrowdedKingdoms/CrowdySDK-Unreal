#include "CrowdyBlueprintCompilerExtension.h"

#include "Compiler/CrowdyAuthoringContributions.h"
#include "Compiler/CrowdyBlueprintCompileHooks.h"
#include "Components/ActorComponent.h"
#include "Core/UDP/Structures/FCrowdyEventContext.h"
#include "CrowdyEditorEventMeta.h"
#include "CrowdyNodesLog.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "KismetCompiler.h"
#include "Nodes/CrowdyApplyEffectContainerResolver.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/CrowdyMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyModelAttributeLookup.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/CrowdyReplicatedEventLibrary.h"
#include "Replication/State/CrowdyStateMetaKeys.h"
#include "Replication/State/FCrowdyRepLayout.h"

namespace
{
	// Blueprint-added components live in the construction script of each
	// Blueprint class in the parent chain; native components live on the first
	// native ancestor's CDO. This runs mid-compile (ProcessBlueprintCompiled),
	// where the compiled class's own CDO does not exist yet - forcing its
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
				// First native ancestor - its CDO exists without compiling anything
				const AActor* CDO = Cast<AActor>(Current->GetDefaultObject(false));
				return CDO && CDO->FindComponentByClass<UCrowdyEntityComponent>() != nullptr;
			}
		}

		return false;
	}

	// How far a chain of macros-inside-macros the authority sweep follows. Engine macros nest a
	// level or two; the bound only exists so a pathological macro library cannot stall a compile.
	constexpr int32 MaxAuthorityScanDepth = 8;

	// Reroute nodes can be chained without limit, so the walk back to whatever really drives a
	// Target pin is bounded too.
	constexpr int32 MaxTargetRerouteHops = 32;

	// Reports every self-targeted Unreal Has Authority call reachable from Graph. AuthoredNode is
	// the node the author actually placed: null while scanning the author's own graph (each call
	// node reports itself), and the macro instance node once the walk has descended into a macro
	// defined in another asset, whose inner nodes the author cannot click. VisitedGraphs is
	// pre-seeded with the Blueprint's own graphs, so a macro the Blueprint itself defines reports at
	// the call node inside it rather than a second time at each instance; it also breaks cycles
	// between macros that instance each other.
	void ReportForbiddenAuthorityCalls(
		FCompilerResultsLog& MessageLog,
		UEdGraph* Graph,
		UEdGraphNode* AuthoredNode,
		int32 Depth,
		TSet<const UEdGraph*>& VisitedGraphs)
	{
		if (!Graph || Depth > MaxAuthorityScanDepth) return;

		TArray<UK2Node_CallFunction*> CallNodes;
		Graph->GetNodesOfClass(CallNodes);

		for (UK2Node_CallFunction* CallNode : CallNodes)
		{
			if (!CallNode) continue;
			if (!UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(CallNode->GetTargetFunction())) continue;

			// A "call function on member" node names its target on the node itself and leaves the
			// Target pin empty, so an empty pin there does not mean the call is about this actor.
			if (CallNode->IsA<UK2Node_CallFunctionOnMember>()) continue;

			// A call aimed at some other actor is a legitimate question about that actor's Unreal
			// network role; only "am I the authority for myself" is the wrong question here.
			if (!UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(
					CallNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input)))
			{
				continue;
			}

			MessageLog.Error(
				TEXT("[CrowdySDK] @@ uses Unreal's Has Authority, which does not apply to a Crowdy entity - ")
				TEXT("the elected host is a convention, not an enforced server, so this reads the wrong value ")
				TEXT("on non-host clients. Use the Crowdy ownership test instead: ")
				TEXT("UCrowdyEntityComponent::IsLocallyOwned, or UCrowdyUtilities::IsCrowdyEntityLocallyControlled."),
				AuthoredNode ? AuthoredNode : static_cast<UEdGraphNode*>(CallNode));
		}

		TArray<UK2Node_MacroInstance*> MacroNodes;
		Graph->GetNodesOfClass(MacroNodes);

		for (UK2Node_MacroInstance* MacroNode : MacroNodes)
		{
			if (!MacroNode) continue;

			UEdGraph* MacroGraph = MacroNode->GetMacroGraph();
			if (!MacroGraph) continue;

			bool bAlreadyVisited = false;
			VisitedGraphs.Add(MacroGraph, &bAlreadyVisited);
			if (bAlreadyVisited) continue;

			ReportForbiddenAuthorityCalls(
				MessageLog,
				MacroGraph,
				AuthoredNode ? AuthoredNode : static_cast<UEdGraphNode*>(MacroNode),
				Depth + 1,
				VisitedGraphs);
		}
	}

	// How far a chain of collapsed graphs and macros-inside-macros the crowd-body sweep follows. Same
	// reason as the authority sweep's bound above.
	constexpr int32 MaxCrowdBodyScanDepth = 8;

	// Follows a pin's single wire back past any reroute nodes to the node that really drives it. Null
	// whenever nothing identifiable does: an unconnected pin, a chain ending in nothing, a reroute
	// missing a pin, or a chain longer than the hop bound.
	//
	// Deliberately not folded together with IsSelfTargetedCallPin, which walks the same chain to answer
	// a different question and treats those ambiguous cases as "not self" so it stays silent rather than
	// failing a compile. Sharing one walk would change that behaviour.
	const UEdGraphNode* ResolveDrivingNode(const UEdGraphPin* Pin)
	{
		if (!Pin || Pin->LinkedTo.Num() == 0) return nullptr;

		const UEdGraphPin* Source = Pin->LinkedTo[0];

		for (int32 Hop = 0; Hop < MaxTargetRerouteHops && Source; ++Hop)
		{
			const UEdGraphNode* SourceNode = Source->GetOwningNodeUnchecked();
			if (!SourceNode) return nullptr;

			const UK2Node_Knot* Reroute = Cast<UK2Node_Knot>(SourceNode);
			if (!Reroute) return SourceNode;

			// GetInputPin indexes Pins unguarded, so only ask a reroute that has both of its pins.
			if (Reroute->Pins.Num() < 2) return nullptr;

			const UEdGraphPin* RerouteInput = Reroute->GetInputPin();
			Source = (RerouteInput && RerouteInput->LinkedTo.Num() > 0) ? RerouteInput->LinkedTo[0] : nullptr;
		}

		return nullptr;
	}

	bool IsActorComponentPin(const UEdGraphPin* Pin)
	{
		if (!Pin) return false;

		const UClass* PinClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
		return PinClass && PinClass->IsChildOf(UActorComponent::StaticClass());
	}

	// True for the Set Timer family. A timer is not a latent node and carries no Latent metadata, so
	// nothing else here would recognise it, yet it is the same problem: the work happens after the body
	// has returned. Matched by owner and name prefix, which covers all four spellings (by function name
	// or by event, this tick or later).
	bool IsTimerCall(const UFunction* Function)
	{
		return Function
			&& Function->GetOwnerClass() == UKismetSystemLibrary::StaticClass()
			&& Function->GetName().StartsWith(TEXT("K2_SetTimer"));
	}

	// Reports every node of one Crowdy multicast body that cannot do its work on a client drawing the
	// entity as a crowd row, and descends into the collapsed graphs and macros the body runs through.
	//
	// The body is collected by walking forward along execution rather than by sweeping the graph,
	// because these limits belong to this body alone: the same call in an ordinary function, or in an
	// owning-client event, is correct. Inside a collapsed graph or a macro the whole graph is swept
	// instead of the tunnels being traced, exactly as the authority sweep does; that can report a node
	// on a branch this body never reaches, which is the cheaper mistake when the alternative is missing
	// a Delay inside a macro.
	//
	// AuthoredNode is the node the author can actually click: null while walking their own graph, and
	// the macro instance once the walk has descended into a macro defined in another asset.
	void ReportCrowdBodyLimits(
		FCompilerResultsLog& MessageLog,
		const FString& EventName,
		const TArray<UEdGraphNode*>& BodyNodes,
		UEdGraphNode* AuthoredNode,
		int32 Depth,
		TSet<const UEdGraph*>& VisitedGraphs)
	{
		if (Depth > MaxCrowdBodyScanDepth) return;

		for (UEdGraphNode* Node : BodyNodes)
		{
			if (!Node) continue;

			const ECrowdyCrowdBodyLimit Limit = UCrowdyBlueprintCompilerExtension::ClassifyCrowdBodyNode(Node);
			if (Limit != ECrowdyCrowdBodyLimit::None)
			{
				MessageLog.Warning(
					*FString::Printf(
						TEXT("[CrowdySDK] @@ in Crowdy multicast event '%s' %s"),
						*EventName,
						*UCrowdyBlueprintCompilerExtension::DescribeCrowdBodyLimit(Limit)),
					AuthoredNode ? AuthoredNode : Node);
			}

			TArray<UEdGraph*> Subgraphs = Node->GetSubGraphs();
			if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
			{
				if (UEdGraph* MacroGraph = Macro->GetMacroGraph())
				{
					Subgraphs.AddUnique(MacroGraph);
				}
			}

			// A collapsed graph belongs to this Blueprint, so its nodes are the author's own and stay
			// clickable; a macro's live in another asset, so those report at the instance node instead.
			const bool bAuthorOwnsSubgraphNodes = !Node->IsA<UK2Node_MacroInstance>();
			UEdGraphNode* SubAuthoredNode = bAuthorOwnsSubgraphNodes
				? AuthoredNode
				: (AuthoredNode ? AuthoredNode : Node);

			for (UEdGraph* Subgraph : Subgraphs)
			{
				if (!Subgraph) continue;

				bool bAlreadyVisited = false;
				VisitedGraphs.Add(Subgraph, &bAlreadyVisited);
				if (bAlreadyVisited) continue;

				ReportCrowdBodyLimits(
					MessageLog, EventName, Subgraph->Nodes, SubAuthoredNode, Depth + 1, VisitedGraphs);
			}
		}
	}

	// Whether this event node's body is one that runs on clients which may be drawing the entity as a
	// crowd row.
	//
	// A custom event declared in the Blueprint being compiled is read off the NODE's own user metadata,
	// because the generated function is not stamped with the Crowdy markers until later in this same
	// compile and so answers nothing here. An override of an event declared on a parent, in C++ or in a
	// parent Blueprint, is read off that parent's function, which already exists and is already stamped.
	bool IsCrowdDeliveredEventNode(UK2Node_Event* EventNode)
	{
		if (!EventNode) return false;

		if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(EventNode))
		{
			const FKismetUserDeclaredFunctionMetadata& NodeMeta = CustomEvent->GetUserDefinedMetaData();
			return HasCrowdyReplicatesMeta(NodeMeta)
				&& UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ResolveCrowdyRecipient(NodeMeta));
		}

		const UFunction* Signature = EventNode->FindEventSignatureFunction();
		return CrowdyRpcMetaKeys::HasReplicatesMeta(Signature)
			&& UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(
				ResolveCrowdyRecipient(Signature->GetMetaData(CrowdyRpcMetaKeys::Recipient)));
	}

	// Mirrors UCrowdyEventRouter::ResolveHandlerSignature - valid handlers take
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
				return TEXT("too many parameters - expected (Payload) or (Payload, Crowdy Event Context)");
			}

			++ParamIndex;
		}

		if (!PayloadProp)
		{
			return TEXT("has no parameters - add a struct input (the event payload)");
		}

		// Handling the struct here is what registers it as an event payload -
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

		// Whether the author ticked Is A One-Shot Action. Valueless, so it is a flag here rather than a
		// string: what matters is that the key reaches the compiled function, since that is the only place
		// the runtime looks for it.
		bool bIsAction = false;
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
			return TEXT("cannot also use Unreal replication - disable Run On Server/Client/Multicast");
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

		// The one-shot marker has to land HERE, on the compiled function, because that is the only thing
		// the runtime reads: an author ticks it on the node, and a marker left on the node alone is read by
		// nobody. Removed when unticked, so unticking it takes effect rather than leaving a stale key on a
		// function that is recompiled in place.
		if (Info.bIsAction)
		{
			Function->SetMetaData(CrowdyRpcMetaKeys::Action, TEXT(""));
		}
		else
		{
			Function->RemoveMetaData(CrowdyRpcMetaKeys::Action);
		}
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

		if (Info.bIsAction)
		{
			Meta.SetMetaData(FName(CrowdyRpcMetaKeys::Action), FString());
		}
		else
		{
			Meta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Action));
		}
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

bool UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(const UFunction* Function)
{
	return Function
		&& Function->GetOwnerClass() == AActor::StaticClass()
		&& Function->GetFName() == GET_FUNCTION_NAME_CHECKED(AActor, HasAuthority);
}

bool UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(const UEdGraphPin* SelfPin)
{
	// No Target pin at all, or one the author left empty, is the implicit self context.
	if (!SelfPin || SelfPin->LinkedTo.Num() == 0) return true;

	// A Target pin accepts one wire; follow reroutes back to the node that really drives it.
	const UEdGraphPin* Source = SelfPin->LinkedTo[0];

	for (int32 Hop = 0; Hop < MaxTargetRerouteHops && Source; ++Hop)
	{
		const UEdGraphNode* SourceNode = Source->GetOwningNodeUnchecked();
		if (!SourceNode) return false;

		if (SourceNode->IsA<UK2Node_Self>()) return true;

		// GetInputPin indexes Pins unguarded, so only ask a reroute that has both of its pins.
		const UK2Node_Knot* Reroute = Cast<UK2Node_Knot>(SourceNode);
		if (!Reroute || Reroute->Pins.Num() < 2) return false;

		const UEdGraphPin* RerouteInput = Reroute->GetInputPin();
		Source = (RerouteInput && RerouteInput->LinkedTo.Num() > 0) ? RerouteInput->LinkedTo[0] : nullptr;
	}

	// A reroute chain fed by nothing drives nothing, so the Target falls back to self. A chain
	// longer than the hop bound is left alone rather than guessed at.
	return Source == nullptr;
}

void UCrowdyBlueprintCompilerExtension::CollectExecutionBody(UEdGraphNode* StartNode, TArray<UEdGraphNode*>& OutNodes)
{
	if (!StartNode) return;

	TSet<UEdGraphNode*> Seen;
	TArray<UEdGraphNode*> Pending;
	Pending.Add(StartNode);
	Seen.Add(StartNode);

	while (Pending.Num() > 0)
	{
		const UEdGraphNode* Node = Pending.Pop();

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output) continue;
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				UEdGraphNode* Next = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
				if (!Next || Seen.Contains(Next)) continue;

				Seen.Add(Next);
				Pending.Add(Next);
				OutNodes.Add(Next);
			}
		}
	}
}

bool UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(const ECrowdyEventRecipient Recipient)
{
	return Recipient == ECrowdyEventRecipient::SpatialMulticast
		|| Recipient == ECrowdyEventRecipient::Multicast;
}

bool UCrowdyBlueprintCompilerExtension::DeclaresOneShotAction(const FKismetUserDeclaredFunctionMetadata& NodeMeta)
{
	// Valueless: the key being present is the whole of the declaration, exactly as meta=(CrowdyAction) is
	// in C++, so nothing here reads a value that could disagree with it.
	return NodeMeta.HasMetaData(FName(CrowdyRpcMetaKeys::Action));
}

ECrowdyCrowdBodyLimit UCrowdyBlueprintCompilerExtension::ClassifyCrowdBodyNode(const UEdGraphNode* Node)
{
	if (!Node) return ECrowdyCrowdBodyLimit::None;

	// Nodes whose whole purpose is to continue after the body has returned. Neither is a function call,
	// so neither is reachable through the call checks below.
	if (Node->IsA<UK2Node_Timeline>() || Node->IsA<UK2Node_BaseAsyncTask>())
	{
		return ECrowdyCrowdBodyLimit::ResumesLater;
	}

	const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
	if (!CallNode) return ECrowdyCrowdBodyLimit::None;

	const UFunction* TargetFunction = CallNode->GetTargetFunction();
	if (CallNode->IsLatentFunction() || IsTimerCall(TargetFunction))
	{
		return ECrowdyCrowdBodyLimit::ResumesLater;
	}

	// A "call function on member" node names its target member on the node itself and leaves the Target
	// pin empty, so the pin walk below cannot see it. The member's own class is what the called function
	// is declared on, which is what says the member is a component.
	if (const UK2Node_CallFunctionOnMember* MemberCall = Cast<UK2Node_CallFunctionOnMember>(Node))
	{
		const UClass* MemberClass = TargetFunction ? TargetFunction->GetOwnerClass() : nullptr;
		const bool bComponentOfSelf = MemberCall->MemberVariableToCallOn.IsSelfContext()
			&& MemberClass
			&& MemberClass->IsChildOf(UActorComponent::StaticClass());

		return bComponentOfSelf ? ECrowdyCrowdBodyLimit::ComponentOfSelf : ECrowdyCrowdBodyLimit::None;
	}

	// An ordinary call whose Target is wired from a Get of one of this Blueprint's own component
	// variables. Only a self-context variable counts: a component reached off some other actor belongs
	// to that actor, which is a real actor wherever it exists at all.
	const UEdGraphPin* TargetPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
	if (!IsActorComponentPin(TargetPin)) return ECrowdyCrowdBodyLimit::None;

	const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(ResolveDrivingNode(TargetPin));
	return (Getter && Getter->VariableReference.IsSelfContext())
		? ECrowdyCrowdBodyLimit::ComponentOfSelf
		: ECrowdyCrowdBodyLimit::None;
}

FString UCrowdyBlueprintCompilerExtension::DescribeCrowdBodyLimit(const ECrowdyCrowdBodyLimit Limit)
{
	switch (Limit)
	{
	case ECrowdyCrowdBodyLimit::ResumesLater:
		return TEXT("resumes its work after the event body returns. A client drawing this entity as a crowd row ")
			TEXT("runs the body on one stand-in shared by every entity of this class, and that stand-in is reset ")
			TEXT("and moved to the next entity as soon as the body returns, so the continuation would resume ")
			TEXT("against a stand-in for somebody else. Do the work in a single pass, or keep the entity a real ")
			TEXT("actor. It behaves as written everywhere the entity is a real actor, including on its owner.");

	case ECrowdyCrowdBodyLimit::ComponentOfSelf:
		return TEXT("calls a function on one of this actor's own components. A client drawing this entity as a ")
			TEXT("crowd row has no per-entity components: the crowd is drawn from fragments by processors, so the ")
			TEXT("call succeeds and changes nothing there. Present in world space at the actor's own location ")
			TEXT("instead (a particle system, a sound, a decal), or drive animation through a Crowdy action event. ")
			TEXT("It behaves as written everywhere the entity is a real actor, including on its owner.");

	default:
		return FString();
	}
}

void UCrowdyBlueprintCompilerExtension::ProcessBlueprintCompiled(
	const FKismetCompilerContext& CompilationContext,
	const FBlueprintCompiledData& Data)
{
	UBlueprint* Blueprint = CompilationContext.Blueprint;
	if (!Blueprint) return;

	bool bHasEntityComponent = false;

	// An actor that carries a Crowdy entity component is implicitly an entity, so the router's
	// ScanExistingObjects finds it via HasMetaData at runtime. The entity component is the only
	// opt-in - there is no separate class flag.
	if (UClass* NewClass = CompilationContext.NewClass)
	{
		bHasEntityComponent = NewClass->IsChildOf<AActor>() && ClassHasEntityComponent(NewClass);

		if (bHasEntityComponent)
		{
			NewClass->SetMetaData(CrowdyMetaKeys::CrowdyEntity, TEXT(""));
		}
		else
		{
			NewClass->RemoveMetaData(CrowdyMetaKeys::CrowdyEntity);
		}

		// The Game Model container tags (the type name and the pull-on-start answer) come from the
		// Blueprint's persisted container marker, which is defined in the editor module. This pass also runs
		// where that module is absent, so the stamp is supplied as a hook and is simply not applied there:
		// a game process has no marker to read and nothing that would consult the tags before the next
		// compile in an editor writes them.
		CrowdyBlueprintCompileHooks::StampContainerMetadata(Blueprint, NewClass);
	}

	TArray<UEdGraph*> SourceGraphs;
	Blueprint->GetAllGraphs(SourceGraphs);

	// Unreal's Has Authority answers a question about Unreal's own network role, which does not
	// exist for a Crowdy entity - the elected host is a convention, never an enforced server - so a
	// call to it reads a value that is meaningless (and usually wrong) on every non-host client. Hard
	// error rather than a warning, the same severity as the other silently-wrong-at-runtime checks
	// in this function.
	//
	// The sweep covers this Blueprint's own authored graphs plus the graphs of any macros they
	// instance. GetAllGraphs already walks every collapsed/composite subgraph the author created, so
	// a call tucked away in a collapsed graph is caught; the macro recursion is what catches Switch
	// Has Authority, which ships as a macro in a separate engine asset and so appears here as a
	// macro instance rather than a call node. The compiler's expanded IntermediateGraphs are
	// deliberately not swept: they also hold the compiler's own generated plumbing, including this
	// file's replication dispatch gate injected further down, so a sweep there risks flagging
	// transient nodes the author never placed and cannot click.
	if (bHasEntityComponent)
	{
		TSet<const UEdGraph*> VisitedGraphs;
		VisitedGraphs.Reserve(SourceGraphs.Num());
		for (const UEdGraph* Graph : SourceGraphs)
		{
			VisitedGraphs.Add(Graph);
		}

		for (UEdGraph* Graph : SourceGraphs)
		{
			ReportForbiddenAuthorityCalls(
				CompilationContext.MessageLog, Graph, /*AuthoredNode*/nullptr, /*Depth*/0, VisitedGraphs);
		}
	}

	// A Crowdy multicast body runs on every client, but not on the same kind of thing everywhere. A
	// client drawing this entity as a crowd row has no actor of the class to run it on, so it runs the
	// body on one stand-in per class instead, and two kinds of node quietly do nothing there. See
	// ECrowdyCrowdBodyLimit for what each of them is.
	//
	// A WARNING rather than an error, unlike the checks around it. Those are for things that are wrong
	// everywhere; this one is a difference between representations. The same body is correct wherever
	// the entity is a real actor, which is always the case on the machine that owns it, and whether an
	// observer draws it as a row is chosen by the map profile's rendering backend rather than by
	// anything the class can see. Failing the compile would forbid a body that works.
	//
	// Silent where no map profile selects a backend that draws entities as rows, because the stand-in these
	// describe does not exist there: the whole warning would be about a representation the project cannot
	// produce. This is the advisory half of the pass and is the only half that gates. The dispatch gate
	// spliced in below must keep running everywhere, uncooked game processes included, which is why it is
	// nowhere near this.
	//
	// Skipped entirely while the loader is regenerating this Blueprint: answering loads the map profile assets,
	// and a load there re-enters the compile already in flight. The warning is advisory and is raised in full on
	// the next compile the author asks for.
	if (bHasEntityComponent && !Blueprint->bIsRegeneratingOnLoad
		&& CrowdyAuthoringContributions::IsCrowdRepresentationSelected())
	{
		for (UEdGraph* Graph : SourceGraphs)
		{
			if (!Graph) continue;

			TArray<UK2Node_Event*> EventNodes;
			Graph->GetNodesOfClass(EventNodes);

			for (UK2Node_Event* EventNode : EventNodes)
			{
				if (!IsCrowdDeliveredEventNode(EventNode)) continue;

				TArray<UEdGraphNode*> BodyNodes;
				UCrowdyBlueprintCompilerExtension::CollectExecutionBody(EventNode, BodyNodes);

				// One visited set per event, not one for the whole sweep: two bodies that both run
				// through the same macro both have the problem, and each wants telling about it.
				TSet<const UEdGraph*> VisitedBodyGraphs;
				ReportCrowdBodyLimits(
					CompilationContext.MessageLog,
					EventNode->GetFunctionName().ToString(),
					BodyNodes,
					/*AuthoredNode*/nullptr,
					/*Depth*/0,
					VisitedBodyGraphs);
			}
		}
	}

	// Crowdy Replicates (RPC-style) events.
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
			// compile when it isn't - a private/protected event silently never replicates otherwise.
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
			Info.bIsAction = UCrowdyBlueprintCompilerExtension::DeclaresOneShotAction(NodeMeta);

			ReplicatedEvents.Add(CustomEvent->CustomFunctionName, Info);
		}
	}

	if (!ReplicatedEvents.IsEmpty())
	{
		// Which of the collected events actually reached a compiled entry and got their gate. An event that
		// reaches none is stamped with nothing and gated with nothing, and used to say so in no way at all:
		// see the report below the loop for why that silence is the worst outcome this file can produce.
		TSet<FName> GatedEvents;

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

						// An event ticked Is A One-Shot Action whose parameters cannot carry one is dead in
						// exactly the way nothing downstream can report: it replicates, its body runs on
						// every actor that has one, and the crowd side quietly starts no action. Asked here,
						// against the compiled signature, because that is the first moment the parameters
						// exist as the runtime will read them.
						//
						// A warning rather than an error: the event still replicates and still runs its
						// body, so the compile has produced a working class, and the box may simply have
						// been ticked before the parameters were added.
						if (Info->bIsAction)
						{
							// The routing is asked about first, because a signature verdict on an event no
							// crowd observer ever receives would be answering the wrong question. Owning
							// Client and Host land on one named machine, which holds the entity as a real
							// actor and runs the body; the declaration buys nothing there and the author is
							// otherwise given a clean compile for an action that can never start.
							if (!UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(
								ResolveCrowdyRecipient(Info->Recipient)))
							{
								CompilationContext.MessageLog.Warning(*FString::Printf(
									TEXT("[CrowdySDK] Crowdy Replicated event '%s' is ticked Is A One-Shot Action, but its Recipient delivers it to one named machine, which holds the entity as a real actor and runs this body. No client drawing the entity as one of a crowd receives it, so the declaration starts no action. Set Recipient to Spatial Multicast or Multicast, or untick Is A One-Shot Action."),
									*Function->GetName()));
							}
							else
							{
								const FString ActionProblem =
									CrowdyAuthoringContributions::DescribeActionEventProblem(Function);
								if (!ActionProblem.IsEmpty())
								{
									CompilationContext.MessageLog.Warning(*FString::Printf(
										TEXT("[CrowdySDK] Crowdy Replicated event '%s' is ticked Is A One-Shot Action, but %s"),
										*Function->GetName(), *ActionProblem));
								}
							}
						}
					}
				}

				if (InjectReplicateGate(CompilationContext.MessageLog, IntermediateGraph, Entry))
				{
					GatedEvents.Add(FunctionName);
				}
				else
				{
					CompilationContext.MessageLog.Warning(*FString::Printf(
						TEXT("[CrowdySDK] Could not install the replication gate for event '%s'; it will not auto-replicate."),
						*FunctionName.ToString()));
				}
			}
		}

		// An event the author marked, that no compiled entry was found for, gets NOTHING done to it: no
		// metadata stamped this compile and no gate installed. It then looks entirely healthy at runtime,
		// because the marker it already carries is serialized from whichever earlier compile did match, and
		// it sends nothing at all while carrying it.
		//
		// Reported at Error, the same severity as the other silently-wrong-at-runtime checks here, and this
		// is the loudest of them: the send path is never entered, so nothing downstream can report a drop.
		for (const TPair<FName, FCrowdyReplicatedEventMeta>& Marked : ReplicatedEvents)
		{
			if (GatedEvents.Contains(Marked.Key)) continue;

			CompilationContext.MessageLog.Error(*FString::Printf(
				TEXT("[CrowdySDK] Crowdy Replicated event '%s' reached no compiled entry, so it has NO dispatch gate: calling it runs its body locally and sends nothing, on every machine, with no error at runtime. A stale marker from an earlier compile is why it still reads as replicated. Check the event is reachable and named as declared, then compile and save."),
				*Marked.Key.ToString()));
		}

		// To the OUTPUT log, not the message log, and this is the point of it: a compiler message is only
		// visible in the Compiler Results panel of whoever happened to be looking, while the question "did
		// this extension run for this Blueprint, and what did it gate" needs answering from a log file after
		// the fact. Only Blueprints that mark an event print, so this is quiet on everything else.
		UE_LOG(LogCrowdyNodes, Display,
			TEXT("[CrowdySDK] '%s': %d Crowdy Replicated event(s) marked, %d gated."),
			*Blueprint->GetName(), ReplicatedEvents.Num(), GatedEvents.Num());

		for (const TPair<FName, FCrowdyReplicatedEventMeta>& Marked : ReplicatedEvents)
		{
			UE_LOG(LogCrowdyNodes, Display, TEXT("[CrowdySDK]   '%s' recipient='%s' gate=%s"),
				*Marked.Key.ToString(),
				Marked.Value.Recipient.IsEmpty() ? TEXT("<default>") : *Marked.Value.Recipient,
				GatedEvents.Contains(Marked.Key) ? TEXT("installed") : TEXT("NOT INSTALLED"));
		}
	}
	else if (bHasEntityComponent)
	{
		// A Crowdy entity Blueprint that marked nothing. Said so that "the extension ran and found no marked
		// events" can be told from "the extension never ran", which are the same silence otherwise, and the
		// difference between an authoring mistake and a broken editor module.
		UE_LOG(LogCrowdyNodes, Display,
			TEXT("[CrowdySDK] '%s': no Crowdy Replicated events marked on any custom event node."),
			*Blueprint->GetName());
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

		// A CrowdyState-replicated VARIABLE whose type this plane cannot carry must hard-fail the
		// compile, the sibling of the replicated-event checks above: discovery would otherwise omit it
		// from the rep layout (FCrowdyStateLayoutBuilder::BuildLayout) and it would silently never
		// replicate, the only signal being a log line. Own properties only; supers were validated when
		// the parent compiled. The reason text comes from the same classifier discovery uses, so this
		// error reads identically to the discovery log and the variable-details dropdown. Editor module,
		// so WITH_METADATA is always defined and HasMetaData is direct (as the event checks above do).
		for (TFieldIterator<FProperty> It(NewClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasMetaData(CrowdyStateMetaKeys::Replicate)) continue;

			// Crowdy and Unreal replication are mutually exclusive on one variable. Picking Crowdy clears the
			// native rep flags (the variable customization's ClearNativeReplication), but UE 5.8 exposes no way
			// to disable another customization's rows, so the native Replication combo stays interactive and a
			// user could re-enable it afterward. Fail the compile if both are set so the exclusivity holds
			// regardless of the editor UI, the sibling of the replicated-event FUNC_NetFuncFlags error above.
			if (Property->HasAnyPropertyFlags(CPF_Net))
			{
				CompilationContext.MessageLog.Error(
					TEXT("[CrowdySDK] Crowdy Replicated variable @@ also has Unreal replication enabled; ")
					TEXT("the two are mutually exclusive. Set the variable's Replication to None ")
					TEXT("(Crowdy manages its replication)."),
					Property);
			}

			const ECrowdyStatePropertySupport Support =
				FCrowdyStateLayoutBuilder::ClassifyStateProperty(Property);
			if (Support == ECrowdyStatePropertySupport::Supported) continue;

			// @@ resolves to a clickable link to the variable: FProperty is an FField, for which the
			// results log has a token overload, so double-clicking the error jumps to the variable.
			CompilationContext.MessageLog.Error(
				*FString::Printf(
					TEXT("[CrowdySDK] Crowdy Replicated variable @@ %s. ")
					TEXT("Change its type or clear Replication on it."),
					*FCrowdyStateLayoutBuilder::DescribeStateSupport(Support)),
				Property);
		}

		// A Server Owned (Game Model) variable requires its class to be a Game Model container, or the attribute is
		// silently missing on the server. Fail the compile so the author marks the class at author time, the sibling
		// of the CrowdyState type check above. Own properties only, and an Abstract class is exempt: an Abstract
		// class is never instantiated, so its Server Owned attributes exist only to be inherited by concrete
		// container subclasses (which discovery gathers with their inherited attributes). A concrete non-container
		// class that declares one, by contrast, has instances whose attribute could never sync, so it is the error.
		//
		// Asked only where the marker can actually be read. Without the editor module no resolver is
		// installed, and an unanswerable question must not fall through to the "not a container" branch:
		// that would fail every Server Owned variable in the project on a process that has nothing wrong
		// with it.
		const bool bContainerAnswerAvailable =
			static_cast<bool>(CrowdyApplyEffectNodeShared::GetContainerBlueprintResolver());

		if (bContainerAnswerAvailable
			&& !CrowdyApplyEffectNodeShared::IsGameModelContainerBlueprint(Blueprint)
			&& !NewClass->HasAnyClassFlags(CLASS_Abstract))
		{
			for (TFieldIterator<FProperty> It(NewClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property->HasMetaData(CrowdyGameModelMetaKeys::Model)) continue;

				CompilationContext.MessageLog.Error(
					TEXT("[CrowdySDK] Server Owned variable @@ requires its class to be a Game Model container. ")
					TEXT("Mark the class from the Crowdy SDK toolbar, or make the class Abstract if it is only a base ")
					TEXT("for container subclasses."),
					Property);
			}
		}

		// Keep the cooked registry bake and any live (PIE) runtime registry in step with the metadata just
		// stamped, both of which are editor-only and so are reached through a hook. Nothing is installed in
		// a game process, where there is no asset to mark dirty and no editor registry to refresh.
		CrowdyBlueprintCompileHooks::NotifyClassCompiled(NewClass);

		// Drop every cached Game Model attribute table, not just this class's. A table is built by walking
		// the class and its supers, so it can hold properties owned by an ancestor; recompiling that ancestor
		// frees them while the derived class keeps its identity, which would leave a table that still passes
		// both staleness checks and resolves to freed reflection data. Derived classes are relinked rather
		// than fully compiled, so they never reach this point on their own and cannot be dropped individually.
		// This runs here rather than off the post-compile broadcast because that broadcast is documented as
		// skippable, while this extension runs for every class whose layout is rebuilt.
		FCrowdyModelAttributeLookup::InvalidateAll();
	}
}
