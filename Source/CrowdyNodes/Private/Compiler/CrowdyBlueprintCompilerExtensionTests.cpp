#include "CrowdyBlueprintCompilerExtension.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Compiler/CrowdyAuthoringContributions.h"
#include "CrowdyEditorEventMeta.h"

#include "Components/ActorComponent.h"
#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyMapProfile.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Components/SkeletalMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdyUtilities.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCompilerExtensionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Only the wiring between pins is under test, so pins are created directly rather than through
	// AllocateDefaultPins, which needs a full Blueprint context these bare nodes do not have.
	UEdGraphPin* MakeAuthorityTestPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName PinName)
	{
		return Node->CreatePin(Direction, UEdGraphSchema_K2::PC_Object, PinName);
	}

	template <typename NodeType>
	NodeType* MakeAuthorityTestNode(UEdGraph* Graph)
	{
		NodeType* Node = NewObject<NodeType>(Graph);
		Graph->Nodes.Add(Node);
		return Node;
	}

	// Pin order matters: UK2Node_Knot reads its input as Pins[0] and its output as Pins[1].
	UK2Node_Knot* MakeAuthorityTestReroute(UEdGraph* Graph)
	{
		UK2Node_Knot* Reroute = MakeAuthorityTestNode<UK2Node_Knot>(Graph);
		MakeAuthorityTestPin(Reroute, EGPD_Input, TEXT("InputPin"));
		MakeAuthorityTestPin(Reroute, EGPD_Output, TEXT("OutputPin"));
		return Reroute;
	}

	// An object pin that declares which class it carries, which is how the crowd-body check tells a
	// component target from any other object target.
	UEdGraphPin* MakeTypedObjectPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName PinName,
		UClass* PinClass)
	{
		UEdGraphPin* Pin = Node->CreatePin(Direction, UEdGraphSchema_K2::PC_Object, PinName);
		Pin->PinType.PinSubCategoryObject = PinClass;
		return Pin;
	}

	UEdGraphPin* MakeExecPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName PinName)
	{
		return Node->CreatePin(Direction, UEdGraphSchema_K2::PC_Exec, PinName);
	}

	// A call node pointed at one real engine function, which is what the classifier reads: latency and
	// the Set Timer family are both properties of the function being called, not of the node.
	UK2Node_CallFunction* MakeCallTo(UEdGraph* Graph, UClass* OwnerClass, const FName FunctionName)
	{
		UK2Node_CallFunction* CallNode = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph);
		CallNode->FunctionReference.SetExternalMember(FunctionName, OwnerClass);
		return CallNode;
	}

	// A Get of one of the Blueprint's own component variables, wired into Target: the shape an author
	// gets by dragging a component out of the Components panel and calling something on it.
	UK2Node_VariableGet* MakeComponentGetter(UEdGraph* Graph, UClass* ComponentClass, const bool bSelfContext)
	{
		UK2Node_VariableGet* Getter = MakeAuthorityTestNode<UK2Node_VariableGet>(Graph);
		if (bSelfContext)
		{
			Getter->VariableReference.SetSelfMember(TEXT("Mesh"));
		}
		else
		{
			Getter->VariableReference.SetExternalMember(TEXT("Mesh"), AActor::StaticClass());
		}

		MakeTypedObjectPin(Getter, EGPD_Output, TEXT("Mesh"), ComponentClass);
		return Getter;
	}

	// TestEqual's generic overload wants a string formatter for whatever it is given, and an enum class
	// has none, so the limit is compared as its underlying value and named in the message text instead.
	void TestNodeLimit(FAutomationTestBase& Test, const TCHAR* What, const UEdGraphNode* Node,
		const ECrowdyCrowdBodyLimit Expected)
	{
		Test.TestEqual(What,
			static_cast<int32>(UCrowdyBlueprintCompilerExtension::ClassifyCrowdBodyNode(Node)),
			static_cast<int32>(Expected));
	}
}

// The forbidden-call classifier must recognize the exact function it exists to catch: Unreal's
// AActor::HasAuthority. This is the pure decision the compiler extension's graph sweep uses to fail
// a Crowdy entity Blueprint's compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyForbiddenHasAuthorityDetectsTargetTest,
	"CrowdySDK.Compiler.ForbiddenHasAuthorityDetectsTarget", CrowdyCompilerExtensionTestFlags)
bool FCrowdyForbiddenHasAuthorityDetectsTargetTest::RunTest(const FString& Parameters)
{
	UFunction* HasAuthorityFn = AActor::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(AActor, HasAuthority));
	if (!TestNotNull(TEXT("AActor::HasAuthority resolves"), HasAuthorityFn))
	{
		return false;
	}

	TestTrue(TEXT("AActor::HasAuthority is the forbidden call"),
		UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(HasAuthorityFn));
	TestFalse(TEXT("a null function is never the forbidden call"),
		UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(nullptr));

	return true;
}

// Near miss: a different class's function that happens to share the exact name "HasAuthority" must
// NOT be flagged - the check is Unreal's specific AActor::HasAuthority, not any function with that
// name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyForbiddenHasAuthorityIgnoresSameNameOtherOwnerTest,
	"CrowdySDK.Compiler.ForbiddenHasAuthorityIgnoresSameNameOtherOwner", CrowdyCompilerExtensionTestFlags)
bool FCrowdyForbiddenHasAuthorityIgnoresSameNameOtherOwnerTest::RunTest(const FString& Parameters)
{
	// A synthetic UFunction literally named "HasAuthority" whose owner is UActorComponent, not
	// AActor. UFunction::GetOwnerClass() walks the outer chain to the first UClass it finds, so
	// parenting it directly under UActorComponent's UClass object makes that its owner without
	// needing a real engine function of the same name on an unrelated class.
	UFunction* NearMissFn = NewObject<UFunction>(UActorComponent::StaticClass(), TEXT("HasAuthority"));
	if (!TestNotNull(TEXT("synthetic near-miss function"), NearMissFn))
	{
		return false;
	}

	TestTrue(TEXT("sanity: the synthetic function's owner really is UActorComponent, not AActor"),
		NearMissFn->GetOwnerClass() == UActorComponent::StaticClass());
	TestFalse(TEXT("a same-named function on an unrelated class is not the forbidden call"),
		UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(NearMissFn));

	return true;
}

// Near miss: other AActor functions, and the plugin's own Crowdy-aware wrapper
// (UCrowdyUtilities::CrowdyHasAuthority / GetCrowdyHasAuthority, which queries the elected host
// through UCrowdyHostSubsystem rather than passing through to Unreal's network role), must NOT be
// flagged - only the exact AActor::HasAuthority function is forbidden.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyForbiddenHasAuthorityIgnoresNearMissesTest,
	"CrowdySDK.Compiler.ForbiddenHasAuthorityIgnoresNearMisses", CrowdyCompilerExtensionTestFlags)
bool FCrowdyForbiddenHasAuthorityIgnoresNearMissesTest::RunTest(const FString& Parameters)
{
	UFunction* GetActorLocationFn = AActor::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(AActor, K2_GetActorLocation));
	UFunction* CrowdyHasAuthorityExecFn = UCrowdyUtilities::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyUtilities, CrowdyHasAuthority));
	UFunction* GetCrowdyHasAuthorityFn = UCrowdyUtilities::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyUtilities, GetCrowdyHasAuthority));

	if (!TestNotNull(TEXT("AActor::K2_GetActorLocation resolves"), GetActorLocationFn)
		|| !TestNotNull(TEXT("UCrowdyUtilities::CrowdyHasAuthority resolves"), CrowdyHasAuthorityExecFn)
		|| !TestNotNull(TEXT("UCrowdyUtilities::GetCrowdyHasAuthority resolves"), GetCrowdyHasAuthorityFn))
	{
		return false;
	}

	TestFalse(TEXT("a different AActor function of the same owner is not the forbidden call"),
		UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(GetActorLocationFn));
	TestFalse(TEXT("the exec-flow Crowdy Has Authority wrapper is not the forbidden call"),
		UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(CrowdyHasAuthorityExecFn));
	TestFalse(TEXT("the pure Crowdy Has Authority wrapper is not the forbidden call"),
		UCrowdyBlueprintCompilerExtension::IsForbiddenActorHasAuthorityCall(GetCrowdyHasAuthorityFn));

	return true;
}

// A Has Authority call with nothing plugged into its Target pin is the "am I the authority for
// myself" question the rule exists to stop, so it must read as self-targeted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHasAuthorityUnconnectedTargetIsSelfTest,
	"CrowdySDK.Compiler.HasAuthorityUnconnectedTargetIsSelf", CrowdyCompilerExtensionTestFlags)
bool FCrowdyHasAuthorityUnconnectedTargetIsSelfTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UK2Node_CallFunction* CallNode = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* TargetPin = MakeAuthorityTestPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
	if (!TestNotNull(TEXT("the call node's Target pin"), TargetPin))
	{
		return false;
	}

	TestTrue(TEXT("a call with no Target pin at all asks about self"),
		UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(nullptr));
	TestTrue(TEXT("an unconnected Target pin asks about self"),
		UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(TargetPin));

	return true;
}

// A Target pin driven by a Get a Reference to Self node is the same question written out, directly
// or through any number of reroute nodes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHasAuthoritySelfNodeTargetIsSelfTest,
	"CrowdySDK.Compiler.HasAuthoritySelfNodeTargetIsSelf", CrowdyCompilerExtensionTestFlags)
bool FCrowdyHasAuthoritySelfNodeTargetIsSelfTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UK2Node_Self* SelfNode = MakeAuthorityTestNode<UK2Node_Self>(Graph.Get());
	UEdGraphPin* SelfOutput = MakeAuthorityTestPin(SelfNode, EGPD_Output, UEdGraphSchema_K2::PN_Self);

	UK2Node_CallFunction* DirectCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* DirectTarget = MakeAuthorityTestPin(DirectCall, EGPD_Input, UEdGraphSchema_K2::PN_Self);

	UK2Node_Self* ReroutedSelfNode = MakeAuthorityTestNode<UK2Node_Self>(Graph.Get());
	UEdGraphPin* ReroutedSelfOutput = MakeAuthorityTestPin(ReroutedSelfNode, EGPD_Output, UEdGraphSchema_K2::PN_Self);

	UK2Node_Knot* FirstReroute = MakeAuthorityTestReroute(Graph.Get());
	UK2Node_Knot* SecondReroute = MakeAuthorityTestReroute(Graph.Get());

	UK2Node_CallFunction* ReroutedCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* ReroutedTarget = MakeAuthorityTestPin(ReroutedCall, EGPD_Input, UEdGraphSchema_K2::PN_Self);

	if (!TestNotNull(TEXT("the Self node's output pin"), SelfOutput)
		|| !TestNotNull(TEXT("the direct call's Target pin"), DirectTarget)
		|| !TestNotNull(TEXT("the rerouted Self node's output pin"), ReroutedSelfOutput)
		|| !TestNotNull(TEXT("the rerouted call's Target pin"), ReroutedTarget))
	{
		return false;
	}

	DirectTarget->MakeLinkTo(SelfOutput);

	ReroutedSelfOutput->MakeLinkTo(FirstReroute->GetInputPin());
	FirstReroute->GetOutputPin()->MakeLinkTo(SecondReroute->GetInputPin());
	ReroutedTarget->MakeLinkTo(SecondReroute->GetOutputPin());

	TestTrue(TEXT("a Target wired straight from a Self node asks about self"),
		UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(DirectTarget));
	TestTrue(TEXT("a Target wired from a Self node through reroutes asks about self"),
		UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(ReroutedTarget));

	return true;
}

// A reroute chain with nothing plugged into it drives nothing, so the Target falls back to the
// implicit self context once the compiler collapses the reroutes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHasAuthorityEmptyRerouteTargetIsSelfTest,
	"CrowdySDK.Compiler.HasAuthorityEmptyRerouteTargetIsSelf", CrowdyCompilerExtensionTestFlags)
bool FCrowdyHasAuthorityEmptyRerouteTargetIsSelfTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UK2Node_Knot* Reroute = MakeAuthorityTestReroute(Graph.Get());

	UK2Node_CallFunction* CallNode = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* TargetPin = MakeAuthorityTestPin(CallNode, EGPD_Input, UEdGraphSchema_K2::PN_Self);
	if (!TestNotNull(TEXT("the call node's Target pin"), TargetPin))
	{
		return false;
	}

	TargetPin->MakeLinkTo(Reroute->GetOutputPin());

	TestTrue(TEXT("a Target fed by an empty reroute chain asks about self"),
		UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(TargetPin));

	return true;
}

// The near miss the rule must leave alone: asking whether some OTHER actor has authority. Nothing
// stops a Crowdy entity from also using Unreal replication for unrelated actors, so a Target wired
// to anything but self is the author's own business and must not fail their compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHasAuthorityOtherActorTargetIsNotSelfTest,
	"CrowdySDK.Compiler.HasAuthorityOtherActorTargetIsNotSelf", CrowdyCompilerExtensionTestFlags)
bool FCrowdyHasAuthorityOtherActorTargetIsNotSelfTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UK2Node_CallFunction* OtherActorSource = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* OtherActorOutput = MakeAuthorityTestPin(OtherActorSource, EGPD_Output, TEXT("ReturnValue"));

	UK2Node_CallFunction* DirectCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* DirectTarget = MakeAuthorityTestPin(DirectCall, EGPD_Input, UEdGraphSchema_K2::PN_Self);

	UK2Node_CallFunction* SecondActorSource = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* SecondActorOutput = MakeAuthorityTestPin(SecondActorSource, EGPD_Output, TEXT("ReturnValue"));

	UK2Node_Knot* Reroute = MakeAuthorityTestReroute(Graph.Get());

	UK2Node_CallFunction* ReroutedCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* ReroutedTarget = MakeAuthorityTestPin(ReroutedCall, EGPD_Input, UEdGraphSchema_K2::PN_Self);

	if (!TestNotNull(TEXT("the other actor's output pin"), OtherActorOutput)
		|| !TestNotNull(TEXT("the direct call's Target pin"), DirectTarget)
		|| !TestNotNull(TEXT("the second actor's output pin"), SecondActorOutput)
		|| !TestNotNull(TEXT("the rerouted call's Target pin"), ReroutedTarget))
	{
		return false;
	}

	DirectTarget->MakeLinkTo(OtherActorOutput);

	SecondActorOutput->MakeLinkTo(Reroute->GetInputPin());
	ReroutedTarget->MakeLinkTo(Reroute->GetOutputPin());

	TestFalse(TEXT("a Target wired to another actor is not a question about self"),
		UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(DirectTarget));
	TestFalse(TEXT("a Target wired to another actor through a reroute is not a question about self"),
		UCrowdyBlueprintCompilerExtension::IsSelfTargetedCallPin(ReroutedTarget));

	return true;
}

// Only a multicast reaches clients that may be drawing the entity as a crowd row. An owning-client or
// host call lands on one named machine, which holds the entity as a real actor, so its body carries none
// of the crowd-row limits and must never be warned about.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdRecipientCoversOnlyMulticastsTest,
	"CrowdySDK.Compiler.CrowdRecipientCoversOnlyMulticasts", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCrowdRecipientCoversOnlyMulticastsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a spatial multicast reaches observers"),
		UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ECrowdyEventRecipient::SpatialMulticast));
	TestTrue(TEXT("a channel multicast reaches observers"),
		UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ECrowdyEventRecipient::Multicast));
	TestFalse(TEXT("an owning-client call lands only where the entity is a real actor"),
		UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ECrowdyEventRecipient::OwningClient));
	TestFalse(TEXT("a host call lands only where the entity is a real actor"),
		UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ECrowdyEventRecipient::Host));

	// An author who never touched the recipient dropdown wrote a spatial multicast, because that is what
	// the runtime defaults to. If an unset value read as out of scope here, the commonest event in a
	// project would be the one this check never looked at.
	TestTrue(TEXT("an event with no recipient chosen is in scope"),
		UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ResolveCrowdyRecipient(FString())));
	TestTrue(TEXT("an unrecognised recipient value falls back into scope"),
		UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ResolveCrowdyRecipient(TEXT("Nonsense"))));
	TestFalse(TEXT("the older OwningPlayer spelling still reads as owning client"),
		UCrowdyBlueprintCompilerExtension::IsCrowdDeliveredRecipient(ResolveCrowdyRecipient(TEXT("OwningPlayer"))));

	return true;
}

// Every limit the classifier can return must be able to say what it is, and None must say nothing. A
// limit with empty text would be reported as a warning carrying no reason at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdBodyLimitsDescribeThemselvesTest,
	"CrowdySDK.Compiler.CrowdBodyLimitsDescribeThemselves", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCrowdBodyLimitsDescribeThemselvesTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a node that resumes later explains itself"),
		!UCrowdyBlueprintCompilerExtension::DescribeCrowdBodyLimit(
			ECrowdyCrowdBodyLimit::ResumesLater).IsEmpty());
	TestTrue(TEXT("a call on a component of self explains itself"),
		!UCrowdyBlueprintCompilerExtension::DescribeCrowdBodyLimit(
			ECrowdyCrowdBodyLimit::ComponentOfSelf).IsEmpty());
	TestTrue(TEXT("a node with no limit says nothing"),
		UCrowdyBlueprintCompilerExtension::DescribeCrowdBodyLimit(ECrowdyCrowdBodyLimit::None).IsEmpty());

	return true;
}

// The three shapes of work that resume after the body has returned. A crowd row runs the body on one
// stand-in shared by every entity of the class, reset and repositioned as soon as the body returns, so a
// continuation would resume against a stand-in for somebody else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdBodyDetectsWorkThatResumesLaterTest,
	"CrowdySDK.Compiler.CrowdBodyDetectsWorkThatResumesLater", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCrowdBodyDetectsWorkThatResumesLaterTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UK2Node_CallFunction* DelayNode = MakeCallTo(Graph.Get(), UKismetSystemLibrary::StaticClass(), TEXT("Delay"));
	UK2Node_CallFunction* TimerByEvent = MakeCallTo(
		Graph.Get(), UKismetSystemLibrary::StaticClass(), TEXT("K2_SetTimerDelegate"));
	UK2Node_CallFunction* TimerByName = MakeCallTo(
		Graph.Get(), UKismetSystemLibrary::StaticClass(), TEXT("K2_SetTimer"));
	UK2Node_Timeline* TimelineNode = MakeAuthorityTestNode<UK2Node_Timeline>(Graph.Get());

	// The whole point of the Delay case is that it is recognised through the engine's own latency
	// metadata rather than by name, so a resolve failure would make this test pass for the wrong reason.
	if (!TestNotNull(TEXT("UKismetSystemLibrary::Delay resolves through the call node"),
			DelayNode->GetTargetFunction())
		|| !TestNotNull(TEXT("UKismetSystemLibrary::K2_SetTimerDelegate resolves through the call node"),
			TimerByEvent->GetTargetFunction())
		|| !TestNotNull(TEXT("UKismetSystemLibrary::K2_SetTimer resolves through the call node"),
			TimerByName->GetTargetFunction()))
	{
		return false;
	}

	TestTrue(TEXT("sanity: Delay really is a latent function"), DelayNode->IsLatentFunction());
	TestFalse(TEXT("sanity: Set Timer by Event carries no latency metadata, so only the name check finds it"),
		TimerByEvent->IsLatentFunction());

	TestNodeLimit(*this, TEXT("a Delay resumes after the body returns"),
		DelayNode, ECrowdyCrowdBodyLimit::ResumesLater);
	TestNodeLimit(*this, TEXT("Set Timer by Event resumes after the body returns"),
		TimerByEvent, ECrowdyCrowdBodyLimit::ResumesLater);
	TestNodeLimit(*this, TEXT("Set Timer by Function Name resumes after the body returns"),
		TimerByName, ECrowdyCrowdBodyLimit::ResumesLater);
	TestNodeLimit(*this, TEXT("a Timeline resumes after the body returns"),
		TimelineNode, ECrowdyCrowdBodyLimit::ResumesLater);

	return true;
}

// A call on one of the actor's own components: the shape an author gets by dragging a component out of
// the Components panel. It succeeds on a crowd row and reaches nothing, because the crowd is drawn from
// fragments by processors and owns no per-entity components.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdBodyDetectsCallOnSelfsComponentTest,
	"CrowdySDK.Compiler.CrowdBodyDetectsCallOnSelfsComponent", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCrowdBodyDetectsCallOnSelfsComponentTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UK2Node_VariableGet* Getter = MakeComponentGetter(
		Graph.Get(), USkeletalMeshComponent::StaticClass(), /*bSelfContext*/true);

	UK2Node_CallFunction* DirectCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* DirectTarget = MakeTypedObjectPin(
		DirectCall, EGPD_Input, UEdGraphSchema_K2::PN_Self, USkeletalMeshComponent::StaticClass());
	DirectTarget->MakeLinkTo(Getter->Pins[0]);

	// The same call reached through reroutes, which is how the wire usually looks once a graph has been
	// tidied up.
	UK2Node_VariableGet* ReroutedGetter = MakeComponentGetter(
		Graph.Get(), USkeletalMeshComponent::StaticClass(), /*bSelfContext*/true);
	UK2Node_Knot* Reroute = MakeAuthorityTestReroute(Graph.Get());
	UK2Node_CallFunction* ReroutedCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* ReroutedTarget = MakeTypedObjectPin(
		ReroutedCall, EGPD_Input, UEdGraphSchema_K2::PN_Self, USkeletalMeshComponent::StaticClass());

	ReroutedGetter->Pins[0]->MakeLinkTo(Reroute->GetInputPin());
	ReroutedTarget->MakeLinkTo(Reroute->GetOutputPin());

	TestNodeLimit(*this, TEXT("a call on a component of self is inert on a crowd row"),
		DirectCall, ECrowdyCrowdBodyLimit::ComponentOfSelf);
	TestNodeLimit(*this, TEXT("the same call through reroutes is the same call"),
		ReroutedCall, ECrowdyCrowdBodyLimit::ComponentOfSelf);

	return true;
}

// The near misses this rule must leave alone. A component reached off SOME OTHER actor belongs to that
// actor, which is a real actor wherever it exists at all; an ordinary call on a plain object is nobody's
// component; and a call with nothing wired into Target is about the actor itself, which the stand-in
// stands in for perfectly well.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdBodyIgnoresCallsThatAreNotSelfsComponentTest,
	"CrowdySDK.Compiler.CrowdBodyIgnoresCallsThatAreNotSelfsComponent", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCrowdBodyIgnoresCallsThatAreNotSelfsComponentTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UK2Node_VariableGet* ForeignGetter = MakeComponentGetter(
		Graph.Get(), USkeletalMeshComponent::StaticClass(), /*bSelfContext*/false);
	UK2Node_CallFunction* ForeignCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* ForeignTarget = MakeTypedObjectPin(
		ForeignCall, EGPD_Input, UEdGraphSchema_K2::PN_Self, USkeletalMeshComponent::StaticClass());
	ForeignTarget->MakeLinkTo(ForeignGetter->Pins[0]);

	UK2Node_VariableGet* ActorGetter = MakeComponentGetter(
		Graph.Get(), AActor::StaticClass(), /*bSelfContext*/true);
	UK2Node_CallFunction* ActorCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* ActorTarget = MakeTypedObjectPin(
		ActorCall, EGPD_Input, UEdGraphSchema_K2::PN_Self, AActor::StaticClass());
	ActorTarget->MakeLinkTo(ActorGetter->Pins[0]);

	UK2Node_CallFunction* BareCall = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	MakeAuthorityTestPin(BareCall, EGPD_Input, UEdGraphSchema_K2::PN_Self);

	UK2Node_CallFunction* PrintNode = MakeCallTo(
		Graph.Get(), UKismetSystemLibrary::StaticClass(), TEXT("PrintString"));

	TestNotNull(TEXT("sanity: PrintString resolves, so the ordinary case is a real function"),
		PrintNode->GetTargetFunction());

	TestNodeLimit(*this, TEXT("a component reached off another actor is that actor's business"),
		ForeignCall, ECrowdyCrowdBodyLimit::None);
	TestNodeLimit(*this, TEXT("a self-context variable that is not a component is not a component call"),
		ActorCall, ECrowdyCrowdBodyLimit::None);
	TestNodeLimit(*this, TEXT("a call on the actor itself is fine, since the stand-in is one"),
		BareCall, ECrowdyCrowdBodyLimit::None);
	TestNodeLimit(*this, TEXT("an ordinary library call carries no limit"),
		PrintNode, ECrowdyCrowdBodyLimit::None);
	TestNodeLimit(*this, TEXT("a null node carries no limit"),
		nullptr, ECrowdyCrowdBodyLimit::None);

	return true;
}

// The scoping this whole rule rests on: the warnings belong to ONE event's body, not to the graph it was
// drawn in. A Delay in an ordinary function next to a multicast event is correct code, and reporting it
// would make the rule noise the moment a graph holds more than one event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdBodyIsScopedToOneExecutionChainTest,
	"CrowdySDK.Compiler.CrowdBodyIsScopedToOneExecutionChain", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCrowdBodyIsScopedToOneExecutionChainTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UEdGraph> Graph(NewObject<UEdGraph>(GetTransientPackage()));

	UEdGraphNode* EventNode = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* EventThen = MakeExecPin(EventNode, EGPD_Output, TEXT("then"));

	UEdGraphNode* FirstInBody = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	MakeExecPin(FirstInBody, EGPD_Input, TEXT("execute"))->MakeLinkTo(EventThen);
	UEdGraphPin* FirstThen = MakeExecPin(FirstInBody, EGPD_Output, TEXT("then"));

	UEdGraphNode* SecondInBody = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	MakeExecPin(SecondInBody, EGPD_Input, TEXT("execute"))->MakeLinkTo(FirstThen);

	// A second chain in the same graph, standing for the other events and functions an author draws
	// beside a multicast one.
	UEdGraphNode* OtherChainRoot = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	UEdGraphPin* OtherThen = MakeExecPin(OtherChainRoot, EGPD_Output, TEXT("then"));
	UEdGraphNode* OtherChainNode = MakeAuthorityTestNode<UK2Node_CallFunction>(Graph.Get());
	MakeExecPin(OtherChainNode, EGPD_Input, TEXT("execute"))->MakeLinkTo(OtherThen);

	TArray<UEdGraphNode*> BodyNodes;
	UCrowdyBlueprintCompilerExtension::CollectExecutionBody(EventNode, BodyNodes);

	TestEqual(TEXT("the body is exactly the two nodes downstream of the event"), BodyNodes.Num(), 2);
	TestTrue(TEXT("the first node of the body is in it"), BodyNodes.Contains(FirstInBody));
	TestTrue(TEXT("the node after it is in it too"), BodyNodes.Contains(SecondInBody));
	TestFalse(TEXT("the event node itself is not part of its own body"), BodyNodes.Contains(EventNode));
	TestFalse(TEXT("another chain in the same graph is not part of this body"),
		BodyNodes.Contains(OtherChainNode));

	// A loop back to an earlier node must terminate rather than revisit, since a Blueprint body may
	// legitimately branch back into itself.
	MakeExecPin(SecondInBody, EGPD_Output, TEXT("then"))->MakeLinkTo(FirstInBody->Pins[0]);

	TArray<UEdGraphNode*> LoopedBodyNodes;
	UCrowdyBlueprintCompilerExtension::CollectExecutionBody(EventNode, LoopedBodyNodes);
	TestEqual(TEXT("a body that loops back on itself is still collected once"), LoopedBodyNodes.Num(), 2);

	return true;
}

/**
 * The one-shot marker is read off the node the author ticked it on.
 *
 * It is written by the details panel onto the NODE and read at runtime off the compiled FUNCTION, and for
 * the whole life of the Blueprint marker nothing carried it between the two. Everything visible said it
 * was working: the box stayed ticked, the event replicated, its gate was installed, its body ran on every
 * observer. Only the animation never started, on either tier, with no message anywhere, because the
 * runtime asked the function a question the compiler never answered.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCompilerOneShotMarkerIsReadFromTheNodeTest,
	"CrowdySDK.Compiler.OneShotMarkerIsReadFromTheNode", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCompilerOneShotMarkerIsReadFromTheNodeTest::RunTest(const FString& Parameters)
{
	FKismetUserDeclaredFunctionMetadata NodeMeta;

	TestFalse(TEXT("an event nobody ticked declares no action"),
		UCrowdyBlueprintCompilerExtension::DeclaresOneShotAction(NodeMeta));

	// Exactly what the details panel writes: the key, with no value. The value is not the declaration and
	// must never be read as one, since the C++ meta=(CrowdyAction) it mirrors carries none either.
	NodeMeta.SetMetaData(FName(CrowdyRpcMetaKeys::Action), FString());
	TestTrue(TEXT("and an event whose author ticked Is A One-Shot Action declares one"),
		UCrowdyBlueprintCompilerExtension::DeclaresOneShotAction(NodeMeta));

	// Unticking has to take effect: a compiled function is recompiled in place, so a marker that only ever
	// got added would leave an event acting as an action after its author said it was not one.
	NodeMeta.RemoveMetaData(FName(CrowdyRpcMetaKeys::Action));
	TestFalse(TEXT("and unticking it takes the declaration away again"),
		UCrowdyBlueprintCompilerExtension::DeclaresOneShotAction(NodeMeta));

	// The marker is its own key. Replicating an event does not make it an action, and an event can be an
	// action without carrying any of the addressing keys.
	FKismetUserDeclaredFunctionMetadata ReplicatedOnly;
	ReplicatedOnly.SetMetaData(FName(CrowdyRpcMetaKeys::Replicates), FString());
	ReplicatedOnly.SetMetaData(FName(CrowdyRpcMetaKeys::Recipient), FString(TEXT("SpatialMulticast")));
	TestFalse(TEXT("a replicated spatial multicast is not an action merely for being one"),
		UCrowdyBlueprintCompilerExtension::DeclaresOneShotAction(ReplicatedOnly));

	return true;
}

/**
 * With no backend supplying them, the action contributions answer "nothing", and installing one is what
 * turns them on.
 *
 * Getting this wrong in the quiet direction is the dangerous one: a seam that answers with nothing installed
 * describes an action contract the project has no handler for, and looks like nothing at all in a project
 * that does supply one, which is every project the suite runs in.
 *
 * The process running this test HAS CrowdyMassNodes loaded, so the uninstalled state has to be produced
 * rather than found, and put back afterwards on every exit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthoringContributionsDefaultToAbsentTest,
	"CrowdySDK.Nodes.AuthoringContributionsDefaultToAbsent", CrowdyCompilerExtensionTestFlags)
bool FCrowdyAuthoringContributionsDefaultToAbsentTest::RunTest(const FString& Parameters)
{
	// Puts back exactly what it displaced, whichever way the case leaves. Exact rather than approximate
	// because this is process-global editor state: a stub left behind here would answer every Blueprint
	// compile for the rest of the session.
	struct FScopedUninstalled
	{
		TFunction<FString(const UFunction*)> DisplacedValidator;
		TFunction<TArray<FCrowdyActionParameterSpec>()> DisplacedProvider;

		FScopedUninstalled()
			: DisplacedValidator(CrowdyAuthoringContributions::SetActionEventValidator(nullptr))
			, DisplacedProvider(CrowdyAuthoringContributions::SetActionParameterProvider(nullptr))
		{
		}

		~FScopedUninstalled()
		{
			CrowdyAuthoringContributions::SetActionParameterProvider(MoveTemp(DisplacedProvider));
			CrowdyAuthoringContributions::SetActionEventValidator(MoveTemp(DisplacedValidator));
		}
	};

	// A real reflected function to ask about. Which one does not matter: the case is about whether the
	// seam reaches an installed check at all, and the checks it installs below ignore their argument.
	const UFunction* AnyFunction =
		UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
	if (!TestNotNull(TEXT("a reflected function to ask about resolved"), AnyFunction))
	{
		return false;
	}

	// Read before the guard touches anything, so the restore can be checked against what was really there.
	// NOT asserted to be present: this test ships inside CrowdySDK, and a project with no crowd backend at
	// all is a configuration the seam has to serve, so demanding one here would fail in the place the
	// feature is most correct. What the process had is recorded instead.
	const int32 InstalledParameterCount = CrowdyAuthoringContributions::GetActionParameters().Num();
	AddInfo(FString::Printf(TEXT("This process supplies %d action parameter(s)."), InstalledParameterCount));

	{
		FScopedUninstalled Uninstalled;

		TestTrue(TEXT("with nothing installed, no action problem is described, because there is nobody to ask"),
			CrowdyAuthoringContributions::DescribeActionEventProblem(AnyFunction).IsEmpty());
		TestEqual(TEXT("and there are no parameters to offer, so the offer is not made"),
			CrowdyAuthoringContributions::GetActionParameters().Num(), 0);

		// Installed here rather than relying on the real one, so the case pins the seam and not the backend
		// behind it.
		CrowdyAuthoringContributions::SetActionEventValidator(
			[](const UFunction*) { return FString(TEXT("the installed check answered")); });

		TestEqual(TEXT("and once installed, that check is what answers, verbatim"),
			CrowdyAuthoringContributions::DescribeActionEventProblem(AnyFunction),
			FString(TEXT("the installed check answered")));
	}

	// The half that has to hold either way: whatever this process had, it still has. A stub left installed
	// here would answer every Blueprint compile for the rest of the session.
	TestEqual(TEXT("the teardown put the parameter provider back"),
		CrowdyAuthoringContributions::GetActionParameters().Num(), InstalledParameterCount);

	// Where a backend does supply them, say so about the real one rather than about the stubs above, which is
	// what makes this a check on the installed contribution and not only on the seam holding it.
	if (InstalledParameterCount > 0)
	{
		TestTrue(TEXT("and its validator answers, so the seam reaches a real check and not only a test stub"),
			!CrowdyAuthoringContributions::DescribeActionEventProblem(AnyFunction).IsEmpty());
	}

	return true;
}

/**
 * A project whose every reachable map profile draws entities with actors reports no crowd representation,
 * which is what keeps the crowd-only authoring surfaces out of it.
 *
 * The dangerous direction is the loud one: answering yes here shows an action checkbox and stand-in warnings
 * in a project that can never draw a row, which is the whole complaint this query exists to answer. The
 * other direction, and the precedence between Default Profile and a plugin's shipped profile, needs a
 * backend that does draw rows and is covered where one exists.
 *
 * The settings are a process-wide CDO, so they are emptied and put back exactly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdRepresentationFollowsProfilesTest,
	"CrowdySDK.Nodes.CrowdRepresentationFollowsProfiles", CrowdyCompilerExtensionTestFlags)
bool FCrowdyCrowdRepresentationFollowsProfilesTest::RunTest(const FString& Parameters)
{
	struct FScopedProfileSettings
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>> SavedMapProfiles;
		TSoftObjectPtr<UCrowdyMapProfile> SavedDefaultProfile;
		FString SavedShippedProvider;
		FSoftObjectPath SavedShippedPath;

		FScopedProfileSettings()
		{
			SavedMapProfiles = Settings->MapProfiles;
			SavedDefaultProfile = Settings->DefaultProfile;
			SavedShippedProvider = UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider();
			SavedShippedPath = UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath();

			// The profile an installed plugin offers is part of what a project can reach, so a case about a
			// project that reaches only actor-pool profiles has to withdraw it too.
			Settings->MapProfiles.Empty();
			Settings->DefaultProfile.Reset();
			UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(SavedShippedProvider);
		}

		~FScopedProfileSettings()
		{
			Settings->MapProfiles = SavedMapProfiles;
			Settings->DefaultProfile = SavedDefaultProfile;

			if (!SavedShippedProvider.IsEmpty())
				UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(SavedShippedProvider,
					TSoftObjectPtr<UCrowdyMapProfile>(SavedShippedPath));
		}
	};

	FScopedProfileSettings ScopedSettings;

	TestFalse(TEXT("a project that configures no profile at all reports no crowd representation"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	const TStrongObjectPtr<UCrowdyMapProfile> ActorPoolProfile(NewObject<UCrowdyMapProfile>());
	ActorPoolProfile->ActorManagement.BackendClass = UCrowdyActorPoolBackend::StaticClass();

	ScopedSettings.Settings->DefaultProfile = ActorPoolProfile.Get();
	TestFalse(TEXT("nor does one whose Default Profile draws entities as actors"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	// The map the entry names is never loaded, and does not have to exist: what decides whether a map can be
	// drawn through an entry is that the entry names one at all.
	ScopedSettings.Settings->DefaultProfile.Reset();
	ScopedSettings.Settings->MapProfiles.Add(
		TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/CrowdyNodesGateMap.CrowdyNodesGateMap"))),
		ActorPoolProfile.Get());
	TestFalse(TEXT("nor does one whose only map entry draws entities as actors"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	// A profile that never chose a backend is not a project opting into crowds: the field defaults to the
	// actor pool, and reading a bare profile as a crowd would turn the surfaces on for every project that
	// left it alone.
	const TStrongObjectPtr<UCrowdyMapProfile> UntouchedProfile(NewObject<UCrowdyMapProfile>());
	ScopedSettings.Settings->MapProfiles.Empty();
	ScopedSettings.Settings->DefaultProfile = UntouchedProfile.Get();
	TestFalse(TEXT("nor does one whose profile never chose a backend at all"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
