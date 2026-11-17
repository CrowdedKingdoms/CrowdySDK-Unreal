#include "Subsystem/CrowdyStudioProjects.h"
#include "CrowdyCppClient.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServicesLog.h"
#include "CrowdyStudioClientStarter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameInstance.h"
#include "Misc/DateTime.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyMods.h"

using namespace CrowdyServiceApi;

namespace
{
	const TCHAR* const StudioProjLogName = TEXT("CrowdyStudioProjects");
	const TCHAR* const StudioProjNotSignedIn = TEXT("The player has no app token yet.");
	const TCHAR* const StudioProjNoClient = TEXT("The Crowdy API client is unavailable, so the request was not sent.");
	const TCHAR* const StudioProjAppChanged = TEXT("The app changed while the call was in flight.");
	const TCHAR* const StudioProjNoAnswer = TEXT("The answer holds nothing.");
	const TCHAR* const StudioProjNoMods = TEXT("The Mods service is unavailable, so the server starter could not be read and nothing was sent.");

	constexpr int32 StudioProjPageSize = 50;
	constexpr int32 StudioProjMaxIdChars = 64;
	constexpr int32 StudioProjMaxTextChars = 256;
	constexpr int32 StudioProjMaxFilesPerTarget = 8;
	constexpr int32 StudioProjMaxFilesOnCreate = 16;
	constexpr int32 StudioProjMaxFileBytes = 64 * 1024;
	constexpr int32 StudioProjMaxTargetBytes = 256 * 1024;
	constexpr int32 StudioProjMaxModuleChars = 48;
	constexpr int32 StudioProjMaxSlugChars = 40;

	const TCHAR* StudioProjResultName(ECrowdyStudioResult Result)
	{
		switch (Result)
		{
		case ECrowdyStudioResult::Success:     return TEXT("Success");
		case ECrowdyStudioResult::NotSignedIn: return TEXT("NotSignedIn");
		case ECrowdyStudioResult::Denied:      return TEXT("Denied");
		case ECrowdyStudioResult::NotFound:    return TEXT("NotFound");
		case ECrowdyStudioResult::Refused:     return TEXT("Refused");
		case ECrowdyStudioResult::Failed:      return TEXT("Failed");
		case ECrowdyStudioResult::BadReply:    return TEXT("BadReply");
		case ECrowdyStudioResult::NotBuilt:    return TEXT("NotBuilt");
		case ECrowdyStudioResult::NoModel:     return TEXT("NoModel");
		}
		return TEXT("Unknown");
	}

	// Only the result and the platform's code are logged: ids, source and the platform's text stay out.
	void StudioProjWarn(const TCHAR* Call, ECrowdyStudioResult Result, const FString& Code)
	{
		UE_CLOG(Result != ECrowdyStudioResult::Success, LogCrowdyServices, Warning, TEXT("[%s] %s ended %s%s%s"), StudioProjLogName, Call,
			StudioProjResultName(Result), Code.IsEmpty() ? TEXT("") : TEXT(": "), *Code);
	}

	template <typename AnswerType>
	void StudioProjFinish(const TCHAR* Call, const TFunction<void(const AnswerType&)>& OnDone, AnswerType Answer, ECrowdyStudioResult Result,
		const FString& Message, const FString& Code = FString())
	{
		Answer.Result = Result;
		Answer.Message = Message;
		StudioProjWarn(Call, Result, Code);
		OnDone(Answer);
	}

	// A failed call answers an empty result, so nothing from a refusal reads as data.
	template <typename AnswerType>
	TFunction<void(ECrowdyStudioResult, const FString&, const FString&)> StudioProjFailWith(const TCHAR* Call, const TFunction<void(const AnswerType&)>& OnDone)
	{
		return [Call, OnDone](ECrowdyStudioResult Result, const FString& Message, const FString& Code)
		{
			StudioProjFinish(Call, OnDone, AnswerType(), Result, Message, Code);
		};
	}

	ECrowdyStudioResult StudioProjFailureResult(const FCrowdyCppJsonResult& Result)
	{
		if (Result.ErrorCode.IsEmpty() || Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage())
		{
			return ECrowdyStudioResult::Failed;
		}
		if (NamesCode(Result, TEXT("NOT_FOUND")) || Result.ErrorCode.EndsWith(TEXT("_NOT_FOUND"), ESearchCase::CaseSensitive))
		{
			return ECrowdyStudioResult::NotFound;
		}
		static const TCHAR* const DeniedCodes[] = {TEXT("FORBIDDEN"), TEXT("UNAUTHORIZED"), TEXT("UNAUTHENTICATED"), TEXT("NOT_ALLOWED"), TEXT("SCOPE_MISSING")};
		for (const TCHAR* Code : DeniedCodes)
		{
			if (NamesCode(Result, Code))
			{
				return ECrowdyStudioResult::Denied;
			}
		}
		return ECrowdyStudioResult::Refused;
	}

	ECrowdyStudioResult StudioProjFromModResult(ECrowdyModResult Result)
	{
		switch (Result)
		{
		case ECrowdyModResult::Success:     return ECrowdyStudioResult::Success;
		case ECrowdyModResult::NotSignedIn: return ECrowdyStudioResult::NotSignedIn;
		case ECrowdyModResult::Denied:      return ECrowdyStudioResult::Denied;
		case ECrowdyModResult::NotFound:    return ECrowdyStudioResult::NotFound;
		case ECrowdyModResult::BadReply:    return ECrowdyStudioResult::BadReply;
		case ECrowdyModResult::NotBuilt:    return ECrowdyStudioResult::NotBuilt;
		case ECrowdyModResult::Refused:
		case ECrowdyModResult::RevisionChanged: return ECrowdyStudioResult::Refused;
		default:                            return ECrowdyStudioResult::Failed;
		}
	}

	TSharedPtr<FJsonObject> StudioProjAnswerObject(const FCrowdyCppJsonResult& Result, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetObjectField(Field, Object) ? *Object : nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* StudioProjAnswerArray(const FCrowdyCppJsonResult& Result, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		return Result.Data.IsValid() && Result.Data->TryGetArrayField(Field, Array) ? Array : nullptr;
	}

	TSharedPtr<FJsonObject> StudioProjElement(const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Value.IsValid() && Value->TryGetObject(Object) ? *Object : nullptr;
	}

	// A string, at most MaxChars long.
	bool StudioProjReadText(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 MaxChars, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return false;
		}
		Out = Value->AsString();
		return Out.Len() <= MaxChars;
	}

	// Absent or null reads as empty.
	bool StudioProjReadOptText(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 MaxChars, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid() || Value->IsNull())
		{
			Out.Reset();
			return true;
		}
		return StudioProjReadText(Object, Field, MaxChars, Out);
	}

	// Absent or null reads as 0.
	bool StudioProjReadOptId(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		if (!Value.IsValid() || Value->IsNull())
		{
			Out = 0;
			return true;
		}
		return ReadWholeInt64(Object, Field, Out);
	}

	// A DateTime arrives as ISO-8601 text.
	bool StudioProjReadTime(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& OutMs)
	{
		FString Text;
		FDateTime Time;
		if (!Object->TryGetStringField(Field, Text) || !FDateTime::ParseIso8601(*Text, Time))
		{
			return false;
		}
		OutMs = (Time - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
		return true;
	}

	bool StudioProjReadKind(const FString& Text, ECrowdyStudioProjectKind& Out)
	{
		if (Text.Equals(TEXT("SERVER_ONLY"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioProjectKind::Server;
			return true;
		}
		if (Text.Equals(TEXT("CLIENT_ONLY"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioProjectKind::Client;
			return true;
		}
		if (Text.Equals(TEXT("PAIRED"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioProjectKind::Paired;
			return true;
		}
		if (Text.Equals(TEXT("INDEPENDENT"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioProjectKind::Independent;
			return true;
		}
		return false;
	}

	const TCHAR* StudioProjKindName(ECrowdyStudioProjectKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyStudioProjectKind::Server: return TEXT("SERVER_ONLY");
		case ECrowdyStudioProjectKind::Client: return TEXT("CLIENT_ONLY");
		case ECrowdyStudioProjectKind::Independent: return TEXT("INDEPENDENT");
		default: return TEXT("PAIRED");
		}
	}

	bool StudioProjHasServer(ECrowdyStudioProjectKind Kind)
	{
		return Kind != ECrowdyStudioProjectKind::Client;
	}

	bool StudioProjHasClient(ECrowdyStudioProjectKind Kind)
	{
		return Kind != ECrowdyStudioProjectKind::Server;
	}

	bool StudioProjReadSource(const FString& Text, ECrowdyStudioProjectSource& Out)
	{
		if (Text.Equals(TEXT("STUDIO"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioProjectSource::Studio;
			return true;
		}
		if (Text.Equals(TEXT("GITHUB"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioProjectSource::GitHub;
			return true;
		}
		return false;
	}

	bool StudioProjReadTarget(const FString& Text, ECrowdyStudioTarget& Out)
	{
		if (Text.Equals(TEXT("SERVER"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioTarget::Server;
			return true;
		}
		if (Text.Equals(TEXT("CLIENT"), ESearchCase::CaseSensitive))
		{
			Out = ECrowdyStudioTarget::Client;
			return true;
		}
		return false;
	}

	bool StudioProjReadProject(const TSharedPtr<FJsonObject>& Object, FCrowdyStudioProject& Out)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		FString Pairing;
		FString SourceText;
		int64 Revision = 0;
		const bool bIdentity = StudioProjReadText(Object, TEXT("projectId"), StudioProjMaxIdChars, Out.ProjectId) && !Out.ProjectId.IsEmpty()
			&& StudioProjReadText(Object, TEXT("name"), StudioProjMaxTextChars, Out.Name) && ReadWholeInt64(Object, TEXT("revision"), Revision)
			&& StudioProjReadTime(Object, TEXT("updatedAt"), Out.UpdatedAtMs) && StudioProjReadOptId(Object, TEXT("gridId"), Out.GridId);
		const bool bShape = bIdentity && StudioProjReadText(Object, TEXT("pairingPreference"), StudioProjMaxIdChars, Pairing) && StudioProjReadKind(Pairing, Out.Kind)
			&& StudioProjReadText(Object, TEXT("source"), StudioProjMaxIdChars, SourceText) && StudioProjReadSource(SourceText, Out.Source)
			&& StudioProjReadOptText(Object, TEXT("serverModuleName"), StudioProjMaxTextChars, Out.ServerModule)
			&& StudioProjReadOptText(Object, TEXT("clientModuleName"), StudioProjMaxTextChars, Out.ClientModule);
		FString Owner;
		FString Repo;
		FString Branch;
		if (!bShape || !StudioProjReadOptText(Object, TEXT("githubOwner"), StudioProjMaxTextChars, Owner) || !StudioProjReadOptText(Object, TEXT("githubRepo"), StudioProjMaxTextChars, Repo)
			|| !StudioProjReadOptText(Object, TEXT("githubBranch"), StudioProjMaxTextChars, Branch) || !StudioProjReadOptText(Object, TEXT("githubSha"), StudioProjMaxTextChars, Out.GitHubSha))
		{
			return false;
		}
		Out.Revision = BigInt(Revision);
		if (Out.Source != ECrowdyStudioProjectSource::GitHub)
		{
			Out.GitHubSha.Reset();
			return true;
		}
		Out.Revision = FString::Printf(TEXT("%s@%s"), *Out.Revision, *Out.GitHubSha);
		Out.GitHubRef = Owner.IsEmpty() || Repo.IsEmpty() ? FString() : FString::Printf(TEXT("%s/%s%s%s"), *Owner, *Repo, Branch.IsEmpty() ? TEXT("") : TEXT("@"), *Branch);
		return true;
	}

	// Cargo.toml, README.md, or src/ then letters, digits, _, - and / in non-empty segments, ending in .rs.
	bool StudioProjPathReadable(const FString& Path)
	{
		if (Path.Equals(TEXT("Cargo.toml"), ESearchCase::CaseSensitive) || Path.Equals(TEXT("README.md"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (!Path.StartsWith(TEXT("src/"), ESearchCase::CaseSensitive) || !Path.EndsWith(TEXT(".rs"), ESearchCase::CaseSensitive) || Path.Len() <= 7)
		{
			return false;
		}
		const FString Body = Path.Mid(4, Path.Len() - 7);
		if (Body.StartsWith(TEXT("/")) || Body.EndsWith(TEXT("/")) || Body.Contains(TEXT("//")))
		{
			return false;
		}
		for (int32 Index = 0; Index < Body.Len(); ++Index)
		{
			const TCHAR Char = Body[Index];
			const bool bAllowed = (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z')) || (Char >= TEXT('0') && Char <= TEXT('9'))
				|| Char == TEXT('_') || Char == TEXT('-') || Char == TEXT('/');
			if (!bAllowed)
			{
				return false;
			}
		}
		return true;
	}

	bool StudioProjReadFiles(const TSharedPtr<FJsonObject>& Object, TArray<FCrowdyStudioProjectFile>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Object->TryGetArrayField(TEXT("files"), Rows) || Rows->Num() > StudioProjMaxFilesOnCreate)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			const TSharedPtr<FJsonObject> File = StudioProjElement(Row);
			FCrowdyStudioProjectFile& Read = Out.AddDefaulted_GetRef();
			FString Target;
			int64 Revision = 0;
			const bool bRead = File.IsValid() && StudioProjReadText(File, TEXT("target"), StudioProjMaxIdChars, Target) && StudioProjReadTarget(Target, Read.Target)
				&& StudioProjReadText(File, TEXT("path"), StudioProjMaxTextChars, Read.Path) && StudioProjReadText(File, TEXT("content"), StudioProjMaxFileBytes, Read.Content)
				&& ReadWholeInt64(File, TEXT("revision"), Revision);
			if (!bRead || !StudioProjPathReadable(Read.Path))
			{
				return false;
			}
			Read.Revision = BigInt(Revision);
		}
		return true;
	}

	bool StudioProjReadInfo(const TSharedPtr<FJsonObject>& Object, FCrowdyStudioProjectInfo& Out)
	{
		return StudioProjReadProject(Object, Out.Project) && StudioProjReadFiles(Object, Out.Files);
	}

	// Every row must read, so a list is never shorter than the platform's without saying so.
	bool StudioProjReadList(const FCrowdyCppJsonResult& Result, FCrowdyStudioProjectList& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = StudioProjAnswerArray(Result, TEXT("crowdyStudioProjects"));
		if (!Rows || Rows->Num() > StudioProjPageSize)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Row : *Rows)
		{
			if (!StudioProjReadProject(StudioProjElement(Row), Out.Projects.AddDefaulted_GetRef()))
			{
				return false;
			}
		}
		return true;
	}

	void StudioProjFinishInfo(const TCHAR* Call, const TFunction<void(const FCrowdyStudioProjectInfo&)>& OnDone, const FCrowdyCppJsonResult& Result,
		const TCHAR* Field, const FString& AskedId = FString())
	{
		FCrowdyStudioProjectInfo Info;
		const TSharedPtr<FJsonObject> Answer = StudioProjAnswerObject(Result, Field);
		if (!Answer.IsValid() || !StudioProjReadInfo(Answer, Info) || (!AskedId.IsEmpty() && !Info.Project.ProjectId.Equals(AskedId, ESearchCase::IgnoreCase)))
		{
			StudioProjFinish(Call, OnDone, FCrowdyStudioProjectInfo(), ECrowdyStudioResult::BadReply, TEXT("The answer holds no well-formed project."));
			return;
		}
		StudioProjFinish(Call, OnDone, MoveTemp(Info), ECrowdyStudioResult::Success, FString());
	}

	TSharedPtr<FJsonObject> StudioProjListVariables(int64 AppId, int32 Limit, int32 Offset)
	{
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("appId"), BigInt(AppId));
		Variables->SetBoolField(TEXT("includeArchived"), false);
		Variables->SetNumberField(TEXT("limit"), Limit);
		Variables->SetNumberField(TEXT("offset"), Offset);
		return Variables;
	}

	bool StudioProjIsModuleChar(TCHAR Char)
	{
		return (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('-') || Char == TEXT('_');
	}

	bool StudioProjModuleValid(const FString& Module)
	{
		if (Module.Len() > StudioProjMaxModuleChars)
		{
			return false;
		}
		for (int32 Index = 0; Index < Module.Len(); ++Index)
		{
			if (!StudioProjIsModuleChar(Module[Index]))
			{
				return false;
			}
		}
		return true;
	}

	// The lowercase module-name form of a project name, or empty when nothing of it fits.
	FString StudioProjSlug(const FString& Name)
	{
		FString Slug;
		const FString Lower = Name.ToLower();
		for (int32 Index = 0; Index < Lower.Len(); ++Index)
		{
			const TCHAR Char = Lower[Index];
			if (StudioProjIsModuleChar(Char) && Char != TEXT('-'))
			{
				Slug.AppendChar(Char);
				continue;
			}
			if (!Slug.IsEmpty() && !Slug.EndsWith(TEXT("-")))
			{
				Slug.AppendChar(TEXT('-'));
			}
		}
		Slug.LeftInline(StudioProjMaxSlugChars);
		while (Slug.EndsWith(TEXT("-")))
		{
			Slug.LeftChopInline(1);
		}
		return Slug;
	}

	// Why the project cannot be sent, or empty when it can.
	FString StudioProjCheckCreate(const FString& Name, ECrowdyStudioProjectKind Kind, const FString& ServerModule, const FString& ClientModule)
	{
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			return TEXT("A project needs a name, so it was not sent.");
		}
		if ((StudioProjHasServer(Kind) && ServerModule.IsEmpty()) || (StudioProjHasClient(Kind) && ClientModule.IsEmpty()))
		{
			return TEXT("Each half of the project needs a module name, so it was not sent.");
		}
		if (!ServerModule.IsEmpty() && !StudioProjHasServer(Kind))
		{
			return TEXT("A CLIENT-only project has no SERVER module, so it was not sent.");
		}
		if (!ClientModule.IsEmpty() && !StudioProjHasClient(Kind))
		{
			return TEXT("A SERVER-only project has no CLIENT module, so it was not sent.");
		}
		if (!StudioProjModuleValid(ServerModule) || !StudioProjModuleValid(ClientModule))
		{
			return FString::Printf(TEXT("A module name is at most %d lowercase letters, digits, - and _, so the project was not sent."), StudioProjMaxModuleChars);
		}
		if (!ServerModule.IsEmpty() && ServerModule == ClientModule)
		{
			return TEXT("The SERVER and CLIENT modules need different names, so the project was not sent.");
		}
		return FString();
	}

	bool StudioProjPathAllowed(const FString& Path)
	{
		if (Path.Equals(TEXT("Cargo.toml"), ESearchCase::CaseSensitive))
		{
			return true;
		}
		const bool bSource = Path.StartsWith(TEXT("src/"), ESearchCase::CaseSensitive) && Path.EndsWith(TEXT(".rs"), ESearchCase::CaseSensitive);
		return bSource && Path.Len() > 7 && !Path.Contains(TEXT("\\")) && !Path.Contains(TEXT("//")) && !Path.Contains(TEXT("/../")) && !Path.Contains(TEXT("/./"));
	}

	FCrowdyStudioProjectFile StudioProjFile(ECrowdyStudioTarget Target, const FString& Path, const FString& Content)
	{
		FCrowdyStudioProjectFile File;
		File.Target = Target;
		File.Path = Path;
		File.Content = Content;
		return File;
	}

	// Why the files cannot be sent, or empty when they can.
	FString StudioProjCheckFiles(const TArray<FCrowdyStudioProjectFile>& Files)
	{
		int32 Counts[2] = {0, 0};
		int64 Bytes[2] = {0, 0};
		if (Files.Num() > StudioProjMaxFilesOnCreate)
		{
			return FString::Printf(TEXT("A new project holds at most %d files, so it was not sent."), StudioProjMaxFilesOnCreate);
		}
		for (const FCrowdyStudioProjectFile& File : Files)
		{
			const int32 Side = File.Target == ECrowdyStudioTarget::Server ? 0 : 1;
			const int64 FileBytes = FTCHARToUTF8(*File.Content, File.Content.Len()).Length();
			++Counts[Side];
			Bytes[Side] += FileBytes;
			if (FileBytes > StudioProjMaxFileBytes)
			{
				return FString::Printf(TEXT("A starter file is over %d bytes, so the project was not sent."), StudioProjMaxFileBytes);
			}
		}
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (Counts[Side] > StudioProjMaxFilesPerTarget || Bytes[Side] > StudioProjMaxTargetBytes)
			{
				return FString::Printf(TEXT("A half holds at most %d files and %d bytes, so the project was not sent."), StudioProjMaxFilesPerTarget, StudioProjMaxTargetBytes);
			}
		}
		return FString();
	}

	// The create input without its appId: the pairing is always sent, never left to the platform's default.
	TSharedPtr<FJsonObject> StudioProjCreateInput(const FString& Name, ECrowdyStudioProjectKind Kind, const FString& ServerModule, const FString& ClientModule,
		const TArray<FCrowdyModFile>& ServerStarter, FString& OutProblem)
	{
		TArray<FCrowdyStudioProjectFile> Files;
		if (StudioProjHasServer(Kind))
		{
			for (const FCrowdyModFile& File : ServerStarter)
			{
				if (StudioProjPathAllowed(File.Path))
				{
					Files.Add(StudioProjFile(ECrowdyStudioTarget::Server, File.Path, File.Content));
				}
			}
		}
		if (StudioProjHasClient(Kind))
		{
			Files.Add(StudioProjFile(ECrowdyStudioTarget::Client, CrowdyStudioClientStarter::CargoPath, CrowdyStudioClientStarter::CargoToml));
			Files.Add(StudioProjFile(ECrowdyStudioTarget::Client, CrowdyStudioClientStarter::LibPath, CrowdyStudioClientStarter::LibRs));
		}
		OutProblem = StudioProjCheckFiles(Files);
		if (!OutProblem.IsEmpty())
		{
			return nullptr;
		}
		TArray<TSharedPtr<FJsonValue>> FileValues;
		for (const FCrowdyStudioProjectFile& File : Files)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("target"), File.Target == ECrowdyStudioTarget::Server ? TEXT("SERVER") : TEXT("CLIENT"));
			Entry->SetStringField(TEXT("path"), File.Path);
			Entry->SetStringField(TEXT("content"), File.Content);
			FileValues.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("name"), Name);
		Input->SetStringField(TEXT("pairingPreference"), StudioProjKindName(Kind));
		if (!ServerModule.IsEmpty())
		{
			Input->SetStringField(TEXT("serverModuleName"), ServerModule);
		}
		if (!ClientModule.IsEmpty())
		{
			Input->SetStringField(TEXT("clientModuleName"), ClientModule);
		}
		Input->SetArrayField(TEXT("initialFiles"), FileValues);
		return Input;
	}
}

void UCrowdyStudioProjects::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LiveSessionToken = MakeShared<uint8>(0);
}

void UCrowdyStudioProjects::Deinitialize()
{
	LiveSessionToken.Reset();
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyStudioProjects::InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, UCrowdyMods* Mods)
{
	LiveSessionToken = MakeShared<uint8>(0);
	ClientForTest = Client;
	AppIdForTest = AppId;
	ModsForTest = Mods;
}
#endif

int64 UCrowdyStudioProjects::GetAppId() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (AppIdForTest != 0)
	{
		return AppIdForTest;
	}
#endif
	return ResolveAppId(GetGameInstance());
}

bool UCrowdyStudioProjects::HasAppToken() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ClientForTest.IsValid())
	{
		return true;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	return Session && !Session->GetGameToken().IsEmpty();
}

FCrowdyCppClient* UCrowdyStudioProjects::ResolveClient() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (const TSharedPtr<FCrowdyCppClient> Override = ClientForTest.Pin())
	{
		return Override.Get();
	}
#endif
	return ResolveApiClient(GetGameInstance(), StudioProjLogName);
}

UCrowdyMods* UCrowdyStudioProjects::ResolveMods() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyMods* Override = ModsForTest.Get())
	{
		return Override;
	}
#endif
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyMods>() : nullptr;
}

void UCrowdyStudioProjects::SendStudioOp(const TCHAR* Operation, const TSharedPtr<FJsonObject>& Variables, TFunction<void(const FCrowdyCppJsonResult&)> OnAnswer,
	TFunction<void(ECrowdyStudioResult, const FString&, const FString&)> OnFailed)
{
	const bool bSignedIn = HasAppToken();
	FCrowdyCppClient* Client = bSignedIn ? ResolveClient() : nullptr;
	if (!Client)
	{
		DeferWhileLive(LiveSessionToken, [OnFailed = MoveTemp(OnFailed), bSignedIn]()
		{
			OnFailed(bSignedIn ? ECrowdyStudioResult::Failed : ECrowdyStudioResult::NotSignedIn, bSignedIn ? StudioProjNoClient : StudioProjNotSignedIn, FString());
		});
		return;
	}
	const int64 AppId = GetAppId();

	TWeakObjectPtr<UCrowdyStudioProjects> WeakThis(this);
	RunOpDeferred(*Client, ECrowdyCppApiDomain::CrowdyStudio, Operation, Variables, LiveSessionToken,
		[WeakThis, AppId, OnAnswer = MoveTemp(OnAnswer), OnFailed = MoveTemp(OnFailed)](FCrowdyCppJsonResult Result)
		{
			const UCrowdyStudioProjects* Self = WeakThis.Get();
			if (!Self || Self->GetAppId() != AppId)
			{
				OnFailed(ECrowdyStudioResult::Failed, StudioProjAppChanged, FString());
				return;
			}
			if (!Result.bTransportOk)
			{
				OnFailed(StudioProjFailureResult(Result), FailureMessage(Result), FailureCode(Result));
				return;
			}
			if (!Result.Data.IsValid())
			{
				OnFailed(ECrowdyStudioResult::BadReply, StudioProjNoAnswer, FString());
				return;
			}
			OnAnswer(Result);
		});
}

void UCrowdyStudioProjects::ListProjects(TFunction<void(const FCrowdyStudioProjectList&)> OnDone)
{
	TWeakObjectPtr<UCrowdyStudioProjects> WeakThis(this);
	SendStudioOp(TEXT("CrowdyStudioProjects"), StudioProjListVariables(GetAppId(), StudioProjPageSize, 0),
		[WeakThis, OnDone](const FCrowdyCppJsonResult& Result)
		{
			FCrowdyStudioProjectList List;
			if (!StudioProjReadList(Result, List))
			{
				StudioProjFinish(TEXT("List Studio Projects"), OnDone, FCrowdyStudioProjectList(), ECrowdyStudioResult::BadReply, TEXT("The answer holds no well-formed project list."));
				return;
			}
			UCrowdyStudioProjects* Self = WeakThis.Get();
			if (!Self || List.Projects.Num() < StudioProjPageSize)
			{
				StudioProjFinish(TEXT("List Studio Projects"), OnDone, MoveTemp(List), ECrowdyStudioResult::Success, FString());
				return;
			}
			Self->ProbeMore(MoveTemp(List), StudioProjPageSize, OnDone);
		},
		StudioProjFailWith<FCrowdyStudioProjectList>(TEXT("List Studio Projects"), OnDone));
}

void UCrowdyStudioProjects::ProbeMore(FCrowdyStudioProjectList List, int32 Offset, TFunction<void(const FCrowdyStudioProjectList&)> OnDone)
{
	SendStudioOp(TEXT("CrowdyStudioProjects"), StudioProjListVariables(GetAppId(), 1, Offset),
		[List = MoveTemp(List), OnDone](const FCrowdyCppJsonResult& Result) mutable
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = StudioProjAnswerArray(Result, TEXT("crowdyStudioProjects"));
			if (!Rows || Rows->Num() > 1)
			{
				StudioProjFinish(TEXT("List Studio Projects"), OnDone, FCrowdyStudioProjectList(), ECrowdyStudioResult::BadReply, TEXT("The answer holds no well-formed project list."));
				return;
			}
			List.bMore = Rows->Num() == 1;
			StudioProjFinish(TEXT("List Studio Projects"), OnDone, MoveTemp(List), ECrowdyStudioResult::Success, FString());
		},
		StudioProjFailWith<FCrowdyStudioProjectList>(TEXT("List Studio Projects"), OnDone));
}

void UCrowdyStudioProjects::GetProject(const FString& ProjectId, TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone)
{
	if (ProjectId.IsEmpty() || ProjectId.Len() > StudioProjMaxIdChars)
	{
		DeferWhileLive(LiveSessionToken, [OnDone = MoveTemp(OnDone)]()
		{
			StudioProjFinish(TEXT("Get Studio Project"), OnDone, FCrowdyStudioProjectInfo(), ECrowdyStudioResult::Refused, TEXT("A project id is 1 to 64 characters, so nothing was sent."));
		});
		return;
	}
	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	Variables->SetStringField(TEXT("projectId"), ProjectId);
	SendStudioOp(TEXT("CrowdyStudioProject"), Variables,
		[OnDone, ProjectId](const FCrowdyCppJsonResult& Result)
		{
			StudioProjFinishInfo(TEXT("Get Studio Project"), OnDone, Result, TEXT("crowdyStudioProject"), ProjectId);
		},
		StudioProjFailWith<FCrowdyStudioProjectInfo>(TEXT("Get Studio Project"), OnDone));
}

void UCrowdyStudioProjects::CreateProject(const FString& Name, ECrowdyStudioProjectKind Kind, TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone)
{
	const FString Slug = StudioProjSlug(Name);
	const FString ServerModule = StudioProjHasServer(Kind) ? Slug : FString();
	const FString ClientModule = StudioProjHasClient(Kind) && !Slug.IsEmpty() ? Slug + TEXT("-client") : FString();
	CreateProject(Name, Kind, ServerModule, ClientModule, MoveTemp(OnDone));
}

void UCrowdyStudioProjects::CreateProject(const FString& Name, ECrowdyStudioProjectKind Kind, const FString& ServerModule, const FString& ClientModule,
	TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone)
{
	const FString Problem = StudioProjCheckCreate(Name, Kind, ServerModule, ClientModule);
	if (!Problem.IsEmpty())
	{
		DeferWhileLive(LiveSessionToken, [OnDone = MoveTemp(OnDone), Problem]()
		{
			StudioProjFinish(TEXT("Create Studio Project"), OnDone, FCrowdyStudioProjectInfo(), ECrowdyStudioResult::Refused, Problem);
		});
		return;
	}

	TWeakObjectPtr<UCrowdyStudioProjects> WeakThis(this);
	const TFunction<void(const TArray<FCrowdyModFile>&)> Proceed = [WeakThis, Name, Kind, ServerModule, ClientModule, OnDone](const TArray<FCrowdyModFile>& ServerStarter)
	{
		UCrowdyStudioProjects* Self = WeakThis.Get();
		if (!Self || !Self->LiveSessionToken.IsValid())
		{
			return;
		}
		FString StarterProblem;
		const TSharedPtr<FJsonObject> Input = StudioProjCreateInput(Name, Kind, ServerModule, ClientModule, ServerStarter, StarterProblem);
		if (!Input.IsValid())
		{
			DeferWhileLive(Self->LiveSessionToken, [OnDone, StarterProblem]()
			{
				StudioProjFinish(TEXT("Create Studio Project"), OnDone, FCrowdyStudioProjectInfo(), ECrowdyStudioResult::Refused, StarterProblem);
			});
			return;
		}
		Self->SendCreate(Input, OnDone);
	};

	if (!StudioProjHasServer(Kind))
	{
		Proceed(TArray<FCrowdyModFile>());
		return;
	}
	UCrowdyMods* Mods = ResolveMods();
	if (!Mods)
	{
		DeferWhileLive(LiveSessionToken, [OnDone = MoveTemp(OnDone)]()
		{
			StudioProjFinish(TEXT("Create Studio Project"), OnDone, FCrowdyStudioProjectInfo(), ECrowdyStudioResult::Failed, StudioProjNoMods);
		});
		return;
	}
	Mods->GetStarter([WeakThis, Proceed, OnDone](const FCrowdyModStarter& Starter)
	{
		const UCrowdyStudioProjects* Self = WeakThis.Get();
		if (!Self || !Self->LiveSessionToken.IsValid())
		{
			return;
		}
		if (Starter.Result != ECrowdyModResult::Success)
		{
			StudioProjFinish(TEXT("Create Studio Project"), OnDone, FCrowdyStudioProjectInfo(), StudioProjFromModResult(Starter.Result), Starter.Message);
			return;
		}
		Proceed(Starter.Files);
	});
}

void UCrowdyStudioProjects::SendCreate(const TSharedPtr<FJsonObject>& Input, TFunction<void(const FCrowdyStudioProjectInfo&)> OnDone)
{
	Input->SetStringField(TEXT("appId"), BigInt(GetAppId()));
	SendStudioOp(TEXT("CrowdyStudioProjectCreate"), WrapInput(Input),
		[OnDone](const FCrowdyCppJsonResult& Result)
		{
			StudioProjFinishInfo(TEXT("Create Studio Project"), OnDone, Result, TEXT("crowdyStudioProjectCreate"));
		},
		StudioProjFailWith<FCrowdyStudioProjectInfo>(TEXT("Create Studio Project"), OnDone));
}
