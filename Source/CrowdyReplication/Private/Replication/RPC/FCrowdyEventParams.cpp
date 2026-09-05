#include "Replication/RPC/FCrowdyEventParams.h"

#include "Replication/RPC/CrowdyRPC.h"

const FProperty* FCrowdyEventParams::FindParam(const FName ParamName, const void*& OutValue) const
{
	OutValue = nullptr;

	if (!Function || !Frame || ParamName.IsNone())
	{
		return nullptr;
	}

	FProperty* Property = Function->FindPropertyByName(ParamName);
	if (!Property)
	{
		return nullptr;
	}

	// A UFunction's field list holds its local variables as well as its parameters, and only the
	// parameters were ever serialized. IsTrueOutputParam is the same predicate the serializer and the
	// signature validator use, so what is readable here is exactly what the sender put on the wire.
	if (!Property->HasAnyPropertyFlags(CPF_Parm) || FCrowdyRPC::IsTrueOutputParam(Property))
	{
		return nullptr;
	}

	OutValue = Property->ContainerPtrToValuePtr<void>(Frame);
	return Property;
}

bool FCrowdyEventParams::GetBool(const FName ParamName, bool& OutValue) const
{
	const void* Value = nullptr;
	const FBoolProperty* Property = FindTypedParam<FBoolProperty>(ParamName, Value);
	if (!Property)
	{
		return false;
	}

	// Through the property rather than by dereferencing the pointer: a bool parameter can be a bitfield,
	// where the value is one bit of a byte shared with its neighbours.
	OutValue = Property->GetPropertyValue(Value);
	return true;
}

bool FCrowdyEventParams::GetInt32(const FName ParamName, int32& OutValue) const
{
	const void* Value = nullptr;
	const FIntProperty* Property = FindTypedParam<FIntProperty>(ParamName, Value);
	if (!Property)
	{
		return false;
	}

	OutValue = Property->GetPropertyValue(Value);
	return true;
}

bool FCrowdyEventParams::GetInt64(const FName ParamName, int64& OutValue) const
{
	const void* Value = nullptr;
	if (const FInt64Property* Wide = FindTypedParam<FInt64Property>(ParamName, Value))
	{
		OutValue = Wide->GetPropertyValue(Value);
		return true;
	}

	if (const FIntProperty* Narrow = FindTypedParam<FIntProperty>(ParamName, Value))
	{
		OutValue = Narrow->GetPropertyValue(Value);
		return true;
	}

	return false;
}

bool FCrowdyEventParams::GetFloat(const FName ParamName, float& OutValue) const
{
	double AsDouble = 0.0;
	if (!GetDouble(ParamName, AsDouble))
	{
		return false;
	}

	OutValue = static_cast<float>(AsDouble);
	return true;
}

bool FCrowdyEventParams::GetDouble(const FName ParamName, double& OutValue) const
{
	const void* Value = nullptr;
	if (const FDoubleProperty* Wide = FindTypedParam<FDoubleProperty>(ParamName, Value))
	{
		OutValue = Wide->GetPropertyValue(Value);
		return true;
	}

	if (const FFloatProperty* Narrow = FindTypedParam<FFloatProperty>(ParamName, Value))
	{
		OutValue = Narrow->GetPropertyValue(Value);
		return true;
	}

	return false;
}

bool FCrowdyEventParams::GetName(const FName ParamName, FName& OutValue) const
{
	const void* Value = nullptr;
	const FNameProperty* Property = FindTypedParam<FNameProperty>(ParamName, Value);
	if (!Property)
	{
		return false;
	}

	OutValue = Property->GetPropertyValue(Value);
	return true;
}

bool FCrowdyEventParams::GetString(const FName ParamName, FString& OutValue) const
{
	const void* Value = nullptr;
	const FStrProperty* Property = FindTypedParam<FStrProperty>(ParamName, Value);
	if (!Property)
	{
		return false;
	}

	OutValue = Property->GetPropertyValue(Value);
	return true;
}
