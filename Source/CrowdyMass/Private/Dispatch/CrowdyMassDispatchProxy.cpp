#include "Dispatch/CrowdyMassDispatchProxy.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UnrealType.h"

namespace
{
#if !UE_BUILD_SHIPPING
	// Running an inbound event's own body on a per-class stand-in is what makes an event authored once
	// behave the same wherever the entity is drawn. It is also a change in what arrives on an observer:
	// before it, nothing at all ran for an event nothing was registered for. This turns it off without
	// unwinding a build, which is what a session measuring its cost, or one chasing a body that misbehaves
	// on observers only, needs.
	int32 GProxyDispatchEnabled = 1;
	FAutoConsoleVariableRef CVarProxyDispatchEnabled(
		TEXT("crowdy.mass.event.proxy"),
		GProxyDispatchEnabled,
		TEXT("Run an inbound event's own body on a per-class stand-in for entities held here as rows. 1 on, 0 off. Off, an event nothing registered a handler for lands nothing beyond the action write a declared action gets, which is what this client did before the stand-in existed."),
		ECVF_Default);
#endif

}

AActor* CrowdyMassDispatchProxy::Construct(UWorld* World, UClass* ActorClass)
{
	if (!World || !World->PersistentLevel)
	{
		return nullptr;
	}

	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{
		return nullptr;
	}

	if (ActorClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return nullptr;
	}

	// Outered to the level rather than to the transient package, because AActor::GetWorld walks the outer
	// chain to a level and answers null from anywhere else, and a body whose world-context nodes see null
	// does nothing at all. Never added to the level's actor list: this is an actor object, not an actor in
	// the world.
	return NewObject<AActor>(World->PersistentLevel, ActorClass, NAME_None, RF_Transient);
}

void CrowdyMassDispatchProxy::BuildResetPlan(const AActor* Proxy, TArray<FProperty*>& OutResetProperties,
	TArray<FProperty*>& OutSubobjectProperties, TArray<TObjectPtr<UObject>>& OutSubobjectValues)
{
	OutResetProperties.Reset();
	OutSubobjectProperties.Reset();
	OutSubobjectValues.Reset();
	if (!Proxy)
	{
		return;
	}

	for (FProperty* Property = Proxy->GetClass()->PropertyLink; Property; Property = Property->PropertyLinkNext)
	{
		// Checked before the instanced flag, because ownership is what matters and the flag is only evidence
		// of it: a component pointer carries the flag, a plain object property holding a default subobject
		// does not, and both name something belonging to this proxy.
		if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(Property))
		{
			UObject* Value = AsObject->GetObjectPropertyValue_InContainer(Proxy);
			if (Value && Value->IsIn(Proxy))
			{
				OutSubobjectProperties.Add(Property);
				OutSubobjectValues.Add(Value);
				continue;
			}
		}

		// A container of instanced references is not restorable by a pointer write, so it is left alone.
		if (Property->HasAnyPropertyFlags(CPF_InstancedReference))
		{
			continue;
		}

		OutResetProperties.Add(Property);
	}
}

void CrowdyMassDispatchProxy::ResetToClassDefaults(AActor* Proxy, const TConstArrayView<FProperty*> ResetProperties,
	const TConstArrayView<FProperty*> SubobjectProperties, const TConstArrayView<TObjectPtr<UObject>> SubobjectValues)
{
	if (!Proxy)
	{
		return;
	}

	const UObject* Cdo = Proxy->GetClass()->GetDefaultObject();
	if (!Cdo)
	{
		return;
	}

	for (FProperty* Property : ResetProperties)
	{
		Property->CopyCompleteValue_InContainer(Proxy, Cdo);
	}

	// Restored to the proxy's OWN subobject, never copied from the default object, whose components belong
	// to an object that must not be written to and would then be shared by every entity.
	const int32 SubobjectCount = FMath::Min(SubobjectProperties.Num(), SubobjectValues.Num());
	for (int32 Index = 0; Index < SubobjectCount; ++Index)
	{
		if (const FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(SubobjectProperties[Index]))
		{
			AsObject->SetObjectPropertyValue_InContainer(Proxy, SubobjectValues[Index]);
		}
	}
}

void CrowdyMassDispatchProxy::ResetToClassDefaults(AActor* Proxy)
{
	TArray<FProperty*> ResetProperties;
	TArray<FProperty*> SubobjectProperties;
	TArray<TObjectPtr<UObject>> SubobjectValues;
	BuildResetPlan(Proxy, ResetProperties, SubobjectProperties, SubobjectValues);
	ResetToClassDefaults(Proxy, ResetProperties, SubobjectProperties, SubobjectValues);
}

bool CrowdyMassDispatchProxy::IsEnabled()
{
#if !UE_BUILD_SHIPPING
	return GProxyDispatchEnabled != 0;
#else
	return true;
#endif
}

// !UE_BUILD_SHIPPING as well as the test guard: the scope writes the console variable behind IsEnabled, and
// that variable only exists outside Shipping. WITH_DEV_AUTOMATION_TESTS can be forced on in a Shipping build,
// where this would then fail to compile. A test scope there would say nothing anyway: IsEnabled is a hard
// true under Shipping, so the dispatch cannot be turned off.
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
CrowdyMassDispatchProxy::FScopedDisableForTest::FScopedDisableForTest()
	: Previous(GProxyDispatchEnabled)
{
	GProxyDispatchEnabled = 0;
}

CrowdyMassDispatchProxy::FScopedDisableForTest::~FScopedDisableForTest()
{
	GProxyDispatchEnabled = Previous;
}
#endif
