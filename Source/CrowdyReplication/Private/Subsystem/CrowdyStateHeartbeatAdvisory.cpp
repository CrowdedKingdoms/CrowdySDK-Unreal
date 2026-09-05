#include "CrowdyStateHeartbeatAdvisory.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "CrowdyReplicationLog.h"
#include "HAL/IConsoleManager.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace CrowdyStateHeartbeatAdvisory
{
	namespace Private
	{
		// One advised property. Keyed by the DECLARING class path, the property name and its declared type,
		// so renaming or retyping the property is a different subject and speaks again while a Blueprint
		// recompile of the same declaration is not.
		//
		// Deliberately not FCrowdyRepProperty::PropertyID, which folds the class the LAYOUT was built for:
		// a layout includes its super-class properties, so one property declared on a base class carries a
		// different id in every subclass layout and would be advised once per subclass, each line naming a
		// class the UPROPERTY it is complaining about cannot be edited on.
		struct FAdvisedRecord
		{
			FString ClassPath;
			FString PropertyName;
			FString EnumName;

			// How many times this property has been resolved while still qualifying. The warning was
			// emitted on the first one; the rest are what DescribeAdvised exists to report.
			int32 DecisionCount = 0;
		};

		// Deliberately outlives the layout cache. That cache is reset wholesale on every world init, so
		// keying "have we said this" on it would repeat the whole advisory on every level load.
		TMap<int64, FAdvisedRecord>& AdvisedRecordsBySubject()
		{
			static TMap<int64, FAdvisedRecord> Records;
			return Records;
		}

		// The class a property is declared on, which is the one an author can edit the UPROPERTY on. Falls
		// back to the class the layout was built for, because a property whose owner is not a class has no
		// better name to offer and naming nothing would be worse than naming the subclass.
		const UClass* ResolveDeclaringClass(const FProperty* Property, const UClass* LayoutOwnerClass)
		{
			const UClass* DeclaringClass = Property ? Property->GetOwnerClass() : nullptr;
			return DeclaringClass ? DeclaringClass : LayoutOwnerClass;
		}

		// The subject a warning is said once about: this property on the class that declares it. The declared
		// type participates so a retype is a new subject, the same reason PropertyID carries its canonical
		// type; GetCPPType is enough here because the advisory only ever speaks about enum-typed properties
		// and a rename of the enum is a rename of the type.
		//
		// Takes the class path already built rather than the class, because the one caller needs the same
		// string again for the record and this is a concatenation per property per registration sweep.
		int64 MakeSubjectKey(const FString& DeclaringClassPath, const FProperty& Property)
		{
			return FCrowdyTypeIDGenerator::GenerateFromString(
				DeclaringClassPath + TEXT("::") + Property.GetName() + TEXT(":") + Property.GetCPPType());
		}

		// The enum a property is declared as, or null when it is not enum-typed. An enum class reflects as
		// an FEnumProperty; TEnumAsByte and a Blueprint enum variable reflect as an FByteProperty carrying
		// the enum, and a plain uint8 reflects as an FByteProperty carrying none.
		const UEnum* ResolveDeclaredEnum(const FProperty* Property)
		{
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
			{
				return EnumProperty->GetEnum();
			}

			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				return ByteProperty->Enum;
			}

			return nullptr;
		}
	}

	void ReportLayout(const FCrowdyRepLayout& Layout)
	{
		// Everything below builds a record whose only consumer is a log statement and a console command,
		// and Shipping strips both (NO_LOGGING, and the command is guarded out at the bottom of this
		// file). Left in, this walks every replicated property of every registered class at startup to
		// produce a report nothing in that build can print.
#if UE_BUILD_SHIPPING
		return;
#else
		const UClass* OwnerClass = Layout.OwnerClass.Get();
		if (!OwnerClass)
		{
			return;
		}

		TMap<int64, Private::FAdvisedRecord>& Records = Private::AdvisedRecordsBySubject();

		for (const FCrowdyRepProperty& RepProp : Layout.Properties)
		{
			if (!RepProp.Property)
			{
				continue;
			}

			// Resolved before the key is built, and left before it. A record is only ever inserted for an
			// enum-typed property, and the key carries the declared type, so for anything else the Remove
			// below has nothing it could match: building its key would be a string concatenation per
			// property per registration sweep with nothing on the other end of it. Most of a layout is not
			// an enum.
			const UEnum* DeclaredEnum = Private::ResolveDeclaredEnum(RepProp.Property);
			if (!DeclaredEnum)
			{
				continue;
			}

			const UClass* DeclaringClass = Private::ResolveDeclaringClass(RepProp.Property, OwnerClass);
			const FString DeclaringClassPath = DeclaringClass->GetPathName();
			const int64 SubjectKey = Private::MakeSubjectKey(DeclaringClassPath, *RepProp.Property);

			// A keyframe never carries an owner-only property, whatever it is marked, so advising the
			// heartbeat flag on one would be advising a remedy that changes nothing.
			if (RepProp.bHeartbeat || RepProp.bOwnerOnly)
			{
				// Dropping it here is how the remedy is confirmed: a property that gains the heartbeat flag
				// leaves the advised list on the next resolution of its class.
				Records.Remove(SubjectKey);
				continue;
			}

			Private::FAdvisedRecord& Record = Records.FindOrAdd(SubjectKey);
			++Record.DecisionCount;
			if (Record.DecisionCount > 1)
			{
				continue;
			}

			Record.ClassPath = DeclaringClassPath;
			Record.PropertyName = RepProp.Property->GetName();
			Record.EnumName = DeclaredEnum->GetName();

			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("[CrowdyAutoRegistry] '%s' replicates the CrowdyState property '%s', whose type is the enum '%s', and it is not part of the keyframe heartbeat. An enum is held state: it is sent when it changes and is never re-sent, so a client that starts observing the entity after the last change, or that loses the one unreliable datagram carrying it, shows the wrong value until the value happens to change again. Add CrowdyHeartbeat to the UPROPERTY meta, or tick Keyframe heartbeat in the variable's Crowdy Replication settings, to have it re-sent on the keyframe interval; that interval must also be non-zero on the map profile and the entity's State Heartbeat must not be Off. Ignore this if the value is meant to be transient. Advisory only, nothing is refused. Said once per property: crowdy.state.heartbeat.advisories lists every property currently advised and how many resolutions each line stands for."),
				*Record.ClassPath, *Record.PropertyName, *Record.EnumName);
		}
#endif
	}

	FString DescribeAdvised()
	{
		const TMap<int64, Private::FAdvisedRecord>& Records = Private::AdvisedRecordsBySubject();
		if (Records.IsEmpty())
		{
			return TEXT("[CrowdyAutoRegistry] No CrowdyState enum property is currently outside the keyframe heartbeat.");
		}

		int32 TotalDecisions = 0;
		FString Text = TEXT("[CrowdyAutoRegistry] CrowdyState enum properties outside the keyframe heartbeat:");
		for (const TPair<int64, Private::FAdvisedRecord>& Pair : Records)
		{
			const Private::FAdvisedRecord& Record = Pair.Value;
			TotalDecisions += Record.DecisionCount;
			Text += FString::Printf(TEXT("\n  '%s' on '%s' (enum '%s'), resolved %d time(s) since the one warning about it."),
				*Record.PropertyName, *Record.ClassPath, *Record.EnumName, Record.DecisionCount);
		}

		Text += FString::Printf(TEXT("\n  %d propert(y/ies), %d resolution(s) in total. Each was warned about once; the rest were silent."),
			Records.Num(), TotalDecisions);
		return Text;
	}

	void ForgetAdvisedForTest()
	{
		Private::AdvisedRecordsBySubject().Reset();
	}
}

// Guarded out of Shipping, matching crowdy.mass.state.verdict, because this project builds Shipping with
// NO_LOGGING: without the guard the command still registers and autocompletes while the only statement in
// it is stripped, so running it prints nothing and reads as "there are no advisories" rather than "this
// build cannot answer". A command that is absent says the second thing correctly.
#if !UE_BUILD_SHIPPING
static FAutoConsoleCommand GCrowdyStateHeartbeatAdvisories(
	TEXT("crowdy.state.heartbeat.advisories"),
	TEXT("Lists every CrowdyState enum property this process found outside the keyframe heartbeat, and how many resolutions each one's single warning stands for."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		UE_LOG(LogCrowdyReplication, Display, TEXT("%s"), *CrowdyStateHeartbeatAdvisory::DescribeAdvised());
	}));
#endif
