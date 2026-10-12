#include "CrowdyMassTransformLayoutTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "CrowdyMassLog.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

// A replicated state struct that carries a transform under names or types the Mass backend does not read
// produces no movement and no complaint: the entity spawns at the origin and stands there. The verdict
// that decided this is worked out once per struct type and then memoised, so the only place with anything
// to say is the resolution itself, and this covers that it says it, says it exactly once, says enough to
// act on, and stays quiet for every shape it does accept.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassTransformLayoutTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Collects LogCrowdyMass warnings for as long as it is in scope. The message is the deliverable here,
	// so the cases read the real log line rather than a counter kept beside it, which would still read
	// green if the line itself were dropped.
	class FCrowdyMassWarningCapture : public FOutputDevice
	{
	public:

		FCrowdyMassWarningCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FCrowdyMassWarningCapture()
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity != ELogVerbosity::Warning || Category != LogCrowdyMass.GetCategoryName())
			{
				return;
			}

			FScopeLock Lock(&Mutex);
			Lines.Add(Message);
		}

		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		TArray<FString> TakeLines()
		{
			GLog->FlushThreadedLogs();

			FScopeLock Lock(&Mutex);
			TArray<FString> Taken = MoveTemp(Lines);
			Lines.Reset();
			return Taken;
		}

	private:

		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	bool RunTransformLayoutExtract(const FInstancedStruct& State)
	{
		FVector Location = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		float SmoothingDuration = 0.0f;
		return UCrowdyMassEntitySubsystem::TryExtractTransformState(State, Location, Rotation, SmoothingDuration);
	}
}

/**
 * A struct whose transform fields are spelled differently is reported, by name, with the spellings that
 * would work.
 *
 * The failure this replaces is entirely silent: the extract returns false, the caller returns early, and
 * the entity is left at wherever it was spawned. Naming the struct is the load-bearing half, because a
 * project has many state structs and the one at fault is the only thing an author cannot infer from
 * watching a crowd stand still.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTransformLayoutUnusableStructIsReportedTest,
	"CrowdySDK.Mass.TransformLayoutUnusableStructIsReported", CrowdyMassTransformLayoutTestFlags)
bool FCrowdyMassTransformLayoutUnusableStructIsReportedTest::RunTest(const FString& Parameters)
{
	const UScriptStruct* Misnamed = FCrowdyMassTransformLayoutMisnamedState::StaticStruct();

	FCrowdyMassWarningCapture Capture;
	const bool bExtracted = RunTransformLayoutExtract(FInstancedStruct::Make<FCrowdyMassTransformLayoutMisnamedState>());
	const TArray<FString> Lines = Capture.TakeLines();

	TestFalse(TEXT("a struct with no readable transform still yields no transform"), bExtracted);

	if (!TestEqual(TEXT("and the verdict is reported exactly once rather than not at all"), Lines.Num(), 1))
	{
		return false;
	}

	const FString& Line = Lines[0];
	TestTrue(TEXT("the message names the struct at fault, which is the one thing watching a still crowd cannot tell you"),
		Line.Contains(Misnamed->GetPathName()));
	TestTrue(TEXT("it says what the consequence is, not just that something is wrong"),
		Line.Contains(TEXT("never move")));
	TestTrue(TEXT("it names the single-field spelling"), Line.Contains(TEXT("'Transform' field of type FTransform")));
	TestTrue(TEXT("and the two-field spelling, including the alternative for the first of them"),
		Line.Contains(TEXT("'Location' (or 'Position') field of type FVector")));
	TestTrue(TEXT("and the rotation half of it, with the type it must be"),
		Line.Contains(TEXT("'Rotation' field of type FRotator")));
	TestTrue(TEXT("and the optional field, so a working struct does not have to be found by trial"),
		Line.Contains(TEXT("'SmoothingDuration'")));

	// The findings are what keeps the message honest: it reports the result of each lookup that was
	// actually performed, so it cannot claim a field is missing that is merely typed differently.
	TestTrue(TEXT("it reports that this struct has no Transform field"), Line.Contains(TEXT("no 'Transform' field")));
	TestTrue(TEXT("nor a Location one"), Line.Contains(TEXT("no 'Location' field")));
	TestTrue(TEXT("nor a Position one"), Line.Contains(TEXT("no 'Position' field")));
	TestTrue(TEXT("nor a Rotation one"), Line.Contains(TEXT("no 'Rotation' field")));

	return true;
}

/**
 * A field present under the right name and the wrong type is reported as that, not as a missing field.
 *
 * Both faults arrive at the same "no transform", and they are opposite instructions: one author has to add
 * a field and the other has to change one they are already looking at. A message that merged them would
 * send half of them hunting for something that is on screen in front of them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTransformLayoutWrongTypeIsNotAMissingFieldTest,
	"CrowdySDK.Mass.TransformLayoutWrongTypeIsNotAMissingField", CrowdyMassTransformLayoutTestFlags)
bool FCrowdyMassTransformLayoutWrongTypeIsNotAMissingFieldTest::RunTest(const FString& Parameters)
{
	FCrowdyMassWarningCapture Capture;
	const bool bExtracted = RunTransformLayoutExtract(FInstancedStruct::Make<FCrowdyMassTransformLayoutWrongTypeState>());
	const TArray<FString> Lines = Capture.TakeLines();

	TestFalse(TEXT("a Location of the wrong vector type yields no transform"), bExtracted);

	if (!TestEqual(TEXT("and is reported"), Lines.Num(), 1))
	{
		return false;
	}

	const FString& Line = Lines[0];
	TestTrue(TEXT("the field that is present says what it is declared as"),
		Line.Contains(TEXT("'Location' is declared as FVector3f")));
	TestFalse(TEXT("and is not also reported as absent"), Line.Contains(TEXT("no 'Location' field")));
	TestTrue(TEXT("the field that is already correct is reported as correct, so the author changes only the other one"),
		Line.Contains(TEXT("'Rotation' is the right type")));

	return true;
}

/**
 * The verdict is said once per struct, and a different struct is still said.
 *
 * Once per subject is the whole reason this can be a warning at all: the extract runs on every update of
 * every entity, so a message that repeated would be a message nobody could leave switched on. The second
 * half is the one that fails quietly if the suppression is written too broadly, and it fails in the
 * direction that loses the report altogether.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTransformLayoutVerdictIsSaidOncePerStructTest,
	"CrowdySDK.Mass.TransformLayoutVerdictIsSaidOncePerStruct", CrowdyMassTransformLayoutTestFlags)
bool FCrowdyMassTransformLayoutVerdictIsSaidOncePerStructTest::RunTest(const FString& Parameters)
{
	const FInstancedStruct ValueOnly = FInstancedStruct::Make<FCrowdyMassTransformLayoutValueOnlyState>();

	FCrowdyMassWarningCapture Capture;

	RunTransformLayoutExtract(ValueOnly);
	const TArray<FString> FirstLines = Capture.TakeLines();
	TestEqual(TEXT("the first update carrying an unreadable struct reports it"), FirstLines.Num(), 1);

	RunTransformLayoutExtract(ValueOnly);
	RunTransformLayoutExtract(ValueOnly);
	const TArray<FString> RepeatLines = Capture.TakeLines();
	TestEqual(TEXT("and every update after it is silent about the same struct"), RepeatLines.Num(), 0);

	// A second struct, unreadable in a different way, proving the silence above is scoped to the struct
	// that was already reported rather than to the diagnostic as a whole.
	RunTransformLayoutExtract(FInstancedStruct::Make<FCrowdyMassTransformLayoutRotationOnlyState>());
	const TArray<FString> OtherLines = Capture.TakeLines();

	if (!TestEqual(TEXT("a different unreadable struct is still reported"), OtherLines.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("and the report names that second struct, not the one already reported"),
		OtherLines[0].Contains(FCrowdyMassTransformLayoutRotationOnlyState::StaticStruct()->GetPathName()));
	TestTrue(TEXT("naming the field it does have as correct and its partner as missing"),
		OtherLines[0].Contains(TEXT("'Rotation' is the right type")) && OtherLines[0].Contains(TEXT("no 'Location' field")));

	return true;
}

/**
 * Every shape the read accepts is read, and says nothing.
 *
 * A warning that also fired on working structs would be worse than none: it would train an author to
 * ignore the line that names their actual mistake. All three accepted spellings are covered, because a
 * check written against only one of them would condemn the other two.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassTransformLayoutAcceptedSpellingsAreSilentTest,
	"CrowdySDK.Mass.TransformLayoutAcceptedSpellingsAreSilent", CrowdyMassTransformLayoutTestFlags)
bool FCrowdyMassTransformLayoutAcceptedSpellingsAreSilentTest::RunTest(const FString& Parameters)
{
	FCrowdyMassWarningCapture Capture;

	TestTrue(TEXT("a Location plus Rotation struct is read"),
		RunTransformLayoutExtract(FInstancedStruct::Make<FCrowdyMassTransformLayoutLocationState>()));
	TestTrue(TEXT("Position is accepted in place of Location"),
		RunTransformLayoutExtract(FInstancedStruct::Make<FCrowdyMassTransformLayoutPositionState>()));
	TestTrue(TEXT("and a single Transform field is read"),
		RunTransformLayoutExtract(FInstancedStruct::Make<FCrowdyMassTransformLayoutTransformState>()));

	TestEqual(TEXT("and none of them is warned about"), Capture.TakeLines().Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
