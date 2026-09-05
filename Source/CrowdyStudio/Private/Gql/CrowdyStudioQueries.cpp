// Fill out your copyright notice in the Description page of Project Settings.

#include "Gql/CrowdyStudioQueries.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"

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

		bool bSplitMode = false;
		if (Node->TryGetBoolField(TEXT("splitMode"), bSplitMode))
		{
			Out.SplitMode = bSplitMode ? TEXT("true") : TEXT("false");
		}
		// App has no gameApiWsUrl - the WS endpoint comes from platformConfig.
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

	void ReadContainerType(const TSharedPtr<FJsonObject>& Node, FStudioContainerType& Out)
	{
		Out.AppId = ReadId(Node, TEXT("appId"));
		Node->TryGetStringField(TEXT("typeName"), Out.TypeName);
		Node->TryGetStringField(TEXT("displayName"), Out.DisplayName);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetStringField(TEXT("instantiableBy"), Out.InstantiableBy);
		Node->TryGetStringField(TEXT("defaultPropertyVisibility"), Out.DefaultPropertyVisibility);
		Node->TryGetStringField(TEXT("metadataJson"), Out.MetadataJson);
	}

	void ReadPropertyDef(const TSharedPtr<FJsonObject>& Node, FStudioPropertyDef& Out)
	{
		Node->TryGetStringField(TEXT("containerTypeName"), Out.ContainerTypeName);
		Node->TryGetStringField(TEXT("key"), Out.Key);
		Node->TryGetStringField(TEXT("valueType"), Out.ValueType);
		Node->TryGetStringField(TEXT("defaultValueJson"), Out.DefaultValueJson);
		Node->TryGetStringField(TEXT("visibility"), Out.Visibility);
		Node->TryGetStringField(TEXT("writable"), Out.Writable);
		Node->TryGetStringField(TEXT("description"), Out.Description);
	}

	void ReadFunction(const TSharedPtr<FJsonObject>& Node, FStudioFunction& Out)
	{
		Node->TryGetStringField(TEXT("functionId"), Out.FunctionId);
		Node->TryGetStringField(TEXT("name"), Out.Name);
		Node->TryGetStringField(TEXT("containerTypeName"), Out.ContainerTypeName);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetStringField(TEXT("returnType"), Out.ReturnType);
		Node->TryGetStringField(TEXT("returnExpression"), Out.ReturnExpression);
		Node->TryGetStringField(TEXT("invokeScope"), Out.InvokeScope);
		Node->TryGetBoolField(TEXT("autonomousInvocable"), Out.bAutonomousInvocable);
		Node->TryGetStringField(TEXT("invokePolicyJson"), Out.InvokePolicyJson);
		ReadStringArray(Node, TEXT("warnings"), Out.Warnings);

		const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
		if (Node->TryGetArrayField(TEXT("parameters"), Params))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Params)
			{
				const TSharedPtr<FJsonObject>* P = nullptr;
				if (Entry->TryGetObject(P) && P->IsValid())
				{
					FStudioFunctionParam Param;
					(*P)->TryGetStringField(TEXT("name"), Param.Name);
					(*P)->TryGetStringField(TEXT("valueType"), Param.ValueType);
					(*P)->TryGetBoolField(TEXT("required"), Param.bRequired);
					(*P)->TryGetStringField(TEXT("defaultValueJson"), Param.DefaultValueJson);
					(*P)->TryGetStringField(TEXT("description"), Param.Description);
					(*P)->TryGetNumberField(TEXT("sortOrder"), Param.SortOrder);
					Out.Parameters.Add(Param);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Muts = nullptr;
		if (Node->TryGetArrayField(TEXT("mutations"), Muts))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Muts)
			{
				const TSharedPtr<FJsonObject>* M = nullptr;
				if (Entry->TryGetObject(M) && M->IsValid())
				{
					FStudioFunctionMutation Mut;
					(*M)->TryGetStringField(TEXT("target"), Mut.Target);
					(*M)->TryGetStringField(TEXT("property"), Mut.Property);
					(*M)->TryGetStringField(TEXT("expression"), Mut.Expression);
					Out.Mutations.Add(Mut);
				}
			}
		}

		// Read notifications verbatim so the schema sync can re-emit them on an upsert (the effect front-end does
		// not author them yet; reading + re-emitting preserves any seed/console-authored ones). Shape matches
		// FunctionNotificationInput: { kind, emitAs, args: [{ name, expression }] }.
		const TArray<TSharedPtr<FJsonValue>>* Notifs = nullptr;
		if (Node->TryGetArrayField(TEXT("notifications"), Notifs))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Notifs)
			{
				const TSharedPtr<FJsonObject>* N = nullptr;
				if (Entry->TryGetObject(N) && N->IsValid())
				{
					FCrowdyGameModelNotification Notification;
					(*N)->TryGetStringField(TEXT("kind"), Notification.Kind);
					(*N)->TryGetStringField(TEXT("emitAs"), Notification.EmitAs);
					const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
					if ((*N)->TryGetArrayField(TEXT("args"), Args))
					{
						for (const TSharedPtr<FJsonValue>& ArgEntry : *Args)
						{
							const TSharedPtr<FJsonObject>* A = nullptr;
							if (ArgEntry->TryGetObject(A) && A->IsValid())
							{
								FCrowdyGameModelNotificationArg Arg;
								(*A)->TryGetStringField(TEXT("name"), Arg.Name);
								(*A)->TryGetStringField(TEXT("expression"), Arg.Expression);
								Notification.Args.Add(Arg);
							}
						}
					}
					Out.Notifications.Add(Notification);
				}
			}
		}

		// Timers, shape FunctionTimerInput: { functionName, target, delayMsExpression, dedupeKeyExpression,
		// params: [{ name, expression }] }. Read so the schema diff can recognize an already-current set; the
		// effect asset is their sole author, so a sync replaces rather than merges them.
		const TArray<TSharedPtr<FJsonValue>>* TimerEntries = nullptr;
		if (Node->TryGetArrayField(TEXT("timers"), TimerEntries))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *TimerEntries)
			{
				const TSharedPtr<FJsonObject>* T = nullptr;
				if (Entry->TryGetObject(T) && T->IsValid())
				{
					FCrowdyGameModelTimer Timer;
					(*T)->TryGetStringField(TEXT("functionName"), Timer.FunctionName);
					(*T)->TryGetStringField(TEXT("target"), Timer.Target);
					(*T)->TryGetStringField(TEXT("delayMsExpression"), Timer.DelayMsExpression);
					(*T)->TryGetStringField(TEXT("dedupeKeyExpression"), Timer.DedupeKeyExpression);
					const TArray<TSharedPtr<FJsonValue>>* TimerParamEntries = nullptr;
					if ((*T)->TryGetArrayField(TEXT("params"), TimerParamEntries))
					{
						for (const TSharedPtr<FJsonValue>& ParamEntry : *TimerParamEntries)
						{
							const TSharedPtr<FJsonObject>* P = nullptr;
							if (ParamEntry->TryGetObject(P) && P->IsValid())
							{
								FCrowdyGameModelTimerParam Param;
								(*P)->TryGetStringField(TEXT("name"), Param.Name);
								(*P)->TryGetStringField(TEXT("expression"), Param.Expression);
								Timer.Params.Add(Param);
							}
						}
					}
					Out.Timers.Add(Timer);
				}
			}
		}
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

	void ReadAutomation(const TSharedPtr<FJsonObject>& Node, FStudioAutomation& Out)
	{
		Node->TryGetStringField(TEXT("automationId"), Out.AutomationId);
		Node->TryGetStringField(TEXT("name"), Out.Name);
		Node->TryGetStringField(TEXT("description"), Out.Description);
		Node->TryGetBoolField(TEXT("enabled"), Out.bEnabled);
		Node->TryGetStringField(TEXT("actionKind"), Out.ActionKind);
		Node->TryGetStringField(TEXT("functionName"), Out.FunctionName);
		Node->TryGetStringField(TEXT("targetMode"), Out.TargetMode);
		Node->TryGetStringField(TEXT("selfContainerId"), Out.SelfContainerId);
		Node->TryGetStringField(TEXT("targetTypeName"), Out.TargetTypeName);
		Node->TryGetStringField(TEXT("sessionId"), Out.SessionId);
		Node->TryGetStringField(TEXT("paramsJson"), Out.ParamsJson);
		Node->TryGetStringField(TEXT("selectorJson"), Out.SelectorJson);
		Node->TryGetStringField(TEXT("triggerType"), Out.TriggerType);
		Node->TryGetStringField(TEXT("scheduleKind"), Out.ScheduleKind);
		Node->TryGetNumberField(TEXT("intervalMs"), Out.IntervalMs);
		Node->TryGetStringField(TEXT("cronExpr"), Out.CronExpr);
		Node->TryGetNumberField(TEXT("maxTargets"), Out.MaxTargets);
		Node->TryGetNumberField(TEXT("gasLimit"), Out.GasLimit);
		Node->TryGetNumberField(TEXT("runTimeoutMs"), Out.RunTimeoutMs);
		Node->TryGetNumberField(TEXT("maxRunsPerMinute"), Out.MaxRunsPerMinute);
		Node->TryGetNumberField(TEXT("failureThreshold"), Out.FailureThreshold);
		Node->TryGetNumberField(TEXT("cooldownMs"), Out.CooldownMs);
	}

	// AutomationName is left to the caller (ParseAutomationTriggers), which resolves it from the automationId the wire
	// carries against the automations read in the same plan.
	void ReadAutomationTrigger(const TSharedPtr<FJsonObject>& Node, FStudioAutomationTrigger& Out)
	{
		Node->TryGetStringField(TEXT("triggerId"), Out.TriggerId);
		Node->TryGetStringField(TEXT("onEvent"), Out.OnEvent);
		Node->TryGetStringField(TEXT("functionName"), Out.FunctionName);
		Node->TryGetStringField(TEXT("containerTypeName"), Out.ContainerTypeName);
		Node->TryGetStringField(TEXT("propertyKey"), Out.PropertyKey);
		Node->TryGetStringField(TEXT("writeSource"), Out.WriteSource);
		Node->TryGetNumberField(TEXT("debounceMs"), Out.DebounceMs);
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

	// Game model (game plane)

	TSharedPtr<FJsonObject> BuildAutomationUpsertVariables(const FCrowdyGameModelAutomationInput& Automation, int64 AppId)
	{
		// The input object is built by the shared marshaller (also used by the editor payload preview) so there is one
		// definition of the wire shape; this wraps it in the mutation's { input: ... } variables.
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), CrowdyGameModelMarshalling::BuildAutomationUpsertInput(Automation, AppId));
		return Variables;
	}

	TSharedPtr<FJsonObject> BuildAutomationTriggerUpsertVariables(const FCrowdyGameModelAutomationTriggerInput& Trigger, int64 AppId)
	{
		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), CrowdyGameModelMarshalling::BuildAutomationTriggerUpsertInput(Trigger, AppId));
		return Variables;
	}

	void ParseContainerTypes(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                         TArray<TSharedPtr<FStudioContainerType>>& OutTypes)
	{
		OutTypes.Reset();
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
				TSharedPtr<FStudioContainerType> Type = MakeShared<FStudioContainerType>();
				ReadContainerType(*Node, *Type);
				OutTypes.Add(Type);
			}
		}
	}

	void ParsePropertyDefs(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                       TArray<TSharedPtr<FStudioPropertyDef>>& OutDefs)
	{
		OutDefs.Reset();
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
				TSharedPtr<FStudioPropertyDef> Def = MakeShared<FStudioPropertyDef>();
				ReadPropertyDef(*Node, *Def);
				OutDefs.Add(Def);
			}
		}
	}

	void ParseFunctions(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                    TArray<TSharedPtr<FStudioFunction>>& OutFns)
	{
		OutFns.Reset();
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
				TSharedPtr<FStudioFunction> Fn = MakeShared<FStudioFunction>();
				ReadFunction(*Node, *Fn);
				OutFns.Add(Fn);
			}
		}
	}

	bool ParseFunction(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioFunction& OutFn)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}
		ReadFunction(Node, OutFn);
		return true;
	}

	void ParseAutomations(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                      TArray<FStudioAutomation>& OutAutomations)
	{
		OutAutomations.Reset();
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
				FStudioAutomation Automation;
				ReadAutomation(*Node, Automation);
				OutAutomations.Add(Automation);
			}
		}
	}

	void ParseAutomationTriggers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                             const TArray<FStudioAutomation>& AutomationsForNameLookup,
	                             TArray<FStudioAutomationTrigger>& OutTriggers)
	{
		OutTriggers.Reset();
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

		// The trigger read-back references its automation by id; the diff keys triggers by automation name, so build
		// an id -> name map from the automations read in the same plan (both queries hit the same pinned app).
		TMap<FString, FString> NameById;
		for (const FStudioAutomation& Automation : AutomationsForNameLookup)
		{
			if (!Automation.AutomationId.IsEmpty())
			{
				NameById.Add(Automation.AutomationId, Automation.Name);
			}
		}

		for (const TSharedPtr<FJsonValue>& Entry : *Array)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (Entry->TryGetObject(Node) && Node->IsValid())
			{
				FStudioAutomationTrigger Trigger;
				ReadAutomationTrigger(*Node, Trigger);
				FString AutomationId;
				(*Node)->TryGetStringField(TEXT("automationId"), AutomationId);
				if (const FString* FoundName = NameById.Find(AutomationId))
				{
					Trigger.AutomationName = *FoundName;
				}
				OutTriggers.Add(Trigger);
			}
		}
	}

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

	bool ParseGameModelPolicy(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioGameModelPolicy& OutPolicy)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}
		OutPolicy.AppId = ReadId(Node, TEXT("appId"));
		Node->TryGetStringField(TEXT("sessionCreationPolicy"), OutPolicy.SessionCreationPolicy);
		Node->TryGetStringField(TEXT("defaultParticipantRole"), OutPolicy.DefaultParticipantRole);
		OutPolicy.bValid = true;
		return true;
	}

	bool ParseModelLint(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioLintReport& OutReport)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}

		OutReport = FStudioLintReport();
		OutReport.AppId = ReadId(Node, TEXT("appId"));
		Node->TryGetNumberField(TEXT("errorCount"), OutReport.ErrorCount);
		Node->TryGetNumberField(TEXT("warningCount"), OutReport.WarningCount);
		Node->TryGetBoolField(TEXT("clean"), OutReport.bClean);

		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (Node->TryGetArrayField(TEXT("findings"), Findings) && Findings)
		{
			OutReport.Findings.Reserve(Findings->Num());
			for (const TSharedPtr<FJsonValue>& Value : *Findings)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
				{
					continue;
				}
				FStudioLintFinding Finding;
				(*Obj)->TryGetStringField(TEXT("code"), Finding.Code);
				(*Obj)->TryGetStringField(TEXT("severity"), Finding.Severity);
				(*Obj)->TryGetStringField(TEXT("subjectKind"), Finding.SubjectKind);
				(*Obj)->TryGetStringField(TEXT("subject"), Finding.Subject);
				(*Obj)->TryGetStringField(TEXT("message"), Finding.Message);
				(*Obj)->TryGetStringField(TEXT("remedy"), Finding.Remedy);
				(*Obj)->TryGetNumberField(TEXT("count"), Finding.Count);
				OutReport.Findings.Add(MoveTemp(Finding));
			}
		}

		// Set last, so a caller reading bRan is looking at a fully populated report rather than a half-filled one.
		OutReport.bRan = true;
		return true;
	}

	bool ParseSeedResult(const TSharedPtr<FJsonObject>& Envelope, FString& OutSummary)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, TEXT("gameModelSeed"));
		if (!Node.IsValid())
		{
			return false;
		}

		int32 Types = 0, Props = 0, Fns = 0, Containers = 0, Edges = 0;
		Node->TryGetNumberField(TEXT("containerTypesCreated"), Types);
		Node->TryGetNumberField(TEXT("propertyDefinitionsCreated"), Props);
		Node->TryGetNumberField(TEXT("functionsCreated"), Fns);
		Node->TryGetNumberField(TEXT("containersCreated"), Containers);
		Node->TryGetNumberField(TEXT("edgesCreated"), Edges);
		OutSummary = FString::Printf(
			TEXT("Seeded %d type(s), %d property def(s), %d function(s), %d container(s), %d edge(s)."),
			Types, Props, Fns, Containers, Edges);
		return true;
	}

	void ParseContainers(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName,
	                     TArray<TSharedPtr<FStudioContainer>>& OutContainers)
	{
		OutContainers.Reset();
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

			TSharedPtr<FStudioContainer> Container = MakeShared<FStudioContainer>();
			(*Node)->TryGetStringField(TEXT("containerId"), Container->ContainerId);
			(*Node)->TryGetStringField(TEXT("sessionId"), Container->SessionId);
			(*Node)->TryGetStringField(TEXT("typeName"), Container->TypeName);
			(*Node)->TryGetStringField(TEXT("displayName"), Container->DisplayName);
			Container->OwnerUserId = ReadId(*Node, TEXT("ownerUserId"));
			(*Node)->TryGetStringField(TEXT("metadataJson"), Container->MetadataJson);
			(*Node)->TryGetStringField(TEXT("bindingKey"), Container->BindingKey);
			OutContainers.Add(Container);
		}
	}

	bool ParseContainerState(const TSharedPtr<FJsonObject>& Envelope, const TCHAR* OpName, FStudioContainerState& OutState)
	{
		const TSharedPtr<FJsonObject> Node = GetDataNode(Envelope, OpName);
		if (!Node.IsValid())
		{
			return false;
		}
		Node->TryGetStringField(TEXT("containerId"), OutState.ContainerId);
		Node->TryGetStringField(TEXT("typeName"), OutState.TypeName);
		Node->TryGetStringField(TEXT("displayName"), OutState.DisplayName);
		OutState.OwnerUserId = ReadId(Node, TEXT("ownerUserId"));
		Node->TryGetStringField(TEXT("propertiesJson"), OutState.PropertiesJson);
		OutState.bValid = true;
		return true;
	}
}
