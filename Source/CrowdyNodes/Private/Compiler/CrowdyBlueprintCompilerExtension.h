#pragma once

#include "BlueprintCompilerExtension.h"

#include "Core/UDP/Enums/ECrowdyMessageType.h"

#include "CrowdyBlueprintCompilerExtension.generated.h"

class UEdGraphNode;
class UEdGraphPin;

/**
 * Why one node inside a Crowdy multicast body cannot do its work on a client that draws the entity as a
 * crowd row rather than as a spawned actor. None means the node carries no such limit.
 *
 * All of these describe a difference between representations, not a mistake: the same body is correct
 * wherever the entity is a real actor, which is always the case on the machine that owns it. Which
 * representation an observer uses is chosen by the map profile's rendering backend, so the class itself
 * cannot say whether it will ever be a row, and that is why these are reported as warnings.
 */
enum class ECrowdyCrowdBodyLimit : uint8
{
	None,

	// The node's work resumes after the body returns: a latent call, a Timeline, an async action, or a
	// timer set on self. A crowd row runs the body on a stand-in that is reset and repositioned for the
	// next entity as soon as the body returns, so a continuation would resume against a stand-in
	// representing somebody else.
	ResumesLater,

	// The node calls a function on one of the actor's own components. A crowd row is drawn from fragments
	// by processors and owns no per-entity components, so the call either reaches a component that was
	// never registered (a native one, which renders nothing and ticks not at all) or a null one (anything
	// the Blueprint added, since only spawning runs the construction script).
	ComponentOfSelf,
};

UCLASS()
class UCrowdyBlueprintCompilerExtension : public UBlueprintCompilerExtension
{
	GENERATED_BODY()

public:
	// True when Function is Unreal's AActor::HasAuthority. That function answers a question about
	// Unreal's own network role, which a Crowdy entity does not use (the elected host is a
	// convention, not an enforced server), so calling it on a Crowdy entity's graph reads the wrong
	// thing on every non-host client. A pure name/owner check so it can be exercised directly against
	// near-miss UFunctions (an unrelated class's identically named function, Crowdy's own wrapper)
	// without compiling a Blueprint.
	static bool IsForbiddenActorHasAuthorityCall(const UFunction* Function);

	// True when a call's Target (self) pin asks the question about the Blueprint running the graph:
	// the pin is absent or unconnected (the implicit self context), or it is driven by a Get a
	// Reference to Self node, possibly through reroute nodes. A Target wired to some other actor is
	// a question about that actor, where Unreal's network role is the author's own business.
	static bool IsSelfTargetedCallPin(const UEdGraphPin* SelfPin);

	// True when an event routed to this recipient is delivered to clients that may be drawing the entity
	// as a crowd row. Owning-client and host calls land on one named machine, which holds the entity as a
	// real actor, so a body of theirs never runs on a stand-in and carries none of the limits below.
	static bool IsCrowdDeliveredRecipient(ECrowdyEventRecipient Recipient);

	/**
	 * Whether an author ticked Is A One-Shot Action on this event's node.
	 *
	 * Asked here rather than inline at the collection site so it can be pinned. The marker is written onto
	 * the NODE by the details panel and read at runtime off the compiled FUNCTION, and for the whole life
	 * of the Blueprint marker nothing carried it between the two: the box stayed ticked, the event
	 * replicated, its body ran, and no crowd entity ever animated from it.
	 */
	static bool DeclaresOneShotAction(const FKismetUserDeclaredFunctionMetadata& NodeMeta);

	// Every node reachable forward along execution from StartNode, which is the body an event node runs
	// and nothing else in the graph it shares. StartNode itself is not included. This is what scopes the
	// crowd-body warnings to one event: the same node in an ordinary function, or in an owning-client
	// event, carries no limit and must stay silent.
	static void CollectExecutionBody(UEdGraphNode* StartNode, TArray<UEdGraphNode*>& OutNodes);

	// Which limit, if any, this node runs into when the body it belongs to is run for a crowd row. Pure
	// per-node classification with no compile state behind it, so it can be exercised against hand-built
	// nodes; the sweep that decides WHICH bodies to ask about lives in the compile pass.
	static ECrowdyCrowdBodyLimit ClassifyCrowdBodyNode(const UEdGraphNode* Node);

	// The one-line reason and remedy for a limit, which is the text the compile warning carries. Split out
	// so a test can pin that each limit says something, and so the two limits stay worded as one family.
	static FString DescribeCrowdBodyLimit(ECrowdyCrowdBodyLimit Limit);

protected:
	virtual void ProcessBlueprintCompiled(
		const FKismetCompilerContext& CompilationContext,
		const FBlueprintCompiledData& Data) override;
};
