// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyModelErrorText.h"

#include "Network/GraphQL/FCrowdyGameApiCodec.h"

FString CrowdyModelErrorText::SubsystemUnavailable()
{
	return TEXT("The Game Model system is unavailable here. Call this from a running game session (a play world).");
}

FString CrowdyModelErrorText::FromInvokeResult(const FCrowdyInvokeResult& Result)
{
	if (!Result.ErrorMessage.IsEmpty())
	{
		return Result.ErrorMessage;
	}
	if (!Result.bTransportOk)
	{
		return TEXT("The request could not reach the server.");
	}
	return TEXT("The server rejected the request.");
}
