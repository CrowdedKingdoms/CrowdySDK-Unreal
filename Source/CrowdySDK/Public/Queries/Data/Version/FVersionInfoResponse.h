#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Shared/Types/Structures/Versioning/FGameVersion.h"

struct FVersionInfoResponse : ICrowdyQueryResponse
{
	FGameVersion ServerVersion;
	FGameVersion MinimumClientVersion;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::VersionInfo;
	}
	
	virtual FName GetOperationName() const override
	{
		return TEXT("Version Information Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("The JSON Object is invalid.");
			bIsValid = false;
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject;

		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract Data Object");
			bIsValid = false;
			return;
		}

		const TSharedPtr<FJsonObject>* VersionInfoObject;
		if (!(*DataObject)->TryGetObjectField(TEXT("versionInfo"), VersionInfoObject) || !VersionInfoObject->IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to extract versionInfo from data object"));
			return;
		}

		// Parse server version
		const TSharedPtr<FJsonObject>* ServerVersionObject;
		if ((*VersionInfoObject)->TryGetObjectField(TEXT("serverVersion"), ServerVersionObject) && ServerVersionObject->
			IsValid())
		{
			ServerVersion.MajorVersion = (*ServerVersionObject)->GetIntegerField(TEXT("major"));
			ServerVersion.MinorVersion = (*ServerVersionObject)->GetIntegerField(TEXT("minor"));
			ServerVersion.PatchVersion = (*ServerVersionObject)->GetIntegerField(TEXT("patch"));
			ServerVersion.BuildNumber = (*ServerVersionObject)->GetIntegerField(TEXT("build"));
		}

		// Parse minimum client version
		const TSharedPtr<FJsonObject>* MinClientVersionObject;
		if ((*VersionInfoObject)->TryGetObjectField(TEXT("minimumClientVersion"), MinClientVersionObject) &&
			MinClientVersionObject->IsValid())
		{
			MinimumClientVersion.MajorVersion = (*MinClientVersionObject)->GetIntegerField(TEXT("major"));
			MinimumClientVersion.MinorVersion = (*MinClientVersionObject)->GetIntegerField(TEXT("minor"));
			MinimumClientVersion.PatchVersion = (*MinClientVersionObject)->GetIntegerField(TEXT("patch"));
			MinimumClientVersion.BuildNumber = (*MinClientVersionObject)->GetIntegerField(TEXT("build"));
		}
		
		bIsValid = true;
	}
};
