#pragma once

#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "CrowdyCppClient.h"
#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecTestTypes.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Network/CrowdyCpp/CrowdyCppClientSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServerObjectTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Every request of the test client answers with this gateway.
	inline constexpr const TCHAR* ConnectAnswer =
		TEXT("{\"data\":{\"execConnect\":{\"gatewayUrl\":\"wss://gateway.example.test\",\"token\":\"connect-1\",\"host\":\"host-a\",\"expiresAt\":\"2026-01-01T00:00:00Z\"}}}");

	inline constexpr const TCHAR* TypeName = TEXT("test_boss");
	inline constexpr const TCHAR* AttackMethod = TEXT("attack_boss");
	inline constexpr const TCHAR* AttackFunction = TEXT("AttackBoss");
	inline constexpr const TCHAR* ConnectFailedWarning = TEXT("Server Objects could not connect to the server");
	constexpr uint64 Epoch = 1759000000000ull;
	constexpr uint8 KindCall = 0x01;
	constexpr uint8 KindSubscribe = 0x02;
	constexpr uint8 KindUnsubscribe = 0x03;

	struct FSentFrame
	{
		TArray<uint8> Payload;
		FString NodeType;
		FString Key;
		/** The method of a call, the topic of a subscribe or unsubscribe. */
		FString Name;
		uint32 Rid = 0;
		uint8 Kind = 0;
	};

	inline bool ReadText(const TArray<uint8>& Bytes, int32& At, int32 Width, FString& Out)
	{
		if (At + Width > Bytes.Num())
		{
			return false;
		}
		const int32 Length = Width == 2 ? Bytes[At] | (Bytes[At + 1] << 8) : Bytes[At];
		At += Width;
		if (At + Length > Bytes.Num())
		{
			return false;
		}
		Out = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData() + At), Length);
		At += Length;
		return true;
	}

	inline bool ParseFrame(const TArray<uint8>& Bytes, FSentFrame& Out)
	{
		if (Bytes.Num() < 5 || Bytes[0] < KindCall || Bytes[0] > KindUnsubscribe)
		{
			return false;
		}
		Out.Kind = Bytes[0];
		Out.Rid = Bytes[1] | (Bytes[2] << 8) | (Bytes[3] << 16) | (static_cast<uint32>(Bytes[4]) << 24);
		int32 At = 5;
		if (!ReadText(Bytes, At, 1, Out.NodeType) || !ReadText(Bytes, At, 2, Out.Key) || !ReadText(Bytes, At, 1, Out.Name))
		{
			return false;
		}
		Out.Payload = TArray<uint8>(Bytes.GetData() + At, Bytes.Num() - At);
		return true;
	}

	inline TArray<uint8> ReplyFrame(uint32 Rid, ECrowdyNativeExecStatus Status, const TArray<uint8>& Payload)
	{
		TArray<uint8> Frame = {0x81, static_cast<uint8>(Rid), static_cast<uint8>(Rid >> 8), static_cast<uint8>(Rid >> 16),
			static_cast<uint8>(Rid >> 24), static_cast<uint8>(Status)};
		Frame.Append(Payload);
		return Frame;
	}

	inline TArray<uint8> PushFrame(const FString& Key, const TArray<uint8>& Payload)
	{
		const FTCHARToUTF8 Type(TypeName);
		const FTCHARToUTF8 InstanceKey(*Key);
		TArray<uint8> Frame = {0x82, static_cast<uint8>(Type.Length())};
		Frame.Append(reinterpret_cast<const uint8*>(Type.Get()), Type.Length());
		Frame.Add(static_cast<uint8>(InstanceKey.Length()));
		Frame.Add(static_cast<uint8>(InstanceKey.Length() >> 8));
		Frame.Append(reinterpret_cast<const uint8*>(InstanceKey.Get()), InstanceKey.Length());
		Frame.Append({0x05, 's', 't', 'a', 't', 'e'});
		Frame.Append(Payload);
		return Frame;
	}

	inline TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text, Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	inline void WriteKey(FCrowdyExecWriter& Writer, const ANSICHAR* Key)
	{
		Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Key), FCStringAnsi::Strlen(Key)));
	}

	inline FCrowdyExecTestBossState Boss(int32 Health, bool bDefeated, ECrowdyExecTestPhase Phase)
	{
		FCrowdyExecTestBossState State;
		State.Health = Health;
		State.bDefeated = bDefeated;
		State.Phase = Phase;
		return State;
	}

	inline TArray<FString> AllWatched()
	{
		return {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")};
	}

	inline bool NamesAre(const TArray<FString>& Names, const TArray<FString>& Expected)
	{
		if (Names.Num() != Expected.Num())
		{
			return false;
		}
		for (const FString& Name : Expected)
		{
			if (!Names.ContainsByPredicate([&Name](const FString& Other) { return Other.Equals(Name, ESearchCase::CaseSensitive); }))
			{
				return false;
			}
		}
		return true;
	}

	inline FString Joined(const TArray<FString>& Names)
	{
		return FString::Join(Names, TEXT(", "));
	}

	inline const FCrowdyExecTestBossState* StateOf(const UCrowdyServerObject* Object)
	{
		return Object ? Object->GetState().GetPtr<FCrowdyExecTestBossState>() : nullptr;
	}

	inline int32 HealthOf(const UCrowdyServerObject* Object)
	{
		const FCrowdyExecTestBossState* State = StateOf(Object);
		return State ? State->Health : -1;
	}

	inline TStrongObjectPtr<UCrowdyServerObjectTestOwner> MakeOwner()
	{
		return TStrongObjectPtr<UCrowdyServerObjectTestOwner>(NewObject<UCrowdyServerObjectTestOwner>(GetTransientPackage(), NAME_None, RF_Transient));
	}

	inline void Destroy(TStrongObjectPtr<UCrowdyServerObjectTestOwner>& Owner)
	{
		Owner->MarkAsGarbage();
		Owner.Reset();
	}

	inline FInstancedStruct AttackParams(int32 Damage)
	{
		FCrowdyServerObjectTestAttackParams Params;
		Params.Damage = Damage;
		Params.Weapon = TEXT("axe");
		return FInstancedStruct::Make(Params);
	}

	struct FValuesSpy
	{
		TSharedRef<TArray<TArray<FString>>> Events = MakeShared<TArray<TArray<FString>>>();

		FDelegateHandle Watch(UCrowdyServerObject* Object) const
		{
			TSharedRef<TArray<TArray<FString>>> Out = Events;
			return Object->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateLambda([Out](UCrowdyServerObject*, TConstArrayView<FString> Fields)
			{
				TArray<FString>& Names = Out->AddDefaulted_GetRef();
				Names.Append(Fields.GetData(), Fields.Num());
			}));
		}

		int32 Num() const { return Events->Num(); }
		TArray<FString> Last() const { return Events->IsEmpty() ? TArray<FString>() : Events->Last(); }
	};

	struct FStatusSpy
	{
		TSharedRef<TArray<ECrowdyServerObjectStatus>> Seen = MakeShared<TArray<ECrowdyServerObjectStatus>>();

		void Listen(UCrowdyServerObject* Object) const
		{
			TSharedRef<TArray<ECrowdyServerObjectStatus>> Out = Seen;
			Object->OnStatusChanged.AddLambda([Out](UCrowdyServerObject*, ECrowdyServerObjectStatus Status) { Out->Add(Status); });
		}
	};

	struct FCallSpy
	{
		TSharedRef<TArray<FCrowdyServerCallResult>> Results = MakeShared<TArray<FCrowdyServerCallResult>>();

		TFunction<void(const FCrowdyServerCallResult&)> OnDone() const
		{
			TSharedRef<TArray<FCrowdyServerCallResult>> Out = Results;
			return [Out](const FCrowdyServerCallResult& Result) { Out->Add(Result); };
		}

		int32 Num() const { return Results->Num(); }
	};

	/** The members keys a scripted read reply or push carries; an unset one is left out, and a Leader of 0 is sent as nil. */
	struct FMembersKeys
	{
		TOptional<TArray<int64>> Members;
		TOptional<int32> MemberCount;
		TOptional<int64> Leader;
		TOptional<bool> bOpen;
		TOptional<bool> bIsMember;
		TOptional<bool> bIsLeader;
		/** Sent as the value of a second members key, for malformed messages. */
		TOptional<TArray<uint8>> RawMembers;

		int32 Num() const
		{
			return static_cast<int32>(Members.IsSet()) + static_cast<int32>(MemberCount.IsSet()) + static_cast<int32>(Leader.IsSet())
				+ static_cast<int32>(bOpen.IsSet()) + static_cast<int32>(bIsMember.IsSet()) + static_cast<int32>(bIsLeader.IsSet())
				+ static_cast<int32>(RawMembers.IsSet());
		}

		void Write(TArray<uint8>& Bytes) const
		{
			FCrowdyExecWriter Writer(Bytes);
			if (Members.IsSet())
			{
				WriteKey(Writer, "members");
				Writer.ArrayHeader(Members->Num());
				for (const int64 Id : *Members)
				{
					Writer.Int(Id);
				}
			}
			if (MemberCount.IsSet())
			{
				WriteKey(Writer, "member_count");
				Writer.Int(*MemberCount);
			}
			if (Leader.IsSet())
			{
				WriteKey(Writer, "leader");
				if (*Leader == 0)
				{
					Writer.Nil();
				}
				else
				{
					Writer.Int(*Leader);
				}
			}
			WriteFlag(Writer, "open", bOpen);
			WriteFlag(Writer, "is_member", bIsMember);
			WriteFlag(Writer, "is_leader", bIsLeader);
			if (RawMembers.IsSet())
			{
				WriteKey(Writer, "members");
				Bytes.Append(*RawMembers);
			}
		}

		static void WriteFlag(FCrowdyExecWriter& Writer, const ANSICHAR* Key, const TOptional<bool>& Flag)
		{
			if (Flag.IsSet())
			{
				WriteKey(Writer, Key);
				Writer.Bool(*Flag);
			}
		}
	};

	/** A game instance with a signed-in session, a scripted gateway client, the subsystem and the test_boss definition. */
	struct FRig
	{
		FAutomationTestBase& Test;
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyCppClientSubsystem> Host;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyServerObjectSubsystem> Subsystem;
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition;
		TSharedPtr<FCrowdyCppClient> Client;
		TArray<FSentFrame> Sent;
		FString InstanceId = TEXT("boss-1");
		bool bBaked = false;
		bool bDeinitialized = false;
		bool bShutDown = false;

		explicit FRig(FAutomationTestBase& InTest, ECrowdyServerObjectVisibility Visibility = ECrowdyServerObjectVisibility::Public,
			ECrowdyServerMembersSource MembersFrom = ECrowdyServerMembersSource::None)
			: Test(InTest)
		{
			Instance.Reset(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
			Host.Reset(NewObject<UCrowdyCppClientSubsystem>(Instance.Get()));
			Session.Reset(NewObject<UCrowdyGameSession>(Instance.Get()));
			Session->SetAppID(42);
			Session->SetGameToken(TEXT("app-token"));
			Client = FCrowdyCppClient::MakeForTest(ConnectAnswer, 200);
			if (Client.IsValid())
			{
				Host->SetClientForTest(Client, FCrowdyCppClientConfig());
			}
			Subsystem.Reset(NewObject<UCrowdyServerObjectSubsystem>(Instance.Get()));
			Subsystem->InitializeForTest(Host.Get(), Session.Get());

			Definition.Reset(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
			Definition->TypeName = TypeName;
			Definition->Visibility = Visibility;
			Definition->MembersFrom = MembersFrom;
			Definition->MaxMembers = MembersFrom == ECrowdyServerMembersSource::ThisObject ? CrowdyExec::MaxElements : 0;
			Definition->State = FCrowdyExecTestBossState::StaticStruct();
			Definition->WatchedFields = {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")};
			FCrowdyServerFunction& Attack = Definition->Functions.AddDefaulted_GetRef();
			Attack.Name = AttackFunction;
			Attack.Params = FCrowdyServerObjectTestAttackParams::StaticStruct();
			Attack.Reply = FCrowdyServerObjectTestAttackReply::StaticStruct();
			TArray<FString> Errors;
			bBaked = Definition->Bake(Errors);
			Test.TestTrue(FString::Printf(TEXT("the test definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked);
		}

		~FRig()
		{
			Shutdown();
		}

		bool IsValid() const
		{
			return Test.TestTrue(TEXT("the scripted gateway client was built"), Client.IsValid()) && bBaked;
		}

		void Deinitialize()
		{
			if (bDeinitialized)
			{
				return;
			}
			bDeinitialized = true;
			Subsystem->Deinitialize();
		}

		// Closed while still installed, and polled once, so every canceled completion is delivered while the rig lives.
		void Shutdown()
		{
			if (bShutDown)
			{
				return;
			}
			bShutDown = true;
			Deinitialize();
			if (Client.IsValid())
			{
				Client->Close();
				Client->Poll();
			}
			Host->SetClientForTest(nullptr, FCrowdyCppClientConfig());
			Client.Reset();
		}

		void Pump()
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			Client->Poll();
			for (const TArray<uint8>& Bytes : Client->TakeTestExecSentFrames())
			{
				FSentFrame Frame;
				if (ParseFrame(Bytes, Frame))
				{
					Sent.Add(MoveTemp(Frame));
				}
			}
		}

		bool PumpUntil(TFunctionRef<bool()> Done, double Seconds = 3.0)
		{
			const double Until = FPlatformTime::Seconds() + Seconds;
			for (;;)
			{
				Pump();
				if (Done())
				{
					return true;
				}
				if (FPlatformTime::Seconds() > Until)
				{
					return false;
				}
				FPlatformProcess::Sleep(0.005f);
			}
		}

		void PumpFor(double Seconds)
		{
			PumpUntil([]() { return false; }, Seconds);
		}

		void Tick(float Seconds, float Step = 0.5f)
		{
			for (float Left = Seconds; Left > UE_KINDA_SMALL_NUMBER; Left -= Step)
			{
				Subsystem->TickForTest(FMath::Min(Step, Left));
			}
		}

		int32 FindFrame(uint8 Kind, const FString& Name) const
		{
			return Sent.IndexOfByPredicate([Kind, &Name](const FSentFrame& Frame)
			{
				return Frame.Kind == Kind && Frame.Name.Equals(Name, ESearchCase::CaseSensitive);
			});
		}

		/** Pumps until a frame of Kind named Name was sent, and takes it. */
		bool WaitForFrame(uint8 Kind, const FString& Name, FSentFrame& Out, double Seconds = 3.0)
		{
			if (!PumpUntil([this, Kind, &Name]() { return FindFrame(Kind, Name) != INDEX_NONE; }, Seconds))
			{
				return false;
			}
			const int32 Index = FindFrame(Kind, Name);
			Out = Sent[Index];
			Sent.RemoveAt(Index);
			return true;
		}

		/** Pumps a short while and reports whether any frame of Kind named Name went out. */
		bool SentAny(uint8 Kind, const FString& Name)
		{
			PumpFor(0.15);
			return FindFrame(Kind, Name) != INDEX_NONE;
		}

		bool SentAnyCall()
		{
			PumpFor(0.15);
			return Sent.ContainsByPredicate([](const FSentFrame& Frame) { return Frame.Kind == KindCall; });
		}

		bool IsAddressed(const FSentFrame& Frame) const
		{
			return Frame.NodeType.Equals(TypeName, ESearchCase::CaseSensitive) && Frame.Key.Equals(InstanceId, ESearchCase::CaseSensitive);
		}

		void Reply(uint32 Rid, ECrowdyNativeExecStatus Status, const TArray<uint8>& Payload)
		{
			Client->TestExecReceiveBinary(ReplyFrame(Rid, Status, Payload));
		}

		void Push(const TArray<uint8>& Payload)
		{
			Client->TestExecReceiveBinary(PushFrame(InstanceId, Payload));
		}

		/** Acknowledges and takes every subscribe sent so far. */
		void AckSubscribes()
		{
			for (int32 Index = FindFrame(KindSubscribe, TEXT("state")); Index != INDEX_NONE; Index = FindFrame(KindSubscribe, TEXT("state")))
			{
				Reply(Sent[Index].Rid, ECrowdyNativeExecStatus::Ok, {});
				Sent.RemoveAt(Index);
			}
		}

		template <typename T>
		TArray<uint8> Encode(const T& Value, TConstArrayView<FString> OnlyFields = {})
		{
			TArray<uint8> Bytes;
			FString Error;
			const bool bEncoded = CrowdyExec::Encode(*Definition, T::StaticStruct(), &Value, Bytes, Error, OnlyFields);
			Test.TestTrue(FString::Printf(TEXT("the test's %s encodes (%s)"), *T::StaticStruct()->GetName(), *Error), bEncoded);
			return Bytes;
		}

		/** A state message: a read reply carries the contract; a push without Values carries no fields at all. */
		TArray<uint8> StateMessage(bool bRead, uint64 MessageEpoch, uint64 Seq, const FCrowdyExecTestBossState* Values, TConstArrayView<FString> Fields,
			const FMembersKeys& Keys = FMembersKeys())
		{
			TArray<uint8> Bytes;
			FCrowdyExecWriter Writer(Bytes);
			Writer.MapHeader((bRead ? 3 : 2) + (Values ? 1 : 0) + Keys.Num());
			if (bRead)
			{
				WriteKey(Writer, "contract");
				Writer.UInt(CrowdyExec::ContractVersion);
			}
			WriteKey(Writer, "epoch");
			Writer.UInt(MessageEpoch);
			WriteKey(Writer, "seq");
			Writer.UInt(Seq);
			if (Values)
			{
				WriteKey(Writer, "fields");
				Bytes.Append(Encode(*Values, Fields));
			}
			Keys.Write(Bytes);
			return Bytes;
		}

		TArray<uint8> ReadMessage(uint64 Seq, const FCrowdyExecTestBossState& Values, const FMembersKeys& Keys = FMembersKeys())
		{
			return StateMessage(true, Epoch, Seq, &Values, AllWatched(), Keys);
		}

		TArray<uint8> PushMessage(uint64 MessageEpoch, uint64 Seq, const FCrowdyExecTestBossState& Values, TConstArrayView<FString> Fields)
		{
			return StateMessage(false, MessageEpoch, Seq, &Values, Fields);
		}

		/** A read reply with an empty fields map, as a non-member of a Members object gets. */
		TArray<uint8> EmptyFieldsRead(uint64 Seq, const FMembersKeys& Keys)
		{
			TArray<uint8> Bytes;
			FCrowdyExecWriter Writer(Bytes);
			Writer.MapHeader(4 + Keys.Num());
			WriteKey(Writer, "contract");
			Writer.UInt(CrowdyExec::ContractVersion);
			WriteKey(Writer, "epoch");
			Writer.UInt(Epoch);
			WriteKey(Writer, "seq");
			Writer.UInt(Seq);
			WriteKey(Writer, "fields");
			Writer.MapHeader(0);
			Keys.Write(Bytes);
			return Bytes;
		}

		UCrowdyServerObject* Acquire(const FString& Id, const UObject* Owner)
		{
			FString Error;
			UCrowdyServerObject* Object = Subsystem->Acquire(Definition.Get(), Id, Owner, Error);
			Test.TestTrue(FString::Printf(TEXT("%s is acquired (%s)"), *Id, *Error), Object != nullptr);
			return Object;
		}

		/** Opens the scripted gateway socket once the subsystem has dialled it, and waits for the subsystem to see it. */
		bool Open()
		{
			if (!Test.TestTrue(TEXT("acquiring dials the gateway"), PumpUntil([this]() { return Client->NumTestExecConnections() >= 1; })))
			{
				return false;
			}
			Client->TestExecOpen();
			return Test.TestTrue(TEXT("the subsystem reports the open connection"), PumpUntil([this]() { return Subsystem->GetConnection() != nullptr; }));
		}

		/** Acknowledges the object's subscribe and answers its first read with Values at Seq. */
		bool AnswerFirstRead(UCrowdyServerObject* Object, const FCrowdyExecTestBossState& Values, uint64 Seq, const FMembersKeys& Keys = FMembersKeys())
		{
			FSentFrame Subscribe;
			FSentFrame Read;
			if (!Test.TestTrue(TEXT("the object subscribes to its watched values"), WaitForFrame(KindSubscribe, TEXT("state"), Subscribe))
				|| !Test.TestTrue(TEXT("the object reads its watched values"), WaitForFrame(KindCall, TEXT("read"), Read)))
			{
				return false;
			}
			Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
			Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, ReadMessage(Seq, Values, Keys));
			return Test.TestTrue(TEXT("the first read makes the object Ready"),
				PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }));
		}

		UCrowdyServerObject* AcquireReady(const UObject* Owner, const FCrowdyExecTestBossState& Values, uint64 Seq, const FMembersKeys& Keys = FMembersKeys())
		{
			UCrowdyServerObject* Object = Acquire(InstanceId, Owner);
			if (!Object || !Open() || !AnswerFirstRead(Object, Values, Seq, Keys))
			{
				return nullptr;
			}
			return Object;
		}
	};

	inline int32 DialsOf(const FRig& Rig)
	{
		return Rig.Client->NumTestExecConnections();
	}

	/** Waits for dial number Dial to reach the gateway, then closes it before it opens, which fails the connect. */
	inline bool FailDial(FRig& Rig, int32 Dial)
	{
		if (!Rig.Test.TestTrue(FString::Printf(TEXT("dial %d reaches the gateway"), Dial), Rig.PumpUntil([&Rig, Dial]() { return DialsOf(Rig) >= Dial; })))
		{
			return false;
		}
		Rig.Client->TestExecCloseFromServer(1006, false);
		Rig.PumpFor(0.2);
		return true;
	}
}

#endif
