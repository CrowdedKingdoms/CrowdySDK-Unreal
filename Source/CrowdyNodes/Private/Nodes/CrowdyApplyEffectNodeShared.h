// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Nodes/CrowdyApplyEffectContainerResolver.h"
#include "Nodes/CrowdyApplyEffectNodePins.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"

struct FPropertyChangedEvent;
class UBlueprint;

/**
 * The pieces every "Apply Crowdy Effect" node has in common, whatever kind of node it is: the names of the pins the
 * reshape reads or rewrites, what to do with a pin the referenced effect does not use, which pins are the node's own
 * authoring surface rather than an argument of the call it lowers to, the JSON encoder each magnitude value type is
 * routed through, the decoder a declared return value is routed back through, the graph pin type each value type maps
 * to, and which edits to the effect asset can change the pin layout. Free functions with no graph state, so they can
 * be exercised headlessly and so a single definition is
 * shared instead of copied per node (a helper defined twice in one module collides under a unity build).
 */
namespace CrowdyApplyEffectNodeShared
{
	// The input pins the reshape reads or rewrites. These are parameter names on the library functions the nodes
	// wrap, so renaming a parameter there has to be mirrored here; a test asserts they still resolve.
	extern const FName PN_Effect;
	extern const FName PN_Target;
	extern const FName PN_Source;
	extern const FName PN_Overrides;
	extern const FName PN_Level;

	// The raw JSON result the latent node already exposes. This is a parameter of the apply-outcome delegate rather
	// than of a function, so renaming it there has to be mirrored here; a test asserts it still resolves.
	extern const FName PN_ReturnValueJson;

	// The typed result pin the latent node synthesizes beside the raw one. Prefixed for the same reason magnitude
	// pins are: it must collide with neither a factory parameter, a delegate parameter, nor the schema's own
	// well-known "ReturnValue" name.
	extern const FName PN_ReturnValue;

	/**
	 * Whether an unwired Target pin will fall back to the Blueprint's own self reference. Both apply nodes declare
	 * DefaultToSelf on Target, so an unconnected pin with no explicit object means "apply this to me".
	 *
	 * The rule itself takes plain facts rather than a pin, because a UEdGraphPin cannot exist without an owning
	 * node and so cannot be constructed in a headless test. The pin overload is the thin adapter.
	 */
	CROWDYNODES_API bool TargetResolvesToSelf(bool bHasLinks, bool bHasDefaultObject);
	CROWDYNODES_API bool TargetResolvesToSelf(const UEdGraphPin* TargetPin);

	/**
	 * True when Class is usable as an effect target: it carries the CrowdyContainer tag that makes it a Game Model
	 * container. Null, or a class with no tag, returns false.
	 *
	 * The tag is read for exactly this class, not its supers, which matches the runtime bind: a class only counts
	 * as a container if it declares the tag itself.
	 */
	CROWDYNODES_API bool IsGameModelContainerClass(const UClass* Class);

	/**
	 * The class an unwired Target resolves to: the Blueprint being compiled. Prefers the generated class, which is
	 * the representation the container tag is stamped onto, and falls back to the skeleton then the parent class.
	 * Null when the node is not in a Blueprint (so a caller can decline to judge rather than report a false error).
	 */
	CROWDYNODES_API UClass* ResolveSelfClassForNode(const UEdGraphNode* Node);

	/**
	 * Report an error when Target is left unwired on a Blueprint whose own class is not a Game Model container.
	 * Self is then not a valid target and the apply would fail at runtime with nothing pointing at the cause, so
	 * this turns it into a compile error naming the class.
	 *
	 * Deliberately silent in two cases, because neither is evidence of a mistake: a Target that is wired to
	 * something (the author chose the target explicitly, and its type is only known at runtime), and a node whose
	 * owning class cannot be resolved at all.
	 */
	CROWDYNODES_API void ValidateSelfTargetIsContainer(const UEdGraphNode* Node, const UEdGraphPin* TargetPin,
		class FCompilerResultsLog& MessageLog);

	/**
	 * True when this compile is the loader regenerating the Blueprint rather than an author compiling it.
	 *
	 * A load-time compile runs inside the loader's own flush, where loading another package re-enters a compile
	 * already in flight, so validation that has to resolve an asset must be skipped there. The author is not
	 * reading a load-time compile's log anyway; the same checks run in full on the next real compile.
	 */
	CROWDYNODES_API bool IsCompilingOnLoad(const UEdGraphNode* Node);

	/**
	 * What a node should do with one of the wrapped function's input pins once the referenced effect is known.
	 * There is deliberately no "remove" case: a function-call node whose parameter has no pin fails to compile, so a
	 * parameter the effect does not use is hidden and put back to its own default rather than dropped. Hiding alone
	 * is not enough, because a value the designer set while the pin was visible survives a node rebuild and would
	 * still be passed to the call.
	 */
	enum class EParameterPinAction : uint8
	{
		Keep,
		HideAndReset
	};

	/**
	 * The disposition of one input pin under a given pin plan. With no literal effect the node cannot know what the
	 * effect needs, so every pin is kept and the designer fills the raw Overrides map by hand. With a literal effect,
	 * Overrides is replaced by the typed magnitude pins, Level is only meaningful when a magnitude samples a curve,
	 * and Source is only meaningful when the effect reads source.<attr>. Every other pin is left alone.
	 */
	CROWDYNODES_API EParameterPinAction ActionForParameterPin(FName PinName, const FCrowdyApplyEffectPinPlan& Plan,
		bool bHasLiteralEffect);

	// True for the pins a node synthesizes for itself rather than pins of the function it calls. They carry their
	// values into the expansion and must be gone before the call is emitted, since a call node carrying a pin that
	// matches no parameter fails to compile.
	CROWDYNODES_API bool ShouldRemovePinBeforeCall(FName PinName);

	// The encoder a magnitude pin's value is routed through on its way into the Overrides map, plus the name of that
	// encoder's input parameter. The four scalar encoders take "Value"; the container_ref path resolves an object to
	// a container id, so it takes "Object".
	CROWDYNODES_API FName EncoderFunctionName(ECrowdyEffectValueType ValueType, FName& OutInputParam);

	// The graph pin type a magnitude of this value type is exposed as.
	CROWDYNODES_API FEdGraphPinType MagnitudePinType(ECrowdyEffectValueType ValueType);

	// The magnitude value type a declared return maps onto. False for None: there is no pin to type. Keyed on the
	// return enum rather than the value type so container_ref, which no return can be, is unrepresentable here.
	CROWDYNODES_API bool ReturnValueType(ECrowdyEffectReturnType ReturnType, ECrowdyEffectValueType& OutValueType);

	// The graph pin type a declared return is exposed as. Routed through MagnitudePinType so a return pin and a
	// magnitude pin of the same type are the same graph type by construction, not by two switches agreeing.
	CROWDYNODES_API FEdGraphPinType ReturnPinType(ECrowdyEffectReturnType ReturnType);

	// The decoder a raw JSON result is routed through on its way to the typed return pin, plus the name of that
	// decoder's input parameter. The mirror of EncoderFunctionName. None is never a caller, since no pin exists for
	// it, and returns NAME_None.
	CROWDYNODES_API FName DecoderFunctionName(ECrowdyEffectReturnType ReturnType, FName& OutInputParam);

	// Whether a declared return type promises a value the effect's body never produces, so the typed return pin
	// would sit there reading its type's zero forever. Takes plain facts rather than a node, because neither a
	// compiler results log nor a live node can be built in a headless test.
	CROWDYNODES_API bool ReturnPinLacksBacking(ECrowdyEffectReturnType DeclaredType, const FString& LoweredReturnExpression);

	/**
	 * Force a referenced effect asset in from its package before anything reads it.
	 *
	 * A pin's default object is only guaranteed to be serialized once its own package has finished loading, and a
	 * Blueprint that is compiled on load is compiled inside that window. An effect read there has no magnitudes and
	 * no container class, so the typed pins are not rebuilt (a wired one is then reported as no longer existing) and
	 * the effect is judged not to compile; compiling by hand afterwards passes, since by then it is loaded.
	 *
	 * Null, an already-loaded asset, and an asset with no linker are all no-ops.
	 */
	CROWDYNODES_API void PreloadEffectAsset(UObject* Effect);

	// The first error out of a set of effect diagnostics, prefixed with its line when it has one, for a node message
	// that has to name the cause in one line. Empty when the set holds no error.
	CROWDYNODES_API FString FirstCompileError(const TArray<FCrowdyEffectDiagnostic>& Diagnostics);

	// Whether a property edit on the referenced effect can change the typed pin layout. The layout is built from the
	// magnitudes array (every sub-field of a magnitude drives a pin), from whether the effect reads a Source, and
	// from the type it declares it returns. An edit to any other field leaves the pins untouched, so it must not
	// trigger a rebuild. This is a closed allowlist: a field the layout starts depending on and that is not added
	// here reshapes nothing, and the stale pins survive until an unrelated edit or a reload.
	CROWDYNODES_API bool ChangeAffectsPinPlan(const FPropertyChangedEvent& PropertyChangedEvent);
}
