#include "CrowdyMassDroppedStateTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "CrowdyMassStateLogCapture.h"
#include "Misc/AutomationTest.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

// A CrowdyState property that decodes perfectly well and that no registered fragment field asks for is
// stepped over on every delta of every entity of its class. Nothing is malformed, so nothing else in the
// system has anything to complain about, and the author sees a replicated variable that simply does
// nothing on the crowd.
//
// The decision is made once, when a class's layout is resolved against the registered fields, and cached as
// an INDEX_NONE the apply loop reads. These cases cover that the decision is reported where it is made,
// once per class and property, that a property which does reach a field is not reported, and that the
// verdict command still names every dropped property after the log has gone quiet about them.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassDroppedStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The name the fixture below registers a fragment field for, and the name of the one property on each
	// fixture class that is therefore claimed.
	const TCHAR* const ClaimedPropertyName = TEXT("Claimed");

	// A subsystem with exactly one registered CrowdyState fragment field and nothing else: no standard pose
	// field, so no class here can also trip the separate "carries no property of the pose enum" diagnostic
	// and confuse a count, and no project entity config, so a class this fixture does not configure resolves
	// to nothing rather than to whatever the running project happens to ship.
	struct FCrowdyMassDroppedStateFixture
	{
		UCrowdyMassEntitySubsystem* Subsystem = nullptr;

		FCrowdyMassDroppedStateFixture()
		{
			Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());
			Subsystem->SetAuthoredEntityConfigForTest(nullptr, nullptr);
			Subsystem->RegisterStateFragmentField(FName(ClaimedPropertyName), ECrowdyMassStateFieldType::UInt8,
				[](const void*, const FCrowdyMassViewStateContext&) {});
		}

		// Resolves one class exactly as an arriving delta for it would. Returns false when the class carries
		// no usable CrowdyState layout at all, which would make every assertion after it vacuous.
		bool Resolve(FAutomationTestBase& Test, const UClass* Class)
		{
			FCrowdyRepLayout Layout;
			if (!Test.TestTrue(TEXT("the fixture class has a CrowdyState layout"),
				FCrowdyStateLayoutBuilder::BuildLayout(Class, Layout)))
			{
				return false;
			}

			Subsystem->ResolveStateFragmentClassForTest(*Class, Layout);
			return true;
		}
	};
}

/**
 * A property nothing claims is named, with its class, and a property something claims is not.
 *
 * Both halves are the case. Naming the dropped one is the whole feature; staying quiet about the claimed
 * one is what keeps the line worth reading, because a diagnostic that also fires on a correctly wired
 * property teaches an author to skip past the one naming their actual mistake.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDroppedStatePropertyIsReportedTest,
	"CrowdySDK.Mass.DroppedStatePropertyIsReported", CrowdyMassDroppedStateTestFlags)
bool FCrowdyMassDroppedStatePropertyIsReportedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDroppedStateFixture Fixture;

	FCrowdyMassStateLogCapture Capture;
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateTarget::StaticClass()))
	{
		return false;
	}
	const TArray<FString> Warnings = Capture.TakeWarnings();

	if (!TestEqual(TEXT("one line for each of the two properties nothing claims, and no line for the third"),
		Warnings.Num(), 2))
	{
		for (const FString& Warning : Warnings)
		{
			AddInfo(Warning);
		}
		return false;
	}

	TestEqual(TEXT("the property named Health is reported"), CrowdyMassCountLinesContaining(Warnings, TEXT("'Health'")), 1);
	TestEqual(TEXT("so is the one named Stamina"), CrowdyMassCountLinesContaining(Warnings, TEXT("'Stamina'")), 1);
	TestEqual(TEXT("and the property a registered field claims is not mentioned at all"),
		CrowdyMassCountLinesContaining(Warnings, FString::Printf(TEXT("'%s'"), ClaimedPropertyName)), 0);

	const FString& Line = Warnings[0];
	TestTrue(TEXT("the line names the class, which is the half a watched crowd cannot tell you"),
		Line.Contains(UCrowdyMassDroppedStateTarget::StaticClass()->GetName()));
	TestTrue(TEXT("it says what happens to the value rather than only that something is unclaimed"),
		Line.Contains(TEXT("decoded and then discarded")));
	TestTrue(TEXT("it names the call that fixes it"), Line.Contains(TEXT("RegisterStateFragmentField")));
	TestTrue(TEXT("it says the harmless reading out loud, so a deliberate actor-only property is not a mystery"),
		Line.Contains(TEXT("only for actors")));
	TestTrue(TEXT("and it points at the command that lists the rest, since this line is said only once"),
		Line.Contains(TEXT("crowdy.mass.state.verdict")));

	return true;
}

/**
 * A dropped property a field could bind is told WHICH type to register for.
 *
 * A registration names the type it reads, so "call RegisterStateFragmentField with 'Stamina'" is only half
 * an instruction: an author who supplies the wrong half writes a registration the resolution then refuses,
 * and the silence they get back afterwards is exactly the silence they came here about. Three properties of
 * three different types are read, because a message that hardcoded one type would satisfy any single one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDroppedStateBindablePropertyNamesItsTypeTest,
	"CrowdySDK.Mass.DroppedStateBindablePropertyNamesItsType", CrowdyMassDroppedStateTestFlags)
bool FCrowdyMassDroppedStateBindablePropertyNamesItsTypeTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDroppedStateFixture Fixture;

	FCrowdyMassStateLogCapture Capture;
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateTarget::StaticClass()))
	{
		return false;
	}
	const TArray<FString> Bindable = Capture.TakeWarnings();

	if (!TestEqual(TEXT("the two unclaimed properties are both reported"), Bindable.Num(), 2))
	{
		return false;
	}

	const FString& IntLine = Bindable[0].Contains(TEXT("'Health'")) ? Bindable[0] : Bindable[1];
	const FString& FloatLine = Bindable[0].Contains(TEXT("'Stamina'")) ? Bindable[0] : Bindable[1];

	TestTrue(TEXT("the int's line names the type to register for, not only the call"),
		IntLine.Contains(TEXT("Call RegisterStateFragmentField with 'Health' and the type int32")));
	TestTrue(TEXT("and the float beside it is told a different type, so the answer is read off the property"),
		FloatLine.Contains(TEXT("Call RegisterStateFragmentField with 'Stamina' and the type float")));
	TestFalse(TEXT("neither is told a field is read as a byte, which stopped being true and would send them to redeclare a value that binds"),
		IntLine.Contains(TEXT("read as a byte")) || FloatLine.Contains(TEXT("read as a byte")));

	// A byte-wide property nothing claims gets the same shape of remedy, naming its own type. It is here
	// because it is the type that used to be the only one: a message still hardcoding uint8 would pass every
	// assertion about this half and fail both above.
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateByteTarget::StaticClass()))
	{
		return false;
	}
	const TArray<FString> Narrow = Capture.TakeWarnings();

	if (!TestEqual(TEXT("the byte-wide property nothing claims is reported too"), Narrow.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("and it is told to register a field for a uint8"),
		Narrow[0].Contains(TEXT("Call RegisterStateFragmentField with 'Posture' and the type uint8")));
	TestFalse(TEXT("with no talk of what cannot be bound, because for it there is nothing that cannot"),
		Narrow[0].Contains(TEXT("cannot bind this one")));

	return true;
}

/**
 * A dropped property that no registration could bind is told why, and never told to register one.
 *
 * Three properties, three reasons, and they were being handed one or two answers between them. A string
 * replicates perfectly well and no field type can read it, so telling its author to register a field
 * produces the same silence with a registration behind it. An owner-only property is bindable and simply
 * never arrives at an observer, which is the same trap by another road. The bool is here as the control: it
 * is one byte, like the string is not, and it DOES bind, so a message framed on width or on "not a byte"
 * gets it wrong in the opposite direction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDroppedStateUnbindablePropertySaysWhyTest,
	"CrowdySDK.Mass.DroppedStateUnbindablePropertySaysWhy", CrowdyMassDroppedStateTestFlags)
bool FCrowdyMassDroppedStateUnbindablePropertySaysWhyTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDroppedStateFixture Fixture;

	FCrowdyMassStateLogCapture Capture;
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateUnbindableTarget::StaticClass()))
	{
		return false;
	}
	const TArray<FString> Warnings = Capture.TakeWarnings();

	if (!TestEqual(TEXT("all three properties are reported"), Warnings.Num(), 3))
	{
		for (const FString& Warning : Warnings)
		{
			AddInfo(Warning);
		}
		return false;
	}

	FString BoolLine;
	FString StringLine;
	FString OwnerLine;
	for (const FString& Warning : Warnings)
	{
		BoolLine = Warning.Contains(TEXT("'bSprinting'")) ? Warning : BoolLine;
		StringLine = Warning.Contains(TEXT("'Nickname'")) ? Warning : StringLine;
		OwnerLine = Warning.Contains(TEXT("'Secret'")) ? Warning : OwnerLine;
	}

	TestTrue(TEXT("the string is named by its own declared type, which is the true reason no field can read it"),
		StringLine.Contains(TEXT("'Nickname' is FString")));
	TestTrue(TEXT("and the types a field could have been registered for are named, so the reason is checkable rather than asserted"),
		StringLine.Contains(TEXT("Declare it as one of")) && StringLine.Contains(TEXT("int32")));
	TestTrue(TEXT("and is offered the representation that can read it as it stands"),
		StringLine.Contains(TEXT("actor representation")));
	TestFalse(TEXT("it is NOT told to register a field, which could not bind it however correctly it was written"),
		StringLine.Contains(TEXT("Call RegisterStateFragmentField with")));

	TestTrue(TEXT("the bool, one byte and bindable, is told to register a field that reads a bool"),
		BoolLine.Contains(TEXT("Call RegisterStateFragmentField with 'bSprinting' and the type bool")));

	TestTrue(TEXT("the owner-only property is told that a crowd observer never receives it"),
		OwnerLine.Contains(TEXT("never receives the value")));
	TestFalse(TEXT("and is NOT told to register a field, which would read nothing however correctly it was written"),
		OwnerLine.Contains(TEXT("Call RegisterStateFragmentField")));
	TestTrue(TEXT("it names the marker to drop, so the remedy is on the declaration where the cause is"),
		OwnerLine.Contains(TEXT("CrowdyOwnerOnly")));

	return true;
}

/**
 * The line is said once per (class, property), and every other subject still speaks.
 *
 * The suppression has to be tight in both directions. Written too loosely it is a line per resolution;
 * written too broadly it silences a second property of the same class, or the same property name on a
 * different class, and those are separate authoring mistakes with separate remedies. The second half is
 * the one that fails quietly, because a diagnostic that never fires looks exactly like a system with
 * nothing to report.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDroppedStatePropertySaidOncePerSubjectTest,
	"CrowdySDK.Mass.DroppedStatePropertySaidOncePerSubject", CrowdyMassDroppedStateTestFlags)
bool FCrowdyMassDroppedStatePropertySaidOncePerSubjectTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDroppedStateFixture Fixture;

	FCrowdyMassStateLogCapture Capture;
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateTarget::StaticClass()))
	{
		return false;
	}

	// Two lines for two properties of one class: the memo is per property, not per class, or one of these
	// two mistakes would never be reported at all.
	TestEqual(TEXT("both unclaimed properties of the class speak the first time it resolves"),
		Capture.TakeWarnings().Num(), 2);

	// A resolution the cache answers, then one that genuinely re-runs. The second is the case that matters:
	// the decision is made again, and the line must not be.
	Fixture.Resolve(*this, UCrowdyMassDroppedStateTarget::StaticClass());
	Fixture.Subsystem->InvalidateAnimSetCachesForTest();
	Fixture.Resolve(*this, UCrowdyMassDroppedStateTarget::StaticClass());
	TestEqual(TEXT("and neither speaks again, including after the resolution cache is dropped under them"),
		Capture.TakeWarnings().Num(), 0);

	// A different class carrying a property of the SAME name. Keyed on the name alone, this second author
	// would never hear about their own class.
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateSecondTarget::StaticClass()))
	{
		return false;
	}
	const TArray<FString> OtherLines = Capture.TakeWarnings();

	if (!TestEqual(TEXT("a second class dropping a property of the same name is still reported"),
		OtherLines.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("and the line names that second class rather than the one already reported"),
		OtherLines[0].Contains(UCrowdyMassDroppedStateSecondTarget::StaticClass()->GetName()));

	return true;
}

/**
 * A dropped bitfield is described as a bitfield, not by the width it is packed into.
 *
 * This is the one property whose C++ type actively misleads. A one-bit member reports itself as the byte it
 * shares, so a message built from that type alone tells its author their uint8 is not among the types a
 * field can read, when uint8 is the first of them. Both halves of that sentence are false at once, and the
 * fix it names is one the property already satisfies, so there is no way out of it by following it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDroppedStateBitfieldSaysWhyTest,
	"CrowdySDK.Mass.DroppedStateBitfieldSaysWhy", CrowdyMassDroppedStateTestFlags)
bool FCrowdyMassDroppedStateBitfieldSaysWhyTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDroppedStateFixture Fixture;

	FCrowdyMassStateLogCapture Capture;
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateBitfieldTarget::StaticClass()))
	{
		return false;
	}
	const TArray<FString> Lines = Capture.TakeWarnings();

	if (!TestEqual(TEXT("the bitfield is reported, so it is not dropped in silence"), Lines.Num(), 1))
	{
		return false;
	}

	const FString& Line = Lines[0];

	TestTrue(TEXT("it says the property is a bitfield, which is the fact that decides it"),
		Line.Contains(TEXT("a bitfield")));
	TestTrue(TEXT("and says why that stops it being read, rather than only naming the category"),
		Line.Contains(TEXT("shares its byte")));
	TestTrue(TEXT("it names the fix that works: stop being a bitfield"),
		Line.Contains(TEXT("Declare it as a plain bool member")));
	TestFalse(TEXT("it never claims the property is declared as uint8, which would be the packing width and would read as 'uint8 is not one of uint8'"),
		Line.Contains(TEXT("is uint8")) || Line.Contains(TEXT("declared as uint8")));
	TestFalse(TEXT("and it never sends this author to register a field, since no registration can reach a bitfield"),
		Line.Contains(TEXT("Call RegisterStateFragmentField")));

	return true;
}

/**
 * The verdict command reports every dropped property and how many decisions each line now stands for.
 *
 * This is what keeps the suppression honest. The warning is written once and the drop goes on happening
 * for the life of the world, so a reader arriving later, or reading a truncated log, would otherwise be
 * told a smaller number than the truth. The count of decisions is printed beside each property for the
 * same reason: it is the figure the single line hides.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDroppedStateVerdictReportsWhatWasSuppressedTest,
	"CrowdySDK.Mass.DroppedStateVerdictReportsWhatWasSuppressed", CrowdyMassDroppedStateTestFlags)
bool FCrowdyMassDroppedStateVerdictReportsWhatWasSuppressedTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDroppedStateFixture Fixture;

	FCrowdyMassStateLogCapture Capture;
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateTarget::StaticClass()))
	{
		return false;
	}

	// A second decision about the same two properties, silent in the log by design.
	Fixture.Subsystem->InvalidateAnimSetCachesForTest();
	Fixture.Resolve(*this, UCrowdyMassDroppedStateTarget::StaticClass());
	TestEqual(TEXT("two decisions, two log lines, not four"), Capture.TakeWarnings().Num(), 2);

	Capture.TakeLogs();
	Fixture.Subsystem->LogStateVerdict();
	const TArray<FString> Verdict = Capture.TakeLogs();

	TestEqual(TEXT("the verdict names both dropped properties, however few lines the log kept"),
		CrowdyMassCountLinesContaining(Verdict, TEXT("property 'Health'")), 1);
	TestEqual(TEXT("including the one whose line scrolled away with it"),
		CrowdyMassCountLinesContaining(Verdict, TEXT("property 'Stamina'")), 1);
	TestEqual(TEXT("and reports the total rather than leaving it to be counted"),
		CrowdyMassCountLinesContaining(Verdict, TEXT("2 dropped")), 1);
	TestEqual(TEXT("with the number of decisions each single line stands for"),
		CrowdyMassCountLinesContaining(Verdict, TEXT("decided 2 time(s)")), 2);
	TestEqual(TEXT("and says nothing about the property a field claims"),
		CrowdyMassCountLinesContaining(Verdict, FString::Printf(TEXT("property '%s'"), ClaimedPropertyName)), 0);

	return true;
}

/**
 * A class every registered field claims is silent, and the verdict says so rather than saying nothing.
 *
 * The empty answer has to be printed. A reader sent to this command by the warning is asking whether their
 * property is still dropped, and a section that vanishes when the answer is no reads as the command having
 * failed to look.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDroppedStateFullyClaimedClassIsSilentTest,
	"CrowdySDK.Mass.DroppedStateFullyClaimedClassIsSilent", CrowdyMassDroppedStateTestFlags)
bool FCrowdyMassDroppedStateFullyClaimedClassIsSilentTest::RunTest(const FString& Parameters)
{
	FCrowdyMassDroppedStateFixture Fixture;

	FCrowdyMassStateLogCapture Capture;
	if (!Fixture.Resolve(*this, UCrowdyMassDroppedStateFullyClaimedTarget::StaticClass()))
	{
		return false;
	}

	TestEqual(TEXT("a class whose every replicated property reaches a field is not warned about"),
		Capture.TakeWarnings().Num(), 0);

	Capture.TakeLogs();
	Fixture.Subsystem->LogStateVerdict();
	const TArray<FString> Verdict = Capture.TakeLogs();

	TestEqual(TEXT("and the verdict answers the question rather than omitting the section"),
		CrowdyMassCountLinesContaining(Verdict, TEXT("every replicated property of every class resolved so far reaches a registered field")), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
