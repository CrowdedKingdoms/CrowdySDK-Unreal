#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMember.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupMembership.h"
#include "Queries/Data/Teams/Types/FCrowdyAppGroupPolicy.h"

// Channels are the same group model as teams (group_type = channel), so these responses
// parse the shared FCrowdyGroup / FCrowdyGroupMember / FCrowdyGroupRole / FCrowdyGroupMembership /
// FCrowdyAppGroupPolicy types. Only the GraphQL field name and EQueryResponseType differ from
// their team counterparts.

struct FMyChannelsResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroupMembership> Memberships;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::MyChannels; }
	virtual FName GetOperationName() const override { return "My Channels Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("myChannels"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroupMembership M;
				FCrowdyGroupMembership::ParseFromJson(*Obj, M);
				Memberships.Add(M);
			}
		}
		bIsValid = true;
	}
};

struct FChannelResponse : ICrowdyQueryResponse
{
	FCrowdyGroup Group;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::Channel; }
	virtual FName GetOperationName() const override { return "Channel Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("channel"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroup::ParseFromJson(*Obj, Group);
	}
};

struct FChannelsResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroup> Groups;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::Channels; }
	virtual FName GetOperationName() const override { return "Channels Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("channels"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroup G;
				FCrowdyGroup::ParseFromJson(*Obj, G);
				Groups.Add(G);
			}
		}
		bIsValid = true;
	}
};

struct FChannelMembersResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroupMember> Members;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::ChannelMembers; }
	virtual FName GetOperationName() const override { return "Channel Members Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("channelMembers"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroupMember M;
				FCrowdyGroupMember::ParseFromJson(*Obj, M);
				Members.Add(M);
			}
		}
		bIsValid = true;
	}
};

struct FChannelRolesResponse : ICrowdyQueryResponse
{
	TArray<FCrowdyGroupRole> Roles;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::ChannelRoles; }
	virtual FName GetOperationName() const override { return "Channel Roles Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (!(*DataObj)->TryGetArrayField(TEXT("channelRoles"), Arr)) { bIsValid = false; return; }

		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj;
			if (V->TryGetObject(Obj))
			{
				FCrowdyGroupRole R;
				FCrowdyGroupRole::ParseFromJson(*Obj, R);
				Roles.Add(R);
			}
		}
		bIsValid = true;
	}
};

struct FChannelPolicyResponse : ICrowdyQueryResponse
{
	FCrowdyAppGroupPolicy Policy;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::ChannelPolicy; }
	virtual FName GetOperationName() const override { return "Channel Policy Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("channelPolicy"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyAppGroupPolicy::ParseFromJson(*Obj, Policy);
	}
};

struct FCreateChannelResponse : ICrowdyQueryResponse
{
	FCrowdyGroup Group;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::CreateChannel; }
	virtual FName GetOperationName() const override { return "Create Channel Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("createChannel"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroup::ParseFromJson(*Obj, Group);
	}
};

struct FUpdateChannelResponse : ICrowdyQueryResponse
{
	FCrowdyGroup Group;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::UpdateChannel; }
	virtual FName GetOperationName() const override { return "Update Channel Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("updateChannel"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroup::ParseFromJson(*Obj, Group);
	}
};

struct FDeleteChannelResponse : ICrowdyQueryResponse
{
	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::DeleteChannel; }
	virtual FName GetOperationName() const override { return "Delete Channel Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }
		const TSharedPtr<FJsonObject>* DataObj;
		bIsValid = JsonObject->TryGetObjectField(TEXT("data"), DataObj);
	}
};

struct FJoinChannelResponse : ICrowdyQueryResponse
{
	FCrowdyGroupMember Member;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::JoinChannel; }
	virtual FName GetOperationName() const override { return "Join Channel Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("joinChannel"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupMember::ParseFromJson(*Obj, Member);
	}
};

struct FRequestToJoinChannelResponse : ICrowdyQueryResponse
{
	FCrowdyGroupMember Member;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::RequestToJoinChannel; }
	virtual FName GetOperationName() const override { return "Request To Join Channel Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("requestToJoinChannel"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupMember::ParseFromJson(*Obj, Member);
	}
};

struct FLeaveChannelResponse : ICrowdyQueryResponse
{
	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::LeaveChannel; }
	virtual FName GetOperationName() const override { return "Leave Channel Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }
		const TSharedPtr<FJsonObject>* DataObj;
		bIsValid = JsonObject->TryGetObjectField(TEXT("data"), DataObj);
	}
};

struct FAddChannelMemberResponse : ICrowdyQueryResponse
{
	FCrowdyGroupMember Member;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::AddChannelMember; }
	virtual FName GetOperationName() const override { return "Add Channel Member Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("addChannelMember"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupMember::ParseFromJson(*Obj, Member);
	}
};

struct FRemoveChannelMemberResponse : ICrowdyQueryResponse
{
	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::RemoveChannelMember; }
	virtual FName GetOperationName() const override { return "Remove Channel Member Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }
		const TSharedPtr<FJsonObject>* DataObj;
		bIsValid = JsonObject->TryGetObjectField(TEXT("data"), DataObj);
	}
};

struct FCreateChannelRoleResponse : ICrowdyQueryResponse
{
	FCrowdyGroupRole Role;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::CreateChannelRole; }
	virtual FName GetOperationName() const override { return "Create Channel Role Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("createChannelRole"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupRole::ParseFromJson(*Obj, Role);
	}
};

struct FUpdateChannelRoleResponse : ICrowdyQueryResponse
{
	FCrowdyGroupRole Role;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::UpdateChannelRole; }
	virtual FName GetOperationName() const override { return "Update Channel Role Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("updateChannelRole"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupRole::ParseFromJson(*Obj, Role);
	}
};

struct FDeleteChannelRoleResponse : ICrowdyQueryResponse
{
	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::DeleteChannelRole; }
	virtual FName GetOperationName() const override { return "Delete Channel Role Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }
		const TSharedPtr<FJsonObject>* DataObj;
		bIsValid = JsonObject->TryGetObjectField(TEXT("data"), DataObj);
	}
};

struct FSetChannelMemberRolesResponse : ICrowdyQueryResponse
{
	FCrowdyGroupMember Member;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::SetChannelMemberRoles; }
	virtual FName GetOperationName() const override { return "Set Channel Member Roles Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("setChannelMemberRoles"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyGroupMember::ParseFromJson(*Obj, Member);
	}
};

struct FSetChannelPolicyResponse : ICrowdyQueryResponse
{
	FCrowdyAppGroupPolicy Policy;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::SetChannelPolicy; }
	virtual FName GetOperationName() const override { return "Set Channel Policy Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid()) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { bIsValid = false; return; }

		const TSharedPtr<FJsonObject>* Obj;
		if (!(*DataObj)->TryGetObjectField(TEXT("setChannelPolicy"), Obj)) { bIsValid = false; return; }

		bIsValid = FCrowdyAppGroupPolicy::ParseFromJson(*Obj, Policy);
	}
};
