#include "FCrowdyQueryParser.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"


FCrowdyQueryParser::FCrowdyQueryParser()
{
	InitializeResponseTypeMap();
}

EQueryResponseType FCrowdyQueryParser::ParseResponse(const FString& ResponseString,
                                                     TSharedPtr<FJsonObject>& OutParsedData)
{
	if (!ensure(!ResponseString.IsEmpty()))
		return EQueryResponseType::Error;
	
	const TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(ResponseString);
	
	if (!FJsonSerializer::Deserialize(JsonReader, OutParsedData) || !OutParsedData.IsValid())
		return EQueryResponseType::Error;
	
	if (!ensure(OutParsedData.IsValid()))
		return EQueryResponseType::Error;
	
	return DetermineResponseType(OutParsedData);
}

TSharedPtr<FJsonObject> FCrowdyQueryParser::GetDataObject(const TSharedPtr<FJsonObject>& ParsedData,
	EQueryResponseType ResponseType)
{
	if (!ensure(ParsedData.IsValid()))
		return nullptr;
	
	const TSharedPtr<FJsonObject>* DataObject;
	
	if (!ParsedData->TryGetObjectField(TEXT("data"), DataObject))
			return nullptr;
		
	FString ResponseTypeName;
	for (const auto& ResponseTypePair : ResponseTypeMap)
	{
		if (ResponseTypePair.Value == ResponseType)
		{
			ResponseTypeName = ResponseTypePair.Key;
			break;
		}
	}
	
	if (!ensure(!ResponseTypeName.IsEmpty()))
		return nullptr;
	
	const TSharedPtr<FJsonObject>* ResponseObject;
	
	if ((*DataObject)->TryGetObjectField(ResponseTypeName, ResponseObject))
		return *ResponseObject;
	
	
	const TArray<TSharedPtr<FJsonValue>>* ResponseArray;
	
	if ((*DataObject)->TryGetArrayField(ResponseTypeName, ResponseArray))
	{
		TSharedPtr<FJsonObject> WrapperObject = MakeShareable(new FJsonObject);
		WrapperObject->SetArrayField(ResponseTypeName, *ResponseArray);
		return WrapperObject;
	}
	
	// Handle null responses (like getChunk: null)
	if ((*DataObject)->HasField(ResponseTypeName))
	{
		TSharedPtr<FJsonObject> NullObject = MakeShareable(new FJsonObject);
		NullObject->SetField(ResponseTypeName, (*DataObject)->TryGetField(ResponseTypeName));
		return NullObject;
	}
	
	return nullptr;
	
}

bool FCrowdyQueryParser::HasErrors(const TSharedPtr<FJsonObject>& ParsedData)
{
	if (!ParsedData.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	return ParsedData->TryGetArrayField(TEXT("errors"), ErrorsArray) && ErrorsArray->Num() > 0;
}

TArray<FString> FCrowdyQueryParser::GetErrorMessages(const TSharedPtr<FJsonObject>& ParsedData)
{
	TArray<FString> ErrorMessages;
    
	if (!HasErrors(ParsedData))
	{
		return ErrorMessages;
	}

	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	if (ParsedData->TryGetArrayField(TEXT("errors"), ErrorsArray))
	{
		for (const auto& ErrorValue : *ErrorsArray)
		{
			const TSharedPtr<FJsonObject>* ErrorObject;
			if (ErrorValue->TryGetObject(ErrorObject))
			{
				FString Message;
				if ((*ErrorObject)->TryGetStringField(TEXT("message"), Message))
				{
					ErrorMessages.Add(Message);
				}
			}
		}
	}

	return ErrorMessages;
}

TArray<int32> FCrowdyQueryParser::GetErrorCodes(const TSharedPtr<FJsonObject>& ParsedData)
{
	TArray<int32> ErrorCodes;
	
	if (!HasErrors(ParsedData))
		return ErrorCodes;
	
	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	if (ParsedData->TryGetArrayField(TEXT("errors"), ErrorsArray))
	{
		for (const auto& ErrorValue : *ErrorsArray)
		{
			const TSharedPtr<FJsonObject>* ErrorObject;
			if (ErrorValue->TryGetObject(ErrorObject))
			{
				int32 Code;
				if ((*ErrorObject)->TryGetNumberField(TEXT("code"), Code))
				{
					ErrorCodes.Add(Code);
				}
			}
		}
	}

	return ErrorCodes;
}

TArray<FString> FCrowdyQueryParser::GetErrorPaths(const TSharedPtr<FJsonObject>& ParsedData)
{
	TArray<FString> ErrorPaths;
	
	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	if (ParsedData->TryGetArrayField(TEXT("errors"), ErrorsArray))
	{
		for (const auto& ErrorValue : *ErrorsArray)
		{
			const TSharedPtr<FJsonObject>* ErrorObject;
			if (ErrorValue->TryGetObject(ErrorObject))
			{
				const TArray<TSharedPtr<FJsonValue>>* PathArray;
				if ((*ErrorObject)->TryGetArrayField(TEXT("path"), PathArray))
				{
					for (const auto& PathValue : *PathArray)
					{
						FString PathString;
						if (PathValue->TryGetString(PathString))
						{
							ErrorPaths.Add(PathString);
						}
					}
				}
			}
		}
	}
	return ErrorPaths;
}

EQueryResponseType FCrowdyQueryParser::DetermineErrorResponseType(const TSharedPtr<FJsonObject>& ParsedData)
{
	TArray<FString> ErrorPaths = GetErrorPaths(ParsedData);
    
	if (ErrorPaths.Num() == 0)
	{
		return EQueryResponseType::Error;
	}

	// Get the first path element to determine the operation type
	const FString& Operation = ErrorPaths[0];
    
	// Use your existing ResponseTypeMap to determine the type
	for (const auto& TypePair : ResponseTypeMap)
	{
		if (TypePair.Key == Operation)
		{
			return TypePair.Value;
		}
	}
    
	return EQueryResponseType::Error;
}

EQueryResponseType FCrowdyQueryParser::DetermineResponseType(const TSharedPtr<FJsonObject>& JsonObject)
{
	// Check for errors first
	if (HasErrors(JsonObject))
	{
		return EQueryResponseType::Error;
	}

	// Check for a data object
	const TSharedPtr<FJsonObject>* DataObject;
	if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
	{
		return EQueryResponseType::Error;
	}

	// Check each possible response type in the data object
	for (const auto& TypePair : ResponseTypeMap)
	{
		if ((*DataObject)->HasField(TypePair.Key))
		{
			return TypePair.Value;
		}
	}

	return EQueryResponseType::Error;
}

void FCrowdyQueryParser::InitializeResponseTypeMap()
{
	ResponseTypeMap.Empty();
	ResponseTypeMap.Add(TEXT("login"), EQueryResponseType::Login);
	ResponseTypeMap.Add(TEXT("register"), EQueryResponseType::Register);
	ResponseTypeMap.Add(TEXT("serverWithLeastClients"), EQueryResponseType::UDP_Info);
	ResponseTypeMap.Add(TEXT("getChunksByDistance"), EQueryResponseType::GetChunkByDistance);
	ResponseTypeMap.Add(TEXT("updateChunk"), EQueryResponseType::UpdateChunk);
	ResponseTypeMap.Add(TEXT("getVoxelList"), EQueryResponseType::VoxelList);
	ResponseTypeMap.Add(TEXT("createAvatar"), EQueryResponseType::CreateAvatar);
	ResponseTypeMap.Add(TEXT("myAvatars"), EQueryResponseType::MyAvatars);
	ResponseTypeMap.Add(TEXT("updateAvatar"), EQueryResponseType::UpdateAvatar);
	ResponseTypeMap.Add(TEXT("updateAvatarState"), EQueryResponseType::UpdateAvatarState);
	ResponseTypeMap.Add(TEXT("teleportRequest"), EQueryResponseType::TeleportRequest);
	ResponseTypeMap.Add(TEXT("updateUserState"), EQueryResponseType::UpdateUserState);
	ResponseTypeMap.Add(TEXT("me"), EQueryResponseType::GetUserState);
	ResponseTypeMap.Add("deleteAvatar", EQueryResponseType::DeleteAvatar);
	ResponseTypeMap.Add("versionInfo", EQueryResponseType::VersionInfo);
	ResponseTypeMap.Add("listVoxelUpdatesByDistance", EQueryResponseType::ListVoxelUpdatesByDistance);
}
