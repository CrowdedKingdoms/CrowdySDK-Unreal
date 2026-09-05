#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ICrowdyEntityEventHandler.generated.h"

struct FCrowdyEventParams;

UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class UCrowdyEntityEventHandler : public UInterface
{
	GENERATED_BODY()
};

/**
 * Receives a call as VALUES for an entity that has no object to run the call on.
 *
 * A CrowdyEvent is declared once, on the class the entity's owner runs as a real object, and every
 * recipient of a multicast is sent the same call. Wherever a recipient holds that entity as a real
 * object, the function body simply runs on it and none of this is involved. Wherever a recipient holds
 * it only as a row in a table, there is no instance for the body to run on, and a body is the wrong
 * thing to want anyway: what such a recipient needs is not the owner's logic but a change to what it
 * draws. So the call arrives here as its decoded parameters, and the receiver applies whatever the game
 * registered for that function.
 *
 * Optional, and implemented by the object that already holds the entity subscriber slot (see
 * ICrowdyEntitySubscriber). It is a second SHAPE for one plane's payload, not a second registration:
 * there is one registered receiver of everything addressed to entities held as data, and a router that
 * grew a second slot for this would leave a path that claimed one and not the other silently delivering
 * some calls and dropping others. A subscriber that implements neither this nor a handler for a given
 * function refuses that call, which the router reports.
 *
 * Both functions are called on the game thread only, and EntityUUID comes off the network: treat it as
 * an arbitrary value, and never create state keyed on an id that is not already known.
 */
class CROWDYREPLICATION_API ICrowdyEntityEventHandler
{
	GENERATED_BODY()

public:

	/**
	 * True when this receiver has something registered to apply Function to an entity it holds as data.
	 *
	 * Asked before a call is decoded, so an event nothing is registered for costs a lookup rather than a
	 * parameter frame. It is also what the router's wait budget reads: a call whose target has not
	 * arrived yet is held for a fraction of a second when it will be applied to a representation, rather
	 * than for the ordinary spawn wait, because a change to what is drawn is only worth making while it
	 * is still current. The answer must therefore depend on the FUNCTION alone and on nothing about any
	 * particular entity, since at that moment there is no entity to ask about.
	 */
	virtual bool HasEntityEventHandler(const UFunction* Function) const = 0;

	/**
	 * Spends one inbound event from this entity's allowance, or refuses it.
	 *
	 * Asked after HasEntityEventHandler and BEFORE the call's parameters are decoded. That order is the
	 * whole point of the question: decoding is the expensive half of receiving a call, and an allowance
	 * spent once the parameter frame already exists bounds the work after it has been done. Whoever sends
	 * the events chooses both the entity id and how many arrive, so the bound has to sit in front.
	 *
	 * False drops the call on the spot: nothing is decoded and HandleEntityEvent is not reached. Refusing
	 * is never a reason to wait, since the entity is already held.
	 *
	 * Charge here and nowhere else on this route, or one inbound event is billed twice. A receiver that
	 * meters nothing keeps the default and answers true.
	 *
	 * EntityUUID comes off the network. Look an allowance up by it; never create one for an id that is not
	 * already known.
	 */
	virtual bool ChargeEntityEventBudget(const FGuid& EntityUUID, const UFunction* Function)
	{
		return true;
	}

	/**
	 * Applies one inbound call to the entity's own storage.
	 *
	 * Reached only when HasEntityEventHandler answered true for Function and ChargeEntityEventBudget
	 * accepted it, so there is no "nothing registered" case to report here and the allowance has already
	 * been spent. Every other refusal is this receiver's own to make and to report: the id may name an
	 * entity that has just gone, or the entity may not be the kind the event was declared for.
	 *
	 * Params is valid for this call only, and everything in it was chosen by whoever sent the call. See
	 * FCrowdyEventParams.
	 */
	virtual void HandleEntityEvent(const FGuid& EntityUUID, const FGuid& SenderID, const UFunction* Function,
		const FCrowdyEventParams& Params) = 0;
};
