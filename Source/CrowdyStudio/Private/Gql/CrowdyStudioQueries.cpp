// Fill out your copyright notice in the Description page of Project Settings.

#include "Gql/CrowdyStudioQueries.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// IDs are the GraphQL BigInt scalar; the server may serialize one as a JSON string or a
	// number, so read both and don't care which it chose.
	int64 ReadId(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object.IsValid())
		{
			return 0;
		}

		FString AsString;
		if (Object->TryGetStringField(Field, AsString))
		{
			return FCString::Atoi64(*AsString);
		}

		int64 AsNumber = 0;
		if (Object->TryGetNumberField(Field, AsNumber))
		{
			return AsNumber;
		}

		return 0;
	}

	TSharedPtr<FJsonObject> GetData(const TSharedPtr<FJsonObject>& Envelope)
	{
		if (!Envelope.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Envelope->TryGetObjectField(TEXT("data"), Data) && Data->IsValid())
		{
			return *Data;
		}

		return nullptr;
	}

	void ReadApp(const TSharedPtr<FJsonObject>& Node, FStudioApp& Out)
	{
		Out.AppId = ReadId(Node, TEXT("appId"));
		Out.OrgId = ReadId(Node, TEXT("orgId"));
		Node->TryGetStringField(TEXT("name"), Out.Name);
		Node->TryGetStringField(TEXT("slug"), Out.Slug);
		Node->TryGetStringField(TEXT("status"), Out.Status);
		Node->TryGetStringField(TEXT("visibility"), Out.Visibility);
		Node->TryGetStringField(TEXT("gameApiUrl"), Out.GameApiUrl);
		Node->TryGetStringField(TEXT("deploymentTarget"), Out.DeploymentTarget);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetStringField(TEXT("createdAt"), Out.CreatedAt);
		Node->TryGetStringField(TEXT("updatedAt"), Out.UpdatedAt);
		Node->TryGetStringField(TEXT("runtimeStatus"), Out.RuntimeStatus);
		Node->TryGetStringField(TEXT("runtimeDenialReason"), Out.RuntimeDenialReason);
		Out.ReservedUdpBytesPerSec = ReadId(Node, TEXT("reservedUdpBytesPerSec"));
		Out.ReservedGraphqlOpsPerSec = ReadId(Node, TEXT("reservedGraphqlOpsPerSec"));

		const TSharedPtr<FJsonObject>* OrgNode = nullptr;
		if (Node->TryGetObjectField(TEXT("org"), OrgNode) && OrgNode->IsValid())
		{
			(*OrgNode)->TryGetStringField(TEXT("name"), Out.OrgName);
			(*OrgNode)->TryGetStringField(TEXT("slug"), Out.OrgSlug);
		}

		bool bSplitMode = false;
		if (Node->TryGetBoolField(TEXT("splitMode"), bSplitMode))
		{
			Out.SplitMode = bSplitMode ? TEXT("true") : TEXT("false");
		}
		// App has no gameApiWsUrl or datacenter; both come from appDiscovery.
	}

	void ReadStringArray(const TSharedPtr<FJsonObject>& Node, const TCHAR* Field, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Node.IsValid() || !Node->TryGetArrayField(Field, Array))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			FString Value;
			if (Entry->TryGetString(Value))
			{
				Out.Add(Value);
			}
		}
	}

	void ReadGroup(const TSharedPtr<FJsonObject>& Node, FStudioGroup& Out)
	{
		Out.GroupId = ReadId(Node, TEXT("groupId"));
		Node->TryGetStringField(TEXT("name"), Out.Name);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetStringField(TEXT("groupType"), Out.GroupType);
		Node->TryGetStringField(TEXT("membershipPolicy"), Out.MembershipPolicy);
		Node->TryGetStringField(TEXT("status"), Out.Status);
	}

	void ReadGroupPolicy(const TSharedPtr<FJsonObject>& Node, FStudioGroupPolicy& Out)
	{
		// A null cap is unlimited, 0 here; reading into the cached policy would keep the old cap.
		Out = FStudioGroupPolicy();
		Out.AppId = ReadId(Node, TEXT("appId"));
		Node->TryGetStringField(TEXT("groupType"), Out.GroupType);
		Node->TryGetStringField(TEXT("creationPolicy"), Out.CreationPolicy);
		Node->TryGetStringField(TEXT("defaultMembershipPolicy"), Out.DefaultMembershipPolicy);
		Node->TryGetNumberField(TEXT("maxMembers"), Out.MaxMembers);
		Node->TryGetNumberField(TEXT("maxGroupsPerUser"), Out.MaxGroupsPerUser);
	}

	// A BigInt field that may be null. Returns false when the field is absent or null so callers
	// can tell "no cap configured" from a real zero.
	bool TryReadBigInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		FString AsString;
		if (Object->TryGetStringField(Field, AsString))
		{
			OutValue = FCString::Atoi64(*AsString);
			return true;
		}

		int64 AsNumber = 0;
		if (Object->TryGetNumberField(Field, AsNumber))
		{
			OutValue = AsNumber;
			return true;
		}

		return false;
	}

	TSharedPtr<FJsonObject> GetDataNode(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (Data->TryGetObjectField(OpName, Node) && Node->IsValid())
		{
			return *Node;
		}
		return nullptr;
	}

	void ReadChunkField(const TSharedPtr<FJsonObject>& Node, const TCHAR* Field, FStudioChunk& Out)
	{
		const TSharedPtr<FJsonObject>* ChunkNode = nullptr;
		if (Node.IsValid() && Node->TryGetObjectField(Field, ChunkNode) && ChunkNode->IsValid())
		{
			Out.X = ReadId(*ChunkNode, TEXT("x"));
			Out.Y = ReadId(*ChunkNode, TEXT("y"));
			Out.Z = ReadId(*ChunkNode, TEXT("z"));
		}
	}

	void ReadGridGroupGrant(const TSharedPtr<FJsonObject>& Node, FStudioGridGroupGrant& Out)
	{
		Out.GridId = ReadId(Node, TEXT("gridId"));
		Out.GroupId = ReadId(Node, TEXT("groupId"));
		int64 RoleId = 0;
		Out.bHasRole = TryReadBigInt(Node, TEXT("groupRoleId"), RoleId);
		Out.GroupRoleId = RoleId;
		Node->TryGetStringField(TEXT("permissionKey"), Out.PermissionKey);
		Node->TryGetStringField(TEXT("expiresAt"), Out.ExpiresAt);
	}

	void ReadFeature(const TSharedPtr<FJsonObject>& Node, FStudioAppFeature& Out)
	{
		Node->TryGetStringField(TEXT("featureKey"), Out.FeatureKey);
		Node->TryGetStringField(TEXT("description"), Out.Description);
	}

	void ReadTierFeature(const TSharedPtr<FJsonObject>& Node, FStudioTierFeature& Out)
	{
		Out.TierId = ReadId(Node, TEXT("tierId"));
		Node->TryGetStringField(TEXT("featureKey"), Out.FeatureKey);
	}

}

namespace CrowdyStudioGql
{
	void ParseOrganizations(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioOrg>>& OutOrgs)
	{
		OutOrgs.Reset();

		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("myOrganizations"), Array))
		{
			return;
		}

		// Each entry is an OrgMembership; the organization itself sits under "org".
		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Membership = nullptr;
			if (!Entry->TryGetObject(Membership) || !Membership->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* OrgNode = nullptr;
			if (!(*Membership)->TryGetObjectField(TEXT("org"), OrgNode) || !OrgNode->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioOrg> Org = MakeShared<FStudioOrg>();
			Org->OrgId = ReadId(*OrgNode, TEXT("orgId"));
			(*OrgNode)->TryGetStringField(TEXT("name"), Org->Name);
			(*OrgNode)->TryGetStringField(TEXT("slug"), Org->Slug);

			const TArray<TSharedPtr<FJsonValue>>* PermissionValues = nullptr;
			if ((*Membership)->TryGetArrayField(TEXT("permissions"), PermissionValues))
			{
				for (const TSharedPtr<FJsonValue>& PermissionValue : *PermissionValues)
				{
					FString Key;
					if (PermissionValue->TryGetString(Key))
					{
						Org->Permissions.Add(Key);
					}
				}
			}

			OutOrgs.Add(Org);
		}
	}

	TSharedPtr<FStudioOrg> ParseOrganization(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Data->TryGetObjectField(OpName, Node) || !Node->IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FStudioOrg> Org = MakeShared<FStudioOrg>();
		Org->OrgId = ReadId(*Node, TEXT("orgId"));
		(*Node)->TryGetStringField(TEXT("name"), Org->Name);
		(*Node)->TryGetStringField(TEXT("slug"), Org->Slug);
		return Org;
	}

	void ParseApps(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioApp>>& OutApps)
	{
		OutApps.Reset();

		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("myApps"), Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioApp> App = MakeShared<FStudioApp>();
			ReadApp(*Node, *App);
			OutApps.Add(App);
		}
	}

	TSharedPtr<FStudioApp> ParseApp(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Data->TryGetObjectField(OpName, Node) || !Node->IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FStudioApp> App = MakeShared<FStudioApp>();
		ReadApp(*Node, *App);
		return App;
	}

	bool ParseAppDiscovery(const TSharedPtr<FJsonObject>& Envelope, int64 AppId, FStudioApp& InOutApp)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("appDiscovery"), Array))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid() || ReadId(*Node, TEXT("appId")) != AppId)
			{
				continue;
			}
			// A null answer clears a previously merged value: an app an operator un-placed must not keep its old code.
			// The HTTP endpoint is deliberately NOT taken from here; the app record's own gameApiUrl is the one
			// routing source the config sync writes, and two sources for one field would race on every refresh.
			InOutApp.DatacenterCode.Reset();
			InOutApp.GameApiWsUrl.Reset();
			(*Node)->TryGetStringField(TEXT("datacenterCode"), InOutApp.DatacenterCode);
			(*Node)->TryGetStringField(TEXT("gameApiWsUrl"), InOutApp.GameApiWsUrl);
			return true;
		}
		return false;
	}

	void ParsePlaceableDatacenters(const TSharedPtr<FJsonObject>& Envelope, TArray<FStudioDatacenter>& OutDatacenters, bool& bOutPlacementEnforced)
	{
		OutDatacenters.Reset();
		bOutPlacementEnforced = false;

		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* Root = nullptr;
		if (!Data->TryGetObjectField(TEXT("placeableDatacenters"), Root) || !Root->IsValid())
		{
			return;
		}
		(*Root)->TryGetBoolField(TEXT("placementEnforced"), bOutPlacementEnforced);

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!(*Root)->TryGetArrayField(TEXT("datacenters"), Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}
			FStudioDatacenter& Dc = OutDatacenters.AddDefaulted_GetRef();
			(*Node)->TryGetStringField(TEXT("code"), Dc.Code);
			(*Node)->TryGetStringField(TEXT("gameApiUrl"), Dc.GameApiUrl);
			(*Node)->TryGetStringField(TEXT("gameApiWsUrl"), Dc.GameApiWsUrl);
			(*Node)->TryGetBoolField(TEXT("placeable"), Dc.bPlaceable);
			(*Node)->TryGetNumberField(TEXT("appShardCount"), Dc.AppShardCount);
			(*Node)->TryGetStringField(TEXT("serving"), Dc.Serving);
		}
	}

	int32 ParseFreeAppsPerOrg(const TSharedPtr<FJsonObject>& Envelope)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return 0;
		}
		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Data->TryGetObjectField(TEXT("platformConfig"), Node) || !Node->IsValid())
		{
			return 0;
		}
		int32 Free = 0;
		(*Node)->TryGetNumberField(TEXT("freeAppsPerOrg"), Free);
		return Free;
	}

	TSharedPtr<FJsonObject> BuildCreateAppVariables(int64 OrgId, const FString& Name, const FString& Slug,
		const FString& Datacenter, const FString& Description)
	{
		const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("orgId"), FString::Printf(TEXT("%lld"), OrgId));
		Input->SetStringField(TEXT("name"), Name);
		Input->SetStringField(TEXT("slug"), Slug);
		Input->SetStringField(TEXT("datacenter"), Datacenter);
		if (!Description.IsEmpty())
		{
			Input->SetStringField(TEXT("description"), Description);
		}

		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), Input);
		return Variables;
	}

	FString SlugFromName(const FString& Name)
	{
		FString Slug;
		Slug.Reserve(Name.Len());
		bool bPendingDash = false;
		for (const TCHAR C : Name)
		{
			const bool bKeep = (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9'));
			const bool bUpper = C >= TEXT('A') && C <= TEXT('Z');
			if (!bKeep && !bUpper)
			{
				bPendingDash = !Slug.IsEmpty();
				continue;
			}
			if (bPendingDash)
			{
				Slug.AppendChar(TEXT('-'));
				bPendingDash = false;
			}
			Slug.AppendChar(bUpper ? static_cast<TCHAR>(C - TEXT('A') + TEXT('a')) : C);
		}
		Slug.LeftInline(128);
		// Runs are already collapsed, so one chop keeps the cut idempotent.
		Slug.RemoveFromEnd(TEXT("-"));
		return Slug;
	}

	// Teams & channels (game plane).
	bool ParseGroupPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGroupPolicy& OutPolicy)
	{
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Node = nullptr;
		if (!Data->TryGetObjectField(OpName, Node) || !Node->IsValid())
		{
			return false;
		}

		ReadGroupPolicy(*Node, OutPolicy);
		return true;
	}

	void ParseGroups(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                 TArray<TSharedPtr<FStudioGroup>>& OutGroups)
	{
		OutGroups.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioGroup> Group = MakeShared<FStudioGroup>();
				ReadGroup(*Node, *Group);
				OutGroups.Add(Group);
			}
		}
	}

	void ParseGroupMembers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioGroupMember>>& OutMembers)
	{
		OutMembers.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioGroupMember> Member = MakeShared<FStudioGroupMember>();
			Member->GroupMemberId = ReadId(*Node, TEXT("groupMemberId"));
			Member->UserId = ReadId(*Node, TEXT("userId"));
			(*Node)->TryGetStringField(TEXT("status"), Member->Status);

			const TArray<TSharedPtr<FJsonValue>>* Roles = nullptr;
			if ((*Node)->TryGetArrayField(TEXT("roles"), Roles))
			{
				for (const TSharedPtr<FJsonValue>& RoleEntry : *Roles)
				{
					const TSharedPtr<FJsonObject>* RoleNode = nullptr;
					if (RoleEntry->TryGetObject(RoleNode) && RoleNode->IsValid())
					{
						FString RoleName;
						(*RoleNode)->TryGetStringField(TEXT("roleName"), RoleName);
						Member->RoleNames.Add(RoleName);
						Member->RoleIds.Add(ReadId(*RoleNode, TEXT("groupRoleId")));
					}
				}
			}

			OutMembers.Add(Member);
		}
	}

	void ParseGroupRoles(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioGroupRole>>& OutRoles)
	{
		OutRoles.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Entry->TryGetObject(Node) || !Node->IsValid())
			{
				continue;
			}

			TSharedPtr<FStudioGroupRole> Role = MakeShared<FStudioGroupRole>();
			Role->GroupRoleId = ReadId(*Node, TEXT("groupRoleId"));
			(*Node)->TryGetStringField(TEXT("roleName"), Role->RoleName);
			(*Node)->TryGetBoolField(TEXT("isSystem"), Role->bIsSystem);
			int32 Rank = 0;
			if ((*Node)->TryGetNumberField(TEXT("rank"), Rank))
			{
				Role->Rank = Rank;
			}
			ReadStringArray(*Node, TEXT("permissions"), Role->Permissions);
			OutRoles.Add(Role);
		}
	}

	// Spatial grid (game plane)

	void ParseNearbyGrids(const TSharedPtr<FJsonObject>& Envelope, TArray<TSharedPtr<FStudioGrid>>& OutGrids)
	{
		OutGrids.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(TEXT("nearbyGridPermissions"), Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioGrid> Grid = MakeShared<FStudioGrid>();
				Grid->AppId = ReadId(*Node, TEXT("appId"));
				Grid->GridId = ReadId(*Node, TEXT("gridId"));
				ReadChunkField(*Node, TEXT("lowChunk"), Grid->Low);
				ReadChunkField(*Node, TEXT("highChunk"), Grid->High);
				ReadStringArray(*Node, TEXT("permissionKeys"), Grid->EffectivePermissionKeys);
				OutGrids.Add(Grid);
			}
		}
	}

	bool ParseCreateGrid(const TSharedPtr<FJsonObject>& Envelope, FStudioGrid& OutGrid, FString& OutError)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, TEXT("createGrid"));
		if (!Node.IsValid())
		{
			return false;
		}

		Node->TryGetStringField(TEXT("error"), OutError);

		const TSharedPtr<FJsonObject>* GridNode = nullptr;
		if (Node->TryGetObjectField(TEXT("grid"), GridNode) && GridNode->IsValid())
		{
			OutGrid.GridId = ReadId(*GridNode, TEXT("grid_id"));
			OutGrid.AppId = ReadId(*GridNode, TEXT("app_id"));
			ReadChunkField(*GridNode, TEXT("low_chunk"), OutGrid.Low);
			ReadChunkField(*GridNode, TEXT("high_chunk"), OutGrid.High);
			return true;
		}
		return false;
	}

	void ParseGridPermissionKeys(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, TArray<FString>& OutKeys)
	{
		OutKeys.Reset();
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (Node.IsValid())
		{
			ReadStringArray(Node, TEXT("permissionKeys"), OutKeys);
		}
	}

	void ParseGridGroupGrants(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                          TArray<TSharedPtr<FStudioGridGroupGrant>>& OutGrants)
	{
		OutGrants.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioGridGroupGrant> Grant = MakeShared<FStudioGridGroupGrant>();
				ReadGridGroupGrant(*Node, *Grant);
				OutGrants.Add(Grant);
			}
		}
	}

	void ParseRuntimePermissions(const TSharedPtr<FJsonObject>& Envelope, TArray<FString>& OutKeys)
	{
		// runtimePermissions is a flat [String!]! straight off the data node, not wrapped in an op object.
		OutKeys.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (Data.IsValid())
		{
			ReadStringArray(Data, TEXT("runtimePermissions"), OutKeys);
		}
	}

	// App access (management plane)

	void ParseFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                   TArray<TSharedPtr<FStudioAppFeature>>& OutFeatures)
	{
		OutFeatures.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioAppFeature> Feature = MakeShared<FStudioAppFeature>();
				ReadFeature(*Node, *Feature);
				OutFeatures.Add(Feature);
			}
		}
	}

	void ParseTierFeatures(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioTierFeature>>& OutGrants)
	{
		OutGrants.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioTierFeature> Grant = MakeShared<FStudioTierFeature>();
				ReadTierFeature(*Node, *Grant);
				OutGrants.Add(Grant);
			}
		}
	}

	void ParseAccessTiers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                      TArray<TSharedPtr<FStudioAccessTier>>& OutTiers)
	{
		OutTiers.Reset();
		const TSharedPtr<FJsonObject> Data = GetData(Envelope);
		if (!Data.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data->TryGetArrayField(OpName, Array))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				TSharedPtr<FStudioAccessTier> Tier = MakeShared<FStudioAccessTier>();
				Tier->TierId = ReadId(*Node, TEXT("tierId"));
				(*Node)->TryGetStringField(TEXT("name"), Tier->Name);
				(*Node)->TryGetBoolField(TEXT("isFree"), Tier->bIsFree);
				(*Node)->TryGetBoolField(TEXT("isDefault"), Tier->bIsDefault);
				ReadStringArray(*Node, TEXT("permissionKeys"), Tier->PermissionKeys);
				(*Node)->TryGetStringField(TEXT("status"), Tier->Status);
				OutTiers.Add(Tier);
			}
		}
	}
}
