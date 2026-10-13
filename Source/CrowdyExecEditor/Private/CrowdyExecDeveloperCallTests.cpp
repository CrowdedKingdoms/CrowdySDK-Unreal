#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "CrowdyCppClient.h"
#include "CrowdyExecDeveloperCall.h"
#include "CrowdyExecEditorTestTypes.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyExecDeveloperCallTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("t_tool");
		Definition->State = FCrowdyExecTestRecordRow::StaticStruct();
		FCrowdyServerFunction& Fix = Definition->Functions.AddDefaulted_GetRef();
		Fix.Name = TEXT("Fix");
		Fix.WhoCanCall = ECrowdyServerFunctionCaller::DevelopersOnly;
		Fix.Params = FCrowdyExecTestRecordHit::StaticStruct();
		Fix.Reply = FCrowdyExecTestRecordHit::StaticStruct();
		FCrowdyServerFunction& Open = Definition->Functions.AddDefaulted_GetRef();
		Open.Name = TEXT("Open");
		return Definition;
	}

	bool PumpUntil(FCrowdyCppClient& Client, TFunctionRef<bool()> Done)
	{
		const double Until = FPlatformTime::Seconds() + 3.0;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Until)
			{
				return false;
			}
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			Client.Poll();
			FPlatformProcess::Sleep(0.005f);
		}
		return true;
	}

	TArray<uint8> FixStr(const ANSICHAR* Text)
	{
		const int32 Length = FCStringAnsi::Strlen(Text);
		TArray<uint8> Bytes = {static_cast<uint8>(0xa0 | Length)};
		Bytes.Append(reinterpret_cast<const uint8*>(Text), Length);
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperCallInputsTest, "CrowdySDK.CrowdyExecEditor.DeveloperCallReadsInputs", CrowdyExecDeveloperCallTests::TestFlags)
bool FCrowdyExecDeveloperCallInputsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperCallTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition();
	const FCrowdyServerFunction& Fix = Definition->Functions[0];
	const FCrowdyServerFunction& Open = Definition->Functions[1];
	TestTrue(TEXT("a Developers Only function is called as a developer"), CrowdyExecDeveloperCall::IsForDevelopers(Fix));
	TestFalse(TEXT("a function players can call is not"), CrowdyExecDeveloperCall::IsForDevelopers(Open));

	FInstancedStruct Inputs;
	FString Error;
	if (TestTrue(TEXT("inputs as JSON"), CrowdyExecDeveloperCall::InputsFromJson(Fix, TEXT("{\"Target\":\"crate-7\",\"Amount\":5}"), Inputs, Error)))
	{
		const FCrowdyExecTestRecordHit* Hit = Inputs.GetPtr<FCrowdyExecTestRecordHit>();
		TestTrue(TEXT("are read into the input struct"), Hit && Hit->Target == TEXT("crate-7") && Hit->Amount == 5);
	}
	TestTrue(TEXT("no JSON keeps the defaults"), CrowdyExecDeveloperCall::InputsFromJson(Fix, FString(), Inputs, Error) && Inputs.Get<FCrowdyExecTestRecordHit>().Amount == 0);
	TestFalse(TEXT("JSON that is not an object is refused"), CrowdyExecDeveloperCall::InputsFromJson(Fix, TEXT("[1,2]"), Inputs, Error));
	TestFalse(TEXT("a misspelt input is refused rather than sent as its default"), CrowdyExecDeveloperCall::InputsFromJson(Fix, TEXT("{\"Amout\":5}"), Inputs, Error));
	TestTrue(TEXT("naming it"), Error.Contains(TEXT("Amout")));
	TestTrue(TEXT("an input's name in any case is read"), CrowdyExecDeveloperCall::InputsFromJson(Fix, TEXT("{\"amount\":3}"), Inputs, Error) && Inputs.Get<FCrowdyExecTestRecordHit>().Amount == 3);
	TestFalse(TEXT("inputs for a function that takes none are refused"), CrowdyExecDeveloperCall::InputsFromJson(Open, TEXT("{\"Amount\":1}"), Inputs, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperCallInt64Test, "CrowdySDK.CrowdyExecEditor.DeveloperCallReadsLargeInt64Exactly", CrowdyExecDeveloperCallTests::TestFlags)
bool FCrowdyExecDeveloperCallInt64Test::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperCallTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition();
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	const FCrowdyServerFunction* AddMember = Definition->FindFunction(TEXT("AddMember"));
	if (!TestNotNull(TEXT("AddMember is found"), AddMember))
	{
		return false;
	}
	FInstancedStruct Inputs;
	FString Error;
	const bool bString = CrowdyExecDeveloperCall::InputsFromJson(*AddMember, TEXT("{\"Player\":\"9007199254740993\"}"), Inputs, Error);
	TestTrue(TEXT("an id written as a string is read exactly"), bString && Inputs.Get<FCrowdyServerMemberInputs>().Player == 9007199254740993);
	Error.Reset();
	TestFalse(TEXT("an id written as a number above 2^53 is refused"), CrowdyExecDeveloperCall::InputsFromJson(*AddMember, TEXT("{\"Player\":9007199254740993}"), Inputs, Error));
	TestTrue(TEXT("naming the input"), Error.Contains(TEXT("Player")));
	TestTrue(TEXT("a small id written as a number is read"), CrowdyExecDeveloperCall::InputsFromJson(*AddMember, TEXT("{\"Player\":123}"), Inputs, Error) && Inputs.Get<FCrowdyServerMemberInputs>().Player == 123);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperCallBuiltInTest, "CrowdySDK.CrowdyExecEditor.DeveloperCallFindsBuiltInMemberFunctions", CrowdyExecDeveloperCallTests::TestFlags)
bool FCrowdyExecDeveloperCallBuiltInTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperCallTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition();
	TestNull(TEXT("a built-in is not found without members"), Definition->FindFunction(TEXT("AddMember")));
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	const FCrowdyServerFunction* AddMember = Definition->FindFunction(TEXT("AddMember"));
	if (TestNotNull(TEXT("a built-in is found by name on a definition with its own members"), AddMember))
	{
		TestTrue(TEXT("and is called as a developer"), CrowdyExecDeveloperCall::IsForDevelopers(*AddMember));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeveloperCallRoundTripTest, "CrowdySDK.CrowdyExecEditor.DeveloperCallSendsAndReadsTheReply", CrowdyExecDeveloperCallTests::TestFlags)
bool FCrowdyExecDeveloperCallRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeveloperCallTests;
	const TSharedPtr<FCrowdyCppClient> Bridge = FCrowdyCppClient::MakeForTest(
		TEXT("{\"data\":{\"execConnectAsDeveloper\":{\"gatewayUrl\":\"wss://gateway.example.test\",\"token\":\"dev-connect\",\"host\":\"host-a\",\"expiresAt\":\"2026-01-01T00:00:00Z\"}}}"), 200);
	if (!TestTrue(TEXT("a test client was made"), Bridge.IsValid()))
	{
		return false;
	}
	const TSharedRef<CrowdyExecDeveloper::FClient> Client = CrowdyExecDeveloper::FClient::CreateForClient(Bridge.ToSharedRef(), 42);
	Client->SetSessionTokenForTest(TEXT("session-token"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition();
	TArray<FString> BakeErrors;
	if (!TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(BakeErrors, TEXT("; "))), Definition->Bake(BakeErrors)))
	{
		return false;
	}
	FInstancedStruct Inputs;
	FString Error;
	CrowdyExecDeveloperCall::InputsFromJson(Definition->Functions[0], TEXT("{\"Target\":\"crate-7\",\"Amount\":5}"), Inputs, Error);

	TOptional<CrowdyExecDeveloperCall::FResult> NoKey;
	CrowdyExecDeveloperCall::Call(*Client, *Definition, Definition->Functions[0], FString(), Inputs, [&NoKey](const CrowdyExecDeveloperCall::FResult& Result) { NoKey = Result; });
	TestTrue(TEXT("a call with no Instance Id is refused at once"), NoKey.IsSet() && !NoKey->bOk && NoKey->Status == TEXT("BadRequest"));
	TestEqual(TEXT("and dials nothing"), Bridge->NumTestExecConnections(), 0);

	TOptional<CrowdyExecDeveloperCall::FResult> Answer;
	CrowdyExecDeveloperCall::Call(*Client, *Definition, Definition->Functions[0], TEXT("k-1"), Inputs,
		[&Answer](const CrowdyExecDeveloperCall::FResult& Result) { Answer = Result; });
	if (!TestFalse(Answer.IsSet() ? FString::Printf(TEXT("answered before sending: %s %s"), *Answer->Status, *Answer->Message) : FString(TEXT("not answered before sending")), Answer.IsSet())
		|| !TestTrue(TEXT("the developer connection dialled the gateway"), PumpUntil(*Bridge, [&Bridge]() { return Bridge->NumTestExecConnections() >= 1; })))
	{
		return false;
	}
	Bridge->TestExecOpen();
	TArray<TArray<uint8>> Sent;
	PumpUntil(*Bridge, [&Bridge, &Sent]() { Sent.Append(Bridge->TakeTestExecSentFrames()); return !Sent.IsEmpty(); });
	if (!TestEqual(TEXT("one call was sent"), Sent.Num(), 1) || !TestTrue(TEXT("with a request id"), Sent[0].Num() >= 5))
	{
		return false;
	}
	const TArray<uint8>& Frame = Sent[0];
	const FString Text = BytesToHex(Frame.GetData(), Frame.Num());
	TestTrue(TEXT("the call names the method"), Text.Contains(BytesToHex(reinterpret_cast<const uint8*>("fix"), 3)));
	TestTrue(TEXT("and carries the inputs"), Text.Contains(BytesToHex(reinterpret_cast<const uint8*>("crate-7"), 7)));

	TArray<uint8> Reply = {0x81, Frame[1], Frame[2], Frame[3], Frame[4], 0x00, 0x82};
	Reply.Append(FixStr("Target"));
	Reply.Append(FixStr("crate-7"));
	Reply.Append(FixStr("Amount"));
	Reply.Add(0x06);
	Bridge->TestExecReceiveBinary(Reply);
	if (!TestTrue(TEXT("the call is answered"), PumpUntil(*Bridge, [&Answer]() { return Answer.IsSet(); })))
	{
		return false;
	}
	TestTrue(FString::Printf(TEXT("as Ok (%s: %s)"), *Answer->Status, *Answer->Message), Answer->bOk);
	TestTrue(FString::Printf(TEXT("with the reply as JSON (%s)"), *Answer->ReplyJson), Answer->ReplyJson.Contains(TEXT("\"amount\":6")));
	return true;
}

#endif
