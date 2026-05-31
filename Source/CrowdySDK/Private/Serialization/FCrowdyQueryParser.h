#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"

class CROWDYSDK_API FCrowdyQueryParser
{
public:
	
	FCrowdyQueryParser();
	~FCrowdyQueryParser() = default;
	
	[[nodiscard]] EQueryResponseType ParseResponse(const FString& ResponseString, TSharedPtr<FJsonObject>& OutParsedData);
	
	[[nodiscard]] TSharedPtr<FJsonObject> GetDataObject(const TSharedPtr<FJsonObject>& ParsedData, EQueryResponseType ResponseType);
	
	[[nodiscard]] bool HasErrors(const TSharedPtr<FJsonObject>& ParsedData);
	
	TArray<FString> GetErrorMessages(const TSharedPtr<FJsonObject>& ParsedData);
	
	TArray<int32> GetErrorCodes(const TSharedPtr<FJsonObject>& ParsedData);
	
	TArray<FString> GetErrorPaths(const TSharedPtr<FJsonObject>& ParsedData);
	
	EQueryResponseType DetermineErrorResponseType(const TSharedPtr<FJsonObject>& ParsedData);
	
	
private:
	
	[[nodiscard]] EQueryResponseType DetermineResponseType(const TSharedPtr<FJsonObject>& JsonObject);
	
	TMap<FString, EQueryResponseType> ResponseTypeMap;
	
	void InitializeResponseTypeMap();
};
