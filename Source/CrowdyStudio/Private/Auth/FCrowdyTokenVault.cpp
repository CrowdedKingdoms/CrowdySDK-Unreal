// Fill out your copyright notice in the Description page of Project Settings.

#include "Auth/FCrowdyTokenVault.h"

#include "Security/FCrowdySecretFile.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"

namespace
{
	// Scope value written for a legacy bare-token file (no JSON record): org token, matching the
	// pre-record restore behaviour. Kept in sync with ECrowdyStudioAuthScope::OrgToken.
	constexpr int32 LegacyScope = 2;
}

FString FCrowdyTokenVault::GetCredentialFilePath()
{
	return FPaths::ProjectSavedDir() / TEXT("CrowdyStudio/credentials.bin");
}

bool FCrowdyTokenVault::Save(const FString& Token, int32 Scope, int64 UserId)
{
	const TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
	Record->SetStringField(TEXT("token"), Token);
	Record->SetNumberField(TEXT("scope"), Scope);
	// userId is a BigInt on the wire; store it as a string so no precision is lost round-tripping it.
	Record->SetStringField(TEXT("userId"), FString::Printf(TEXT("%lld"), UserId));

	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Record, Writer);

	return FCrowdySecretFile::SaveString(GetCredentialFilePath(), Serialized);
}

bool FCrowdyTokenVault::Load(FString& OutToken, int32& OutScope, int64& OutUserId)
{
	FString Blob;
	if (!FCrowdySecretFile::LoadString(GetCredentialFilePath(), Blob) || Blob.IsEmpty())
	{
		return false;
	}

	// A JSON record carries the scope + user id; anything else is a legacy bare token.
	TSharedPtr<FJsonObject> Record;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Blob);
	if (FJsonSerializer::Deserialize(Reader, Record) && Record.IsValid())
	{
		FString Token;
		if (Record->TryGetStringField(TEXT("token"), Token) && !Token.IsEmpty())
		{
			OutToken = Token;
			OutScope = static_cast<int32>(Record->GetNumberField(TEXT("scope")));
			FString UserIdStr;
			OutUserId = Record->TryGetStringField(TEXT("userId"), UserIdStr) ? FCString::Atoi64(*UserIdStr) : 0;
			return true;
		}
	}

	OutToken = Blob;
	OutScope = LegacyScope;
	OutUserId = 0;
	return true;
}

bool FCrowdyTokenVault::Load(FString& OutToken)
{
	int32 Scope = 0;
	int64 UserId = 0;
	return Load(OutToken, Scope, UserId);
}

void FCrowdyTokenVault::Clear()
{
	FCrowdySecretFile::Delete(GetCredentialFilePath());
}
