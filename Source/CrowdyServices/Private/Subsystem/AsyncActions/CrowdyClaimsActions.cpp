#include "Subsystem/AsyncActions/CrowdyClaimsActions.h"
#include "CrowdyServicesActionSupport.h"

namespace
{
	const TCHAR* const ClaimsMissingSubsystem = TEXT("UCrowdyClaims subsystem not found");
}

UCrowdyClaimChunk* UCrowdyClaimChunk::ClaimChunk(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
	UCrowdyClaimChunk* Action = NewObject<UCrowdyClaimChunk>();
	Action->WorldContextObject = WorldContextObject;
	Action->ChunkX = ChunkX;
	Action->ChunkY = ChunkY;
	Action->ChunkZ = ChunkZ;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyClaimChunk::Activate()
{
	UCrowdyClaims* Claims = GetServiceSubsystem<UCrowdyClaims>(WorldContextObject);
	if (!Claims)
	{
		FCrowdyChunkClaim Missing;
		Missing.Message = ClaimsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyClaimChunk> WeakThis(this);
	Claims->ClaimChunk(ChunkX, ChunkY, ChunkZ, [WeakThis](const FCrowdyChunkClaim& Claim)
	{
		if (UCrowdyClaimChunk* Self = WeakThis.Get())
		{
			Self->Finish(Claim);
		}
	});
}

void UCrowdyClaimChunk::Finish(const FCrowdyChunkClaim& Claim)
{
	FCrowdyClaimAsyncResult& Pin = Claim.Result == ECrowdyClaimResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Claim.GridId, Claim.Result, Claim.Message);
	SetReadyToDestroy();
}

UCrowdyReleaseClaim* UCrowdyReleaseClaim::ReleaseClaim(UObject* WorldContextObject, int64 GridId)
{
	UCrowdyReleaseClaim* Action = NewObject<UCrowdyReleaseClaim>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyReleaseClaim::Activate()
{
	UCrowdyClaims* Claims = GetServiceSubsystem<UCrowdyClaims>(WorldContextObject);
	if (!Claims)
	{
		FCrowdyClaimRelease Missing;
		Missing.GridId = GridId;
		Missing.Message = ClaimsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyReleaseClaim> WeakThis(this);
	Claims->ReleaseClaim(GridId, [WeakThis](const FCrowdyClaimRelease& Release)
	{
		if (UCrowdyReleaseClaim* Self = WeakThis.Get())
		{
			Self->Finish(Release);
		}
	});
}

void UCrowdyReleaseClaim::Finish(const FCrowdyClaimRelease& Release)
{
	FCrowdyClaimAsyncResult& Pin = Release.Result == ECrowdyClaimResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Release.GridId, Release.Result, Release.Message);
	SetReadyToDestroy();
}

UCrowdyFindClaims* UCrowdyFindClaims::FindClaims(UObject* WorldContextObject, int64 LowX, int64 LowY, int64 LowZ, int64 HighX, int64 HighY, int64 HighZ)
{
	UCrowdyFindClaims* Action = NewObject<UCrowdyFindClaims>();
	Action->WorldContextObject = WorldContextObject;
	Action->Low = FInt64Vector(LowX, LowY, LowZ);
	Action->High = FInt64Vector(HighX, HighY, HighZ);
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyFindClaims::Activate()
{
	UCrowdyClaims* Claims = GetServiceSubsystem<UCrowdyClaims>(WorldContextObject);
	if (!Claims)
	{
		FCrowdyClaimSearch Missing;
		Missing.Message = ClaimsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyFindClaims> WeakThis(this);
	Claims->FindClaims(Low.X, Low.Y, Low.Z, High.X, High.Y, High.Z, [WeakThis](const FCrowdyClaimSearch& Search)
	{
		if (UCrowdyFindClaims* Self = WeakThis.Get())
		{
			Self->Finish(Search);
		}
	});
}

void UCrowdyFindClaims::Finish(const FCrowdyClaimSearch& Search)
{
	FCrowdyClaimSearchAsyncResult& Pin = Search.Result == ECrowdyClaimResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Search.Claims, Search.Result, Search.Message);
	SetReadyToDestroy();
}

UCrowdyCanEditChunk* UCrowdyCanEditChunk::CanEditChunk(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
	UCrowdyCanEditChunk* Action = NewObject<UCrowdyCanEditChunk>();
	Action->WorldContextObject = WorldContextObject;
	Action->ChunkX = ChunkX;
	Action->ChunkY = ChunkY;
	Action->ChunkZ = ChunkZ;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyCanEditChunk::Activate()
{
	UCrowdyClaims* Claims = GetServiceSubsystem<UCrowdyClaims>(WorldContextObject);
	if (!Claims)
	{
		FCrowdyChunkEditCheck Missing;
		Missing.Message = ClaimsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyCanEditChunk> WeakThis(this);
	Claims->CanEditChunk(ChunkX, ChunkY, ChunkZ, [WeakThis](const FCrowdyChunkEditCheck& Check)
	{
		if (UCrowdyCanEditChunk* Self = WeakThis.Get())
		{
			Self->Finish(Check);
		}
	});
}

void UCrowdyCanEditChunk::Finish(const FCrowdyChunkEditCheck& Check)
{
	FCrowdyChunkEditAsyncResult& Pin = Check.Result == ECrowdyClaimResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Check.bCanEdit, Check.GridId, Check.Result, Check.Message);
	SetReadyToDestroy();
}

UCrowdyGetGridPermissions* UCrowdyGetGridPermissions::GetGridPermissions(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
	UCrowdyGetGridPermissions* Action = NewObject<UCrowdyGetGridPermissions>();
	Action->WorldContextObject = WorldContextObject;
	Action->ChunkX = ChunkX;
	Action->ChunkY = ChunkY;
	Action->ChunkZ = ChunkZ;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyGetGridPermissions::Activate()
{
	UCrowdyClaims* Claims = GetServiceSubsystem<UCrowdyClaims>(WorldContextObject);
	if (!Claims)
	{
		FCrowdyGridPermissions Missing;
		Missing.Message = ClaimsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyGetGridPermissions> WeakThis(this);
	Claims->GetGridPermissions(ChunkX, ChunkY, ChunkZ, [WeakThis](const FCrowdyGridPermissions& Permissions)
	{
		if (UCrowdyGetGridPermissions* Self = WeakThis.Get())
		{
			Self->Finish(Permissions);
		}
	});
}

void UCrowdyGetGridPermissions::Finish(const FCrowdyGridPermissions& Permissions)
{
	FCrowdyGridPermissionsAsyncResult& Pin = Permissions.Result == ECrowdyClaimResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Permissions.Grids, Permissions.Result, Permissions.Message);
	SetReadyToDestroy();
}

UCrowdyListGridChannels* UCrowdyListGridChannels::ListGridChannels(UObject* WorldContextObject, int64 GridId)
{
	UCrowdyListGridChannels* Action = NewObject<UCrowdyListGridChannels>();
	Action->WorldContextObject = WorldContextObject;
	Action->GridId = GridId;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyListGridChannels::Activate()
{
	UCrowdyClaims* Claims = GetServiceSubsystem<UCrowdyClaims>(WorldContextObject);
	if (!Claims)
	{
		FCrowdyGridChannels Missing;
		Missing.GridId = GridId;
		Missing.Message = ClaimsMissingSubsystem;
		Finish(Missing);
		return;
	}
	TWeakObjectPtr<UCrowdyListGridChannels> WeakThis(this);
	Claims->ListGridChannels(GridId, [WeakThis](const FCrowdyGridChannels& Channels)
	{
		if (UCrowdyListGridChannels* Self = WeakThis.Get())
		{
			Self->Finish(Channels);
		}
	});
}

void UCrowdyListGridChannels::Finish(const FCrowdyGridChannels& Channels)
{
	FCrowdyGridChannelsAsyncResult& Pin = Channels.Result == ECrowdyClaimResult::Success ? OnDone : OnFailed;
	Pin.Broadcast(Channels.Channels, Channels.Result, Channels.Message);
	SetReadyToDestroy();
}
