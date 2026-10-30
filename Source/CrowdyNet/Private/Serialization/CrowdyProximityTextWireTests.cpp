#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Messages/Communication/FTextMessageNotification.h"
#include "Messages/Communication/FTextMessageRequest.h"
#include "Serialization/CrowdyFrameSplit.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/SerializationFunctionLibrary.h"

namespace
{
	constexpr EAutomationTestFlags ProximityTextWireTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A text notification as a server writes it, opcode taken off: the unsigned spatial header, Body, the trailer.
	bool ProximityTextDecode(const TArray<uint8>& Body, FTextMessageNotification& OutMessage)
	{
		TArray<uint8> Data;
		for (const int64 Value : { int64(7), int64(6), int64(0), int64(0) })
		{
			Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(Value));
		}
		Data.Append({ 4, 0, 0 });
		Data.Append(reinterpret_cast<const uint8*>("0123456789ABCDEF0123456789ABCDEF"), 32);
		Data.Append(Body);
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1700000000000LL));
		Data.Add(9);

		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::CLIENT_TEXT_NOTIFICATION), Data);
		return CrowdyFrameSplit::ReadSpatialEnvelope(Frame) && OutMessage.DecodePayload(Frame);
	}

	FTextMessageRequest ProximityTextRequest(const TCHAR* Message)
	{
		FTextMessageRequest Request;
		Request.AppID = 7;
		Request.ChunkX = 6;
		Request.ChunkY = -1;
		Request.ChunkZ = 0;
		Request.ReplicationDistance = ECrowdyReplicationDistance::Four_Chunks;
		Request.Message = Message;
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProximityTextPayloadTest,
	"CrowdySDK.Transport.ProximityTextCarriesOnlyTheText", ProximityTextWireTestFlags)
bool FCrowdyProximityTextPayloadTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Empty = ProximityTextRequest(TEXT("")).Serialize();
	const TArray<uint8> Text = ProximityTextRequest(TEXT("hé")).Serialize();

	// The payload is the UTF-8 alone, the form CrowdyJS and CrowdyCPP send: 'h', then 0xC3 0xA9.
	const TArray<uint8> Utf8 = { 'h', 0xC3, 0xA9 };
	if (TestEqual(TEXT("the text adds exactly its UTF-8 to the message"), Text.Num() - Empty.Num(), Utf8.Num()))
	{
		TestTrue(TEXT("and ends with it"), FMemory::Memcmp(Text.GetData() + Empty.Num(), Utf8.GetData(), Utf8.Num()) == 0);
	}

	FTextMessageRequest Named = ProximityTextRequest(TEXT("hé"));
	Named.UserID = 99;
	Named.Username = TEXT("tester");
	TestTrue(TEXT("a user id and name set by an older caller are not sent"), Named.Serialize() == Text);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProximityTextDecodeTest,
	"CrowdySDK.Transport.ProximityTextDecodesUpToAZeroByte", ProximityTextWireTestFlags)
bool FCrowdyProximityTextDecodeTest::RunTest(const FString& Parameters)
{
	FTextMessageNotification Plain;
	if (TestTrue(TEXT("a UTF-8 line decodes"), ProximityTextDecode({ 'h', 0xC3, 0xA9 }, Plain)))
	{
		TestEqual(TEXT("to its text"), Plain.Message, FString(TEXT("hé")));
	}

	FTextMessageNotification Cut;
	if (TestTrue(TEXT("a line with a zero byte decodes"), ProximityTextDecode({ 'o', 'k', 0, 'x', 'y' }, Cut)))
	{
		TestEqual(TEXT("to the text before it"), Cut.Message, FString(TEXT("ok")));
	}

	FTextMessageNotification Empty;
	TestFalse(TEXT("an empty body is refused"), ProximityTextDecode({}, Empty));
	FTextMessageNotification Leading;
	TestFalse(TEXT("and so is one that starts with a zero byte"), ProximityTextDecode({ 0, 'x' }, Leading));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLocalPlayerChunkTest,
	"CrowdySDK.Transport.LocalPlayerChunkFallsBackWithoutAPawn", ProximityTextWireTestFlags)
bool FCrowdyLocalPlayerChunkTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyGameSession> Session(NewObject<UCrowdyGameSession>(Instance.Get()));
	Session->SetPlayerCurrentChunkCoordinates(3, -2, 1);
	TestTrue(TEXT("with no world there is no pawn, so the cached chunk is used"), Session->GetLocalPlayerChunk(nullptr) == FInt64Vector(3, -2, 1));
	return true;
}

#endif
