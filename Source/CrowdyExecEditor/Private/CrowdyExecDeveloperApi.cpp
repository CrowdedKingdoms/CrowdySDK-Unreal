#include "CrowdyExecDeveloperApi.h"

#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "CrowdyStudioModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyExecDevApi
{
	using namespace CrowdyExecDeveloper;

	const TCHAR* const PlatformBusyCode = TEXT("PLATFORM_BUSY");
	const TCHAR* const WrongDatacenterCode = TEXT("WRONG_DATACENTER");
	const FString HttpsScheme = TEXT("https://");

	// Reads the fields of one answer object; the first missing or mistyped field is kept and every read after it is harmless.
	struct FAnswerReader
	{
		explicit FAnswerReader(const FJsonObject& InJson)
			: Json(InJson)
		{
		}

		const FJsonObject& Json;
		FString BadField;
		FString Problem;

		void Fail(const FString& Field)
		{
			if (BadField.IsEmpty())
			{
				BadField = Field;
			}
		}

		bool IsNull(const TCHAR* Field) const
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Field);
			return !Value.IsValid() || Value->IsNull();
		}

		TSharedPtr<FJsonValue> Get(const TCHAR* Field, EJson Type, bool bNullable)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Field);
			if (!Value.IsValid() || Value->IsNull())
			{
				if (!bNullable)
				{
					Fail(Field);
				}
				return nullptr;
			}
			if (Value->Type != Type)
			{
				Fail(Field);
				return nullptr;
			}
			return Value;
		}

		FString String(const TCHAR* Field, bool bNullable = false)
		{
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::String, bNullable);
			return Value.IsValid() ? Value->AsString() : FString();
		}

		bool Flag(const TCHAR* Field)
		{
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::Boolean, false);
			return Value.IsValid() && Value->AsBool();
		}

		double Number(const TCHAR* Field)
		{
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::Number, false);
			return Value.IsValid() ? Value->AsNumber() : 0.0;
		}

		TOptional<double> OptionalNumber(const TCHAR* Field)
		{
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::Number, true);
			return Value.IsValid() ? TOptional<double>(Value->AsNumber()) : TOptional<double>();
		}

		int32 ToInt(const TCHAR* Field, double Value)
		{
			if (Value < static_cast<double>(MIN_int32) || Value > static_cast<double>(MAX_int32))
			{
				Fail(Field);
				return 0;
			}
			return static_cast<int32>(Value);
		}

		int32 Int(const TCHAR* Field)
		{
			return ToInt(Field, Number(Field));
		}

		TOptional<int32> OptionalInt(const TCHAR* Field)
		{
			const TOptional<double> Value = OptionalNumber(Field);
			return Value.IsSet() ? TOptional<int32>(ToInt(Field, Value.GetValue())) : TOptional<int32>();
		}

		// A BigInt may arrive as a number or as a string of digits.
		int64 BigInt(const TCHAR* Field)
		{
			const TSharedPtr<FJsonValue> Value = Json.TryGetField(Field);
			int64 Parsed = 0;
			const bool bNumber = Value.IsValid() && Value->Type == EJson::Number && Value->TryGetNumber(Parsed);
			const bool bDigits = Value.IsValid() && Value->Type == EJson::String && IsWholeNumber(Value->AsString())
				&& LexTryParseString(Parsed, *Value->AsString());
			if (!bNumber && !bDigits)
			{
				Fail(Field);
				return 0;
			}
			return Parsed;
		}

		static bool IsWholeNumber(const FString& Text)
		{
			return Text.IsNumeric() && !Text.Contains(TEXT("."));
		}

		FDateTime Time(const TCHAR* Field)
		{
			FDateTime Parsed;
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::String, false);
			if (Value.IsValid() && !FDateTime::ParseIso8601(*Value->AsString(), Parsed))
			{
				Fail(Field);
			}
			return Parsed;
		}

		TArray<FString> Strings(const TCHAR* Field)
		{
			TArray<FString> Out;
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::Array, false);
			if (!Value.IsValid())
			{
				return Out;
			}
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (!Item.IsValid() || Item->Type != EJson::String)
				{
					Fail(Field);
					return Out;
				}
				Out.Add(Item->AsString());
			}
			return Out;
		}

		template <typename T, typename FReadFields>
		void Object(const TCHAR* Field, T& Out, const FReadFields& ReadFields)
		{
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::Object, false);
			if (!Value.IsValid() || !Value->AsObject().IsValid())
			{
				Fail(Field);
				return;
			}
			FAnswerReader Inner(*Value->AsObject());
			ReadFields(Inner, Out);
			if (!Inner.BadField.IsEmpty())
			{
				Fail(FString::Printf(TEXT("%s.%s"), Field, *Inner.BadField));
			}
		}

		template <typename T, typename FReadItem>
		void List(const TCHAR* Field, TArray<T>& Out, const FReadItem& ReadItem)
		{
			const TSharedPtr<FJsonValue> Value = Get(Field, EJson::Array, false);
			if (!Value.IsValid())
			{
				return;
			}
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (!Item.IsValid() || Item->Type != EJson::Object || !Item->AsObject().IsValid())
				{
					Fail(Field);
					return;
				}
				FAnswerReader Inner(*Item->AsObject());
				ReadItem(Inner, Out.AddDefaulted_GetRef());
				if (!Inner.BadField.IsEmpty())
				{
					Fail(FString::Printf(TEXT("%s.%s"), Field, *Inner.BadField));
					return;
				}
			}
		}
	};

	void ReadBuildFile(FAnswerReader& Reader, FBuildFile& Out)
	{
		Out.Path = Reader.String(TEXT("path"));
		Out.Content = Reader.String(TEXT("content"));
	}

	void ReadArtifact(FAnswerReader& Reader, FBuildArtifact& Out)
	{
		Out.Crate = Reader.String(TEXT("crate"));
		Out.Digest = Reader.String(TEXT("digest"));
		Out.SizeBytes = Reader.BigInt(TEXT("sizeBytes"));
	}

	void ReadBuild(FAnswerReader& Reader, FBuild& Out)
	{
		Out.BuildId = Reader.String(TEXT("buildId"));
		Out.Status = Reader.String(TEXT("status"));
		Out.Log = Reader.String(TEXT("log"), true);
		Reader.List(TEXT("artifacts"), Out.Artifacts, &ReadArtifact);
	}

	void ReadDeployed(FAnswerReader& Reader, int32& Out)
	{
		Out = Reader.Int(TEXT("version"));
	}

	void ReadVersion(FAnswerReader& Reader, FVersion& Out)
	{
		Out.Version = Reader.Int(TEXT("version"));
		Out.CreatedBy = Reader.String(TEXT("createdBy"), true);
		Out.CreatedAt = Reader.Time(TEXT("createdAt"));
		Out.Types = Reader.Int(TEXT("types"));
		Out.bActive = Reader.Flag(TEXT("active"));
		Out.ManifestJson = Reader.String(TEXT("manifestJson"), true);
	}

	void ReadAppStatus(FAnswerReader& Reader, FAppStatus& Out)
	{
		Out.ActiveVersion = Reader.OptionalInt(TEXT("activeVersion"));
		Out.bDisabled = Reader.Flag(TEXT("disabled"));
		Out.DisabledTypes = Reader.Strings(TEXT("disabledTypes"));
		Out.bBudgetPaused = Reader.Flag(TEXT("budgetPaused"));
	}

	void ReadInstance(FAnswerReader& Reader, FInstance& Out)
	{
		Out.InstanceId = Reader.String(TEXT("instanceId"));
		Out.NodeType = Reader.String(TEXT("nodeType"));
		Out.Key = Reader.String(TEXT("key"));
		Out.Kind = Reader.String(TEXT("kind"));
		Out.Phase = Reader.String(TEXT("phase"));
		Out.Host = Reader.String(TEXT("host"), true);
		Out.HeldBack = Reader.String(TEXT("heldBack"), true);
		Out.Epoch = Reader.BigInt(TEXT("epoch"));
		Out.SinceMs = Reader.Number(TEXT("sinceMs"));
	}

	void ReadLogLine(FAnswerReader& Reader, FLogLine& Out)
	{
		Out.Id = Reader.String(TEXT("id"));
		Out.NodeType = Reader.String(TEXT("nodeType"));
		Out.Key = Reader.String(TEXT("key"));
		Out.Host = Reader.String(TEXT("host"));
		Out.Flow = Reader.String(TEXT("flow"), true);
		Out.Text = Reader.String(TEXT("text"));
		Out.Level = Reader.Int(TEXT("level"));
		Out.At = Reader.Time(TEXT("at"));
	}

	void ReadEndpointStat(FAnswerReader& Reader, FEndpointStat& Out)
	{
		Out.NodeType = Reader.String(TEXT("nodeType"));
		Out.Method = Reader.String(TEXT("method"));
		Out.Calls = Reader.Number(TEXT("calls"));
		Out.AppErrors = Reader.Number(TEXT("appErrors"));
		Out.Busy = Reader.Number(TEXT("busy"));
		Out.Denied = Reader.Number(TEXT("denied"));
		Out.DeadlineExceeded = Reader.Number(TEXT("deadlineExceeded"));
		Out.OtherErrors = Reader.Number(TEXT("otherErrors"));
		Out.LatencyMsAvg = Reader.OptionalNumber(TEXT("latencyMsAvg"));
		Out.LatencyMsMax = Reader.OptionalNumber(TEXT("latencyMsMax"));
	}

	void ReadStarter(FAnswerReader& Reader, FStarter& Out)
	{
		Out.Crate.Name = Reader.String(TEXT("crate"));
		Out.NodeType = Reader.String(TEXT("nodeType"));
		Out.Description = Reader.String(TEXT("description"));
		Reader.List(TEXT("files"), Out.Crate.Files, &ReadBuildFile);
	}

	void ReadStarterPack(FAnswerReader& Reader, FStarterPack& Out)
	{
		Out.ManifestJson = Reader.String(TEXT("manifestJson"));
		Reader.List(TEXT("starters"), Out.Starters, &ReadStarter);
	}

	template <typename T, typename FRead>
	TResult<T> ReadAnswer(const FCrowdyCppJsonResult& Result, const FString& Operation, const FRead& Read)
	{
		TResult<T> Out;
		if (!Result.bTransportOk || !Result.Data.IsValid())
		{
			Out.Error.Message = Result.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("%s failed"), *Operation) : Result.ErrorMessage;
			Out.Error.Code = Result.ErrorCode;
			Out.Error.bNoAnswer = Result.ErrorCode.IsEmpty();
			return Out;
		}
		FAnswerReader Reader(*Result.Data);
		Read(Reader, Out.Value);
		if (!Reader.Problem.IsEmpty())
		{
			Out.Error.Message = Reader.Problem;
			return Out;
		}
		if (!Reader.BadField.IsEmpty())
		{
			Out.Error.Message = FString::Printf(TEXT("%s answered without a valid %s"), *Operation, *Reader.BadField);
			return Out;
		}
		Out.bOk = true;
		return Out;
	}

	bool IsHostCharacter(TCHAR Character)
	{
		return (Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'))
			|| Character == TEXT('.') || Character == TEXT('-');
	}

	// The https origin a wrong-datacenter refusal names, only when its host is under crowdedkingdoms.com; empty otherwise.
	FString RedirectOrigin(const FString& Message)
	{
		const FString Lead = TEXT("Reconnect to https://");
		const int32 At = Message.Find(Lead, ESearchCase::CaseSensitive);
		if (At == INDEX_NONE)
		{
			return FString();
		}
		const int32 HostStart = At + Lead.Len();
		int32 HostEnd = HostStart;
		while (HostEnd < Message.Len() && IsHostCharacter(Message[HostEnd]))
		{
			++HostEnd;
		}
		FString Host = Message.Mid(HostStart, HostEnd - HostStart);
		while (Host.EndsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			Host.LeftChopInline(1);
		}
		const FString Estate = TEXT(".crowdedkingdoms.com");
		if (Host.Len() <= Estate.Len() || !Host.EndsWith(Estate, ESearchCase::CaseSensitive))
		{
			return FString();
		}
		return HttpsScheme + Host;
	}

	struct FRequest
	{
		TWeakPtr<FCrowdyCppClient> Client;
		FString Operation;
		TSharedPtr<FJsonObject> Variables;
		TArray<float> BusyWaits;
		TFunction<void(const FCrowdyCppJsonResult&)> OnResult;
		int32 Attempt = 0;
		bool bMoved = false;
	};

	void After(float Seconds, TFunction<void()> Then)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Then = MoveTemp(Then)](float) -> bool
		{
			Then();
			return false;
		}), Seconds);
	}

	void Send(const TSharedRef<FRequest>& Request);

	// Moves the client to the datacenter a refusal named and sends once more; answers the refusal when it cannot move there.
	void FollowRedirect(const TSharedRef<FRequest>& Request, const FString& Origin, const FCrowdyCppJsonResult& Refusal)
	{
		const TSharedPtr<FCrowdyCppClient> Client = Request->Client.Pin();
		const FString ApiUrl = Origin + TEXT("/graphql");
		const FString WsUrl = TEXT("wss://") + Origin.RightChop(HttpsScheme.Len()) + TEXT("/graphql");
		if (!Client.IsValid() || (!Client->MoveToDatacenter(ApiUrl, WsUrl) && Client->GetApiEndpoint() != ApiUrl))
		{
			Request->OnResult(Refusal);
			return;
		}
		Request->bMoved = true;
		Send(Request);
	}

	// Sends the operation under the developer's session bearer; a refusal as busy is sent again after each wait in BusyWaits.
	void Send(const TSharedRef<FRequest>& Request)
	{
		const TSharedPtr<FCrowdyCppClient> Client = Request->Client.Pin();
		if (!Client.IsValid())
		{
			FCrowdyCppJsonResult Closed;
			Closed.ErrorMessage = TEXT("The connection to Crowdy was closed");
			Request->OnResult(Closed);
			return;
		}
		Client->RunOp(ECrowdyCppApiDomain::Exec, Request->Operation, Request->Variables, [Request](FCrowdyCppJsonResult Result)
			{
				const bool bRedirect = !Request->bMoved && Result.ErrorCode == WrongDatacenterCode;
				const FString Origin = bRedirect ? RedirectOrigin(Result.ErrorMessage) : FString();
				if (!Origin.IsEmpty())
				{
					After(0.f, [Request, Origin, Result] { FollowRedirect(Request, Origin, Result); });
					return;
				}
				if (Result.ErrorCode != PlatformBusyCode || !Request->BusyWaits.IsValidIndex(Request->Attempt))
				{
					Request->OnResult(Result);
					return;
				}
				After(Request->BusyWaits[Request->Attempt++], [Request] { Send(Request); });
			},
			ECrowdyCppTokenPlane::Management);
	}

	template <typename T, typename FRead>
	void Run(const TSharedPtr<FCrowdyCppClient>& Client, const TCHAR* Operation, const TSharedRef<FJsonObject>& Variables,
		const TArray<float>& BusyWaits, TFunction<void(const TResult<T>&)> OnDone, FRead Read)
	{
		const TSharedRef<FRequest> Request = MakeShared<FRequest>();
		Request->Client = Client;
		Request->Operation = Operation;
		Request->Variables = Variables;
		Request->BusyWaits = BusyWaits;
		Request->OnResult = [Name = Request->Operation, OnDone = MoveTemp(OnDone), Read](const FCrowdyCppJsonResult& Result)
		{
			if (OnDone)
			{
				OnDone(ReadAnswer<T>(Result, Name, Read));
			}
		};
		Send(Request);
	}

	// An operation whose answer is one object under RootField.
	template <typename T, typename FReadFields>
	void RunObject(const TSharedPtr<FCrowdyCppClient>& Client, const TCHAR* Operation, const TCHAR* RootField,
		const TSharedRef<FJsonObject>& Variables, const TArray<float>& BusyWaits, TFunction<void(const TResult<T>&)> OnDone,
		FReadFields ReadFields)
	{
		Run<T>(Client, Operation, Variables, BusyWaits, MoveTemp(OnDone),
			[RootField, ReadFields](FAnswerReader& Data, T& Out) { Data.Object(RootField, Out, ReadFields); });
	}

	// An operation whose answer is a list of objects under RootField.
	template <typename T, typename FReadItem>
	void RunList(const TSharedPtr<FCrowdyCppClient>& Client, const TCHAR* Operation, const TCHAR* RootField,
		const TSharedRef<FJsonObject>& Variables, TFunction<void(const TResult<TArray<T>>&)> OnDone, FReadItem ReadItem)
	{
		Run<TArray<T>>(Client, Operation, Variables, TArray<float>(), MoveTemp(OnDone),
			[RootField, ReadItem](FAnswerReader& Data, TArray<T>& Out) { Data.List(RootField, Out, ReadItem); });
	}

	TSharedRef<FJsonObject> AppVariables(int64 AppId)
	{
		const TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("appId"), LexToString(AppId));
		return Variables;
	}

	void SetStringIfSet(FJsonObject& Variables, const TCHAR* Field, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Variables.SetStringField(Field, Value);
		}
	}

	TSharedRef<FJsonValue> CrateValue(const FBuildCrate& Crate)
	{
		TArray<TSharedPtr<FJsonValue>> Files;
		Files.Reserve(Crate.Files.Num());
		for (const FBuildFile& File : Crate.Files)
		{
			const TSharedRef<FJsonObject> FileObject = MakeShared<FJsonObject>();
			FileObject->SetStringField(TEXT("path"), File.Path);
			FileObject->SetStringField(TEXT("content"), File.Content);
			Files.Add(MakeShared<FJsonValueObject>(FileObject));
		}
		const TSharedRef<FJsonObject> CrateObject = MakeShared<FJsonObject>();
		CrateObject->SetStringField(TEXT("name"), Crate.Name);
		CrateObject->SetArrayField(TEXT("files"), Files);
		return MakeShared<FJsonValueObject>(CrateObject);
	}

	TSharedRef<FJsonObject> InputVariables(const TSharedRef<FJsonObject>& Input)
	{
		const TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), Input);
		return Variables;
	}

	struct FBuildWait
	{
		FString BuildId;
		TFunction<void(const TResult<FBuild>&)> OnDone;
		TFunction<void(const FBuild&)> OnProgress;
		double Deadline = 0.0;
		float TimeoutSeconds = 0.f;
		bool bInFlight = false;
		bool bFinished = false;
	};

	void FinishWait(FBuildWait& Wait, const TResult<FBuild>& Result)
	{
		Wait.bFinished = true;
		if (Wait.OnDone)
		{
			Wait.OnDone(Result);
		}
	}

	TResult<FBuild> WaitError(const FString& Message)
	{
		TResult<FBuild> Result;
		Result.Error.Message = Message;
		return Result;
	}

	// One status read of a build being waited on; false once the wait is over.
	bool TickBuildWait(const TWeakPtr<FClient>& WeakClient, const TSharedRef<FBuildWait>& Wait)
	{
		if (Wait->bFinished)
		{
			return false;
		}
		if (Wait->bInFlight)
		{
			return true;
		}
		const TSharedPtr<FClient> Client = WeakClient.Pin();
		if (!Client.IsValid())
		{
			FinishWait(*Wait, WaitError(TEXT("The connection to Crowdy was closed")));
			return false;
		}
		if (FPlatformTime::Seconds() >= Wait->Deadline)
		{
			const int32 Minutes = FMath::Max(1, FMath::CeilToInt(Wait->TimeoutSeconds / 60.f));
			FinishWait(*Wait, WaitError(FString::Printf(TEXT("The build did not finish in %d %s"), Minutes, Minutes == 1 ? TEXT("minute") : TEXT("minutes"))));
			return false;
		}
		Wait->bInFlight = true;
		Client->BuildStatus(Wait->BuildId, [Wait](const TResult<FBuild>& Result)
		{
			Wait->bInFlight = false;
			if (!Result.bOk && (Result.Error.Code == PlatformBusyCode || Result.Error.bNoAnswer))
			{
				return;
			}
			if (Result.bOk && Wait->OnProgress)
			{
				Wait->OnProgress(Result.Value);
			}
			if (!Result.bOk || Result.Value.IsFinished())
			{
				FinishWait(*Wait, Result);
			}
		});
		return true;
	}
}

namespace CrowdyExecDeveloper
{
	TSharedPtr<FClient> FClient::Create(FString& OutError)
	{
		const FString Token = CrowdyStudioAuth::GetSignedInToken();
		if (Token.IsEmpty())
		{
			OutError = TEXT("Sign in to Crowdy Studio first");
			return nullptr;
		}
		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		if (!Settings || Settings->AppID <= 0)
		{
			OutError = TEXT("Set the app in Crowdy Studio first");
			return nullptr;
		}
		FString Url = Settings->GetDiscoveryUrl();
		Url.RemoveFromEnd(TEXT("/"));
		if (Url.IsEmpty())
		{
			OutError = TEXT("Set the backend address in Project Settings > Crowdy SDK first");
			return nullptr;
		}
		Url += TEXT("/graphql");

		FCrowdyCppClientConfig Config;
		Config.ApiUrl = Url;
		Config.DiscoveryUrl = Url;
		const TSharedPtr<FCrowdyCppAdminClientHost> NewHost = FCrowdyCppAdminClientHost::Create(Config, FString());
		if (!NewHost.IsValid())
		{
			OutError = TEXT("Could not open a connection to Crowdy");
			return nullptr;
		}
		NewHost->GetClient()->SetManagementToken(Token);

		const TSharedRef<FClient> NewClient = MakeShareable(new FClient());
		NewClient->Host = NewHost;
		NewClient->Client = NewHost->GetClient();
		NewClient->AppId = Settings->AppID;
		return NewClient;
	}

	TSharedRef<FClient> FClient::CreateForClient(TSharedRef<FCrowdyCppClient> InClient, int64 InAppId)
	{
		const TSharedRef<FClient> NewClient = MakeShareable(new FClient());
		NewClient->Client = InClient;
		NewClient->AppId = InAppId;
		return NewClient;
	}

	void FClient::Starters(TFunction<void(const TResult<FStarterPack>&)> OnDone)
	{
		CrowdyExecDevApi::RunObject<FStarterPack>(Client, TEXT("ExecStarters"), TEXT("execStarters"),
			CrowdyExecDevApi::AppVariables(AppId), TArray<float>(), MoveTemp(OnDone), &CrowdyExecDevApi::ReadStarterPack);
	}

	void FClient::Build(const TArray<FBuildCrate>& Crates, TFunction<void(const TResult<FBuild>&)> OnDone)
	{
		TArray<TSharedPtr<FJsonValue>> CrateValues;
		CrateValues.Reserve(Crates.Num());
		for (const FBuildCrate& Crate : Crates)
		{
			CrateValues.Add(CrowdyExecDevApi::CrateValue(Crate));
		}
		const TSharedRef<FJsonObject> Input = CrowdyExecDevApi::AppVariables(AppId);
		Input->SetArrayField(TEXT("crates"), CrateValues);

		CrowdyExecDevApi::RunObject<FBuild>(Client, TEXT("ExecBuild"), TEXT("execBuild"),
			CrowdyExecDevApi::InputVariables(Input), BusyWaitSeconds, MoveTemp(OnDone), &CrowdyExecDevApi::ReadBuild);
	}

	void FClient::BuildStatus(const FString& BuildId, TFunction<void(const TResult<FBuild>&)> OnDone)
	{
		const TSharedRef<FJsonObject> Variables = CrowdyExecDevApi::AppVariables(AppId);
		Variables->SetStringField(TEXT("buildId"), BuildId);

		CrowdyExecDevApi::Run<FBuild>(Client, TEXT("ExecBuildStatus"), Variables, TArray<float>(), MoveTemp(OnDone),
			[BuildId](CrowdyExecDevApi::FAnswerReader& Data, FBuild& Out)
			{
				if (Data.IsNull(TEXT("execBuildStatus")))
				{
					Data.Problem = FString::Printf(TEXT("The app has no build %s"), *BuildId);
					return;
				}
				Data.Object(TEXT("execBuildStatus"), Out, &CrowdyExecDevApi::ReadBuild);
			});
	}

	void FClient::WaitForBuild(const FString& BuildId, TFunction<void(const TResult<FBuild>&)> OnDone,
		TFunction<void(const FBuild&)> OnProgress, float PollSeconds, float TimeoutSeconds)
	{
		const TSharedRef<CrowdyExecDevApi::FBuildWait> Wait = MakeShared<CrowdyExecDevApi::FBuildWait>();
		Wait->BuildId = BuildId;
		Wait->OnDone = MoveTemp(OnDone);
		Wait->OnProgress = MoveTemp(OnProgress);
		Wait->Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		Wait->TimeoutSeconds = TimeoutSeconds;

		const TWeakPtr<FClient> WeakThis = AsShared();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, Wait](float) -> bool
		{
			return CrowdyExecDevApi::TickBuildWait(WeakThis, Wait);
		}), FMath::Max(0.f, PollSeconds));
	}

	void FClient::Deploy(const FString& ManifestJson, const FString& BuildId, TFunction<void(const TResult<int32>&)> OnDone)
	{
		const TSharedRef<FJsonObject> Input = CrowdyExecDevApi::AppVariables(AppId);
		Input->SetStringField(TEXT("manifestJson"), ManifestJson);
		Input->SetStringField(TEXT("buildId"), BuildId);
		Input->SetArrayField(TEXT("artifacts"), TArray<TSharedPtr<FJsonValue>>());

		CrowdyExecDevApi::RunObject<int32>(Client, TEXT("ExecDeploy"), TEXT("execDeploy"),
			CrowdyExecDevApi::InputVariables(Input), BusyWaitSeconds, MoveTemp(OnDone), &CrowdyExecDevApi::ReadDeployed);
	}

	void FClient::Versions(TFunction<void(const TResult<TArray<FVersion>>&)> OnDone)
	{
		CrowdyExecDevApi::RunList<FVersion>(Client, TEXT("ExecVersions"), TEXT("execVersions"),
			CrowdyExecDevApi::AppVariables(AppId), MoveTemp(OnDone), &CrowdyExecDevApi::ReadVersion);
	}

	void FClient::ActivateVersion(int32 Version, TFunction<void(const TResult<FAppStatus>&)> OnDone)
	{
		const TSharedRef<FJsonObject> Variables = CrowdyExecDevApi::AppVariables(AppId);
		Variables->SetNumberField(TEXT("version"), Version);

		CrowdyExecDevApi::RunObject<FAppStatus>(Client, TEXT("ExecActivateVersion"), TEXT("execActivateVersion"),
			Variables, BusyWaitSeconds, MoveTemp(OnDone), &CrowdyExecDevApi::ReadAppStatus);
	}

	void FClient::SetEnabled(bool bEnabled, const FString& NodeType, TFunction<void(const TResult<FAppStatus>&)> OnDone)
	{
		const TSharedRef<FJsonObject> Variables = CrowdyExecDevApi::AppVariables(AppId);
		Variables->SetBoolField(TEXT("enabled"), bEnabled);
		CrowdyExecDevApi::SetStringIfSet(*Variables, TEXT("nodeType"), NodeType);

		CrowdyExecDevApi::RunObject<FAppStatus>(Client, TEXT("ExecSetEnabled"), TEXT("execSetEnabled"),
			Variables, BusyWaitSeconds, MoveTemp(OnDone), &CrowdyExecDevApi::ReadAppStatus);
	}

	void FClient::Status(TFunction<void(const TResult<FAppStatus>&)> OnDone)
	{
		CrowdyExecDevApi::RunObject<FAppStatus>(Client, TEXT("ExecAppStatus"), TEXT("execAppStatus"),
			CrowdyExecDevApi::AppVariables(AppId), TArray<float>(), MoveTemp(OnDone), &CrowdyExecDevApi::ReadAppStatus);
	}

	void FClient::Instances(TFunction<void(const TResult<TArray<FInstance>>&)> OnDone)
	{
		CrowdyExecDevApi::RunList<FInstance>(Client, TEXT("ExecInstances"), TEXT("execInstances"),
			CrowdyExecDevApi::AppVariables(AppId), MoveTemp(OnDone), &CrowdyExecDevApi::ReadInstance);
	}

	void FClient::Logs(const FLogQuery& Query, TFunction<void(const TResult<TArray<FLogLine>>&)> OnDone)
	{
		const TSharedRef<FJsonObject> Variables = CrowdyExecDevApi::AppVariables(AppId);
		CrowdyExecDevApi::SetStringIfSet(*Variables, TEXT("nodeType"), Query.NodeType);
		CrowdyExecDevApi::SetStringIfSet(*Variables, TEXT("key"), Query.Key);
		CrowdyExecDevApi::SetStringIfSet(*Variables, TEXT("flow"), Query.Flow);
		CrowdyExecDevApi::SetStringIfSet(*Variables, TEXT("before"), Query.Before);
		if (Query.MaxLevel >= 0)
		{
			Variables->SetNumberField(TEXT("maxLevel"), Query.MaxLevel);
		}
		if (Query.Limit >= 0)
		{
			Variables->SetNumberField(TEXT("limit"), Query.Limit);
		}

		CrowdyExecDevApi::RunList<FLogLine>(Client, TEXT("ExecLogs"), TEXT("execLogs"), Variables, MoveTemp(OnDone),
			&CrowdyExecDevApi::ReadLogLine);
	}

	void FClient::EndpointStats(const FString& NodeType, int32 SinceMinutes,
		TFunction<void(const TResult<TArray<FEndpointStat>>&)> OnDone)
	{
		const TSharedRef<FJsonObject> Variables = CrowdyExecDevApi::AppVariables(AppId);
		CrowdyExecDevApi::SetStringIfSet(*Variables, TEXT("nodeType"), NodeType);
		if (SinceMinutes >= 1)
		{
			Variables->SetNumberField(TEXT("sinceMinutes"), SinceMinutes);
		}

		CrowdyExecDevApi::RunList<FEndpointStat>(Client, TEXT("ExecEndpointStats"), TEXT("execEndpointStats"), Variables,
			MoveTemp(OnDone), &CrowdyExecDevApi::ReadEndpointStat);
	}
}
