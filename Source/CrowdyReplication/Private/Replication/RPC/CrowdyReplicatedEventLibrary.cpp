#include "Replication/RPC/CrowdyReplicatedEventLibrary.h"

#include "Replication/RPC/CrowdyRPC.h"

DEFINE_FUNCTION(UCrowdyReplicatedEventLibrary::execCrowdyDispatchReplicatedEvent)
{
	// CustomThunk: the stack describes the marked event that called us, not this gate.
	// Stack.Node is that event's UFunction, Stack.Locals is its parameter frame (already
	// populated by the caller), and Stack.Object is the entity or component it lives on.
	// Stack.OutParms holds the by-ref/out parameters, including const-ref containers, which the
	// VM keeps here rather than inline in Locals, so serialization can read their real values.
	UFunction* EventFunction = Stack.Node;
	void* ParameterFrame = Stack.Locals;
	FOutParmRec* OutParms = Stack.OutParms;
	UObject* Self = Stack.Object;

	P_FINISH;

	bool bRoutedOverNetwork = false;
	P_NATIVE_BEGIN;
	bRoutedOverNetwork = FCrowdyRPC::DispatchOrReplayBlueprintCall(Self, EventFunction, ParameterFrame, OutParms);
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bRoutedOverNetwork;
}
