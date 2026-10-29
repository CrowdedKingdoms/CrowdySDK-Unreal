#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyNativeKit.h"

#include <cstring>
#include <limits>
#include <string>

namespace
{
	constexpr EAutomationTestFlags CrowdyNativeKitStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using FCrowdyNativeKitStateTestWrite = TFunctionRef<void(TArray<uint8>&)>;

	void CrowdyNativeKitStateTestPutStr(TArray<uint8>& Out, const std::string& Text)
	{
		Out.Add(0xd9);
		Out.Add(static_cast<uint8>(Text.size()));
		Out.Append(reinterpret_cast<const uint8*>(Text.data()), static_cast<int32>(Text.size()));
	}

	void CrowdyNativeKitStateTestPutDouble(TArray<uint8>& Out, const double Value)
	{
		uint64 Bits = 0;
		std::memcpy(&Bits, &Value, sizeof(Bits));
		Out.Add(0xcb);
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			Out.Add(static_cast<uint8>(Bits >> Shift));
		}
	}

	// A mob at the origin as MessagePack: its id written by WriteId, then one more member, Key, written by WriteValue.
	TArray<uint8> CrowdyNativeKitStateTestMob(FCrowdyNativeKitStateTestWrite WriteId, const char* Key, FCrowdyNativeKitStateTestWrite WriteValue)
	{
		TArray<uint8> Out;
		Out.Add(0x85);
		CrowdyNativeKitStateTestPutStr(Out, "kind");
		CrowdyNativeKitStateTestPutStr(Out, "mob");
		CrowdyNativeKitStateTestPutStr(Out, "id");
		WriteId(Out);
		CrowdyNativeKitStateTestPutStr(Out, "x");
		Out.Add(0x00);
		CrowdyNativeKitStateTestPutStr(Out, "z");
		Out.Add(0x00);
		CrowdyNativeKitStateTestPutStr(Out, Key);
		WriteValue(Out);
		return Out;
	}

	bool CrowdyNativeKitStateTestDecodes(FCrowdyNativeKitStateTestWrite WriteId, const char* Key, FCrowdyNativeKitStateTestWrite WriteValue,
		FCrowdyNativeKitActorState& Out)
	{
		return CrowdyNativeKit::DecodeActorState(CrowdyNativeKitStateTestMob(WriteId, Key, WriteValue), Out);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitActorWordsAreCappedTest,
	"CrowdySDK.CrowdyCppBridge.GameKit.ActorNameAndModeAreCapped", CrowdyNativeKitStateTestFlags)
bool FCrowdyNativeKitActorWordsAreCappedTest::RunTest(const FString& Parameters)
{
	const auto Id = [](TArray<uint8>& Out) { Out.Add(0x07); };
	const auto Text = [](const size_t Length)
	{
		return [Length](TArray<uint8>& Out) { CrowdyNativeKitStateTestPutStr(Out, std::string(Length, 'a')); };
	};

	FCrowdyNativeKitActorState State;
	TestTrue(TEXT("a name of 128 bytes is shown"), CrowdyNativeKitStateTestDecodes(Id, "name", Text(128), State)
		&& State.Name.IsSet() && State.Name->Len() == 128);
	TestFalse(TEXT("one of 129 drops the update"), CrowdyNativeKitStateTestDecodes(Id, "name", Text(129), State));
	// The longest name a pet can be given: 24 characters of 4 bytes each.
	const auto Wide = [](TArray<uint8>& Out)
	{
		std::string Name;
		for (int32 Index = 0; Index < 24; ++Index)
		{
			Name += "\xF0\x9F\x90\xBA";
		}
		CrowdyNativeKitStateTestPutStr(Out, Name);
	};
	TestTrue(TEXT("a pet's longest name is shown"), CrowdyNativeKitStateTestDecodes(Id, "name", Wide, State) && State.Name.IsSet() && !State.Name->IsEmpty());
	TestTrue(TEXT("a mode of 32 bytes is shown"), CrowdyNativeKitStateTestDecodes(Id, "mode", Text(32), State)
		&& State.Mode.IsSet() && State.Mode->Len() == 32);
	TestFalse(TEXT("one of 33 drops the update"), CrowdyNativeKitStateTestDecodes(Id, "mode", Text(33), State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNativeKitActorNumbersOutsideInt64Test,
	"CrowdySDK.CrowdyCppBridge.GameKit.ActorNumbersOutsideInt64AreRefused", CrowdyNativeKitStateTestFlags)
bool FCrowdyNativeKitActorNumbersOutsideInt64Test::RunTest(const FString& Parameters)
{
	const auto Id = [](TArray<uint8>& Out) { Out.Add(0x07); };
	const auto Float = [](const double Value)
	{
		return [Value](TArray<uint8>& Out) { CrowdyNativeKitStateTestPutDouble(Out, Value); };
	};
	const auto Fifty = [](TArray<uint8>& Out) { Out.Add(0x32); };
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Infinity = std::numeric_limits<double>::infinity();

	FCrowdyNativeKitActorState State;
	TestTrue(TEXT("a whole float id is read as its integer"), CrowdyNativeKitStateTestDecodes(Float(7.0), "hp", Fifty, State)
		&& State.Id == 7 && State.Hp.Get(0) == 50);
	TestFalse(TEXT("a NaN id drops the update"), CrowdyNativeKitStateTestDecodes(Float(NaN), "hp", Fifty, State));
	TestFalse(TEXT("an id past int64 drops it"), CrowdyNativeKitStateTestDecodes(Float(1.0e19), "hp", Fifty, State));
	TestFalse(TEXT("a NaN hp drops it"), CrowdyNativeKitStateTestDecodes(Id, "hp", Float(NaN), State));
	TestFalse(TEXT("an infinite hp drops it"), CrowdyNativeKitStateTestDecodes(Id, "hp", Float(Infinity), State));
	TestFalse(TEXT("an hp past int64 drops it"), CrowdyNativeKitStateTestDecodes(Id, "hp", Float(1.0e19), State));
	TestFalse(TEXT("a NaN max hp drops it"), CrowdyNativeKitStateTestDecodes(Id, "max_hp", Float(NaN), State));
	TestFalse(TEXT("a NaN owner drops it"), CrowdyNativeKitStateTestDecodes(Id, "owner", Float(NaN), State));
	TestFalse(TEXT("an owner below int64 drops it"), CrowdyNativeKitStateTestDecodes(Id, "owner", Float(-1.0e19), State));
	TestTrue(TEXT("an owner within it is read"), CrowdyNativeKitStateTestDecodes(Id, "owner", Float(-3.0), State)
		&& State.Owner.Get(0) == -3);
	return true;
}

#endif
