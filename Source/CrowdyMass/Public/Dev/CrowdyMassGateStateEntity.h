#pragma once

#include "CoreMinimal.h"
#include "CrowdyMassGateStateEntity.generated.h"

/**
 * The stance the gate's synthesized deltas carry, standing in for a consuming game's own state enum.
 *
 * Declared here, in this module, so the gates depend on nothing a project authored: their verdict is meant
 * to be a statement about the plugin, and an enum borrowed from a game would make it a statement about that
 * game's content instead.
 *
 * The values matter to what the gates prove. The harness authors pose rows in an order deliberately unlike
 * this order, so a row id and a stance value are never equal: a receive path that wrote the arriving byte
 * straight through would fail the gates instead of passing them by coincidence.
 */
UENUM()
enum class ECrowdyMassGateStance : uint8
{
	Idle,
	Wounded,
	Downed
};

/**
 * The CrowdyState-native class the gates build synthesized deltas against, standing in for a
 * consuming game's own entity class so the participant-less receive path can be exercised without
 * depending on any one game's plugin.
 *
 * The two fields are claimed in two DIFFERENT ways, which is most of what the gates are for.
 *
 * Stance is claimed by its TYPE. CrowdyMass registers one standard pose ingestor and binds it to whichever
 * CrowdyState property is declared as the enum that this class's animation set names
 * (UCrowdyMassEntitySubsystem::ResolveStateFragmentClass). So the property's NAME is not part of the
 * contract and renaming it changes nothing; what matters is that it stays declared as
 * ECrowdyMassGateStance and that the harness keeps naming that enum on the set it binds to this class. A
 * class with no animation set binds no pose at all, which is why the harness authors one rather than
 * relying on whatever the project ships.
 *
 * GateOnlyStateByte is claimed by NAME, through the same public RegisterStateFragmentField call a
 * consuming game uses, and stands in for a field a game adds. A field name claimed that way is claimed
 * process-wide rather than per class, so the same name is matched against every class whose layout is
 * resolved. It carries no ingestor until a gate registers one: that is a property of what is registered
 * right now, not a guarantee that nothing will ever claim it, exactly as FCrowdyMassGateWireState's own
 * unclaimed fields are qualified.
 *
 * Both fields are one byte because that is what the gates need, not because the registry demands it: a
 * name-matched field binds whatever type it registers for, while the pose is read as a byte by the pose
 * ingestor itself (see the type check in UCrowdyMassEntitySubsystem::ResolveStateFragmentClass). A
 * byte-wide enum reaches the wire as its value rather than its entry name (see EnumValueProperty in
 * CrowdyStateCodec.cpp), so it costs exactly what the raw byte it replaced cost.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassGateStateEntity : public UObject
{
	GENERATED_BODY()

public:
	// Claimed by TYPE. The pose the gates drive, and the only field CrowdyMass itself binds an ingestor to.
	UPROPERTY(meta = (CrowdyState))
	ECrowdyMassGateStance Stance = ECrowdyMassGateStance::Idle;

	// Claimed by NAME, by nothing in this build until a gate registers for it.
	UPROPERTY(meta = (CrowdyState))
	uint8 GateOnlyStateByte = 0;
};
