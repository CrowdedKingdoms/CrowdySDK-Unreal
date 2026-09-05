// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyKitInventory.h"

#include "Dom/JsonObject.h"
#include "Replication/GameModel/Kit/CrowdyKitJson.h"
#include "Replication/GameModel/Kit/CrowdyKitPolicy.h"

namespace CrowdyKit
{
	namespace
	{
		TSharedPtr<FJsonObject> ContainerType(const FString& TypeName, const FString& Description)
		{
			TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("typeName"), TypeName);
			T->SetStringField(TEXT("displayName"), TypeName);
			T->SetStringField(TEXT("instantiableBy"), TEXT("member"));
			T->SetStringField(TEXT("description"), Description);
			return T;
		}

		TSharedPtr<FJsonObject> PropertyDef(const FString& Type, const FString& Key, const FString& ValueType,
			const TCHAR* DefaultJson)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("containerTypeName"), Type);
			P->SetStringField(TEXT("key"), Key);
			P->SetStringField(TEXT("valueType"), ValueType);
			if (DefaultJson != nullptr)
			{
				P->SetStringField(TEXT("defaultValueJson"), DefaultJson);
			}
			return P;
		}

		TSharedPtr<FJsonObject> Param(const FString& Name, const FString& ValueType)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), Name);
			P->SetStringField(TEXT("valueType"), ValueType);
			P->SetBoolField(TEXT("required"), true);
			return P;
		}

		TSharedPtr<FJsonObject> Mutation(const FString& Target, const FString& Property, const FString& Expression)
		{
			TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("target"), Target);
			M->SetStringField(TEXT("property"), Property);
			M->SetStringField(TEXT("expression"), Expression);
			return M;
		}
	}

	FCrowdyKitInventoryNames InventoryNames(const FString& TypePrefix)
	{
		const FString FnPrefix = TypePrefix.IsEmpty() ? FString() : ToSnakeCase(TypePrefix) + TEXT("_");
		FCrowdyKitInventoryNames Names;
		Names.InventoryType = TypePrefix + TEXT("Inventory");
		Names.StackType = TypePrefix + TEXT("ItemStack");
		Names.GrantFn = FnPrefix + TEXT("grant_stack");
		Names.ConsumeFn = FnPrefix + TEXT("consume_stack");
		Names.MoveFn = FnPrefix + TEXT("move_stack");
		Names.TransferFn = FnPrefix + TEXT("transfer_stack");
		Names.ContainsEdge = TEXT("inventory_contains");
		return Names;
	}

	FCrowdyKitBlueprint InventoryBlueprint(const FCrowdyInventoryBlueprintOptions& Options)
	{
		const FCrowdyKitInventoryNames Names = InventoryNames(Options.TypePrefix);
		const FString OwnerOnly = KitPolicyJson(OwnerOfSelfPolicy());

		FCrowdyKitBlueprint Bp;
		Bp.Name = Names.InventoryType;

		Bp.ContainerTypes.Add(ContainerType(Names.InventoryType, TEXT("A bag of item stacks owned by one player.")));
		Bp.ContainerTypes.Add(ContainerType(Names.StackType, TEXT("One stack of a single item type in an inventory slot.")));

		Bp.PropertyDefinitions.Add(PropertyDef(Names.InventoryType, TEXT("max_slots"), TEXT("int"),
			*FString::FromInt(Options.MaxSlots)));
		Bp.PropertyDefinitions.Add(PropertyDef(Names.StackType, TEXT("item_id"), TEXT("string"), nullptr));
		{
			TSharedPtr<FJsonObject> OwnerMirror = PropertyDef(Names.StackType, TEXT("owner_user_id"), TEXT("int"), TEXT("0"));
			OwnerMirror->SetStringField(TEXT("description"),
				TEXT("Mirror of the stack owner's user id (kit convention), read by cross-container guards such as the economy trade/market functions."));
			Bp.PropertyDefinitions.Add(OwnerMirror);
		}
		Bp.PropertyDefinitions.Add(PropertyDef(Names.StackType, TEXT("quantity"), TEXT("int"), TEXT("0")));
		Bp.PropertyDefinitions.Add(PropertyDef(Names.StackType, TEXT("slot"), TEXT("int"), TEXT("0")));

		{
			TSharedPtr<FJsonObject> Fn = MakeShared<FJsonObject>();
			Fn->SetStringField(TEXT("name"), Names.GrantFn);
			Fn->SetStringField(TEXT("containerTypeName"), Names.StackType);
			Fn->SetStringField(TEXT("returnType"), TEXT("int"));
			Fn->SetArrayField(TEXT("parameters"), CrowdyKitJson::ArrayOfObjects({Param(TEXT("amount"), TEXT("int"))})->AsArray());
			Fn->SetArrayField(TEXT("mutations"), CrowdyKitJson::ArrayOfObjects(
				{Mutation(TEXT("self"), TEXT("quantity"), TEXT("self.quantity + max(0, $amount)"))})->AsArray());
			Fn->SetStringField(TEXT("returnExpression"), TEXT("self.quantity"));
			Fn->SetStringField(TEXT("invokePolicyJson"), OwnerOnly);
			Fn->SetStringField(TEXT("description"), TEXT("Add items to a stack the caller owns."));
			Bp.Functions.Add(Fn);
		}
		{
			TSharedPtr<FJsonObject> Fn = MakeShared<FJsonObject>();
			Fn->SetStringField(TEXT("name"), Names.ConsumeFn);
			Fn->SetStringField(TEXT("containerTypeName"), Names.StackType);
			Fn->SetStringField(TEXT("returnType"), TEXT("int"));
			Fn->SetArrayField(TEXT("parameters"), CrowdyKitJson::ArrayOfObjects({Param(TEXT("amount"), TEXT("int"))})->AsArray());
			Fn->SetArrayField(TEXT("mutations"), CrowdyKitJson::ArrayOfObjects(
				{Mutation(TEXT("self"), TEXT("quantity"), TEXT("self.quantity - $amount"))})->AsArray());
			Fn->SetStringField(TEXT("returnExpression"), TEXT("self.quantity"));
			Fn->SetStringField(TEXT("invokePolicyJson"), KitPolicyJson(AndPolicy(
				{OwnerOfSelfPolicy(), ConditionPolicy(TEXT("$amount > 0 && self.quantity >= $amount"))})));
			Fn->SetStringField(TEXT("description"), TEXT("Spend items from a stack; refuses to overdraw."));
			Bp.Functions.Add(Fn);
		}
		{
			TSharedPtr<FJsonObject> Fn = MakeShared<FJsonObject>();
			Fn->SetStringField(TEXT("name"), Names.MoveFn);
			Fn->SetStringField(TEXT("containerTypeName"), Names.StackType);
			Fn->SetStringField(TEXT("returnType"), TEXT("int"));
			Fn->SetArrayField(TEXT("parameters"), CrowdyKitJson::ArrayOfObjects({Param(TEXT("to_slot"), TEXT("int"))})->AsArray());
			Fn->SetArrayField(TEXT("mutations"), CrowdyKitJson::ArrayOfObjects(
				{Mutation(TEXT("self"), TEXT("slot"),
					FString::Printf(TEXT("clamp($to_slot, 0, %d)"), Options.SlotCount - 1))})->AsArray());
			Fn->SetStringField(TEXT("returnExpression"), TEXT("self.slot"));
			Fn->SetStringField(TEXT("invokePolicyJson"), OwnerOnly);
			Fn->SetStringField(TEXT("description"), TEXT("Move a stack to another slot."));
			Bp.Functions.Add(Fn);
		}
		{
			TSharedPtr<FJsonObject> Fn = MakeShared<FJsonObject>();
			Fn->SetStringField(TEXT("name"), Names.TransferFn);
			Fn->SetStringField(TEXT("containerTypeName"), Names.StackType);
			Fn->SetStringField(TEXT("returnType"), TEXT("int"));
			Fn->SetArrayField(TEXT("parameters"), CrowdyKitJson::ArrayOfObjects(
				{Param(TEXT("to_id"), TEXT("container_ref")), Param(TEXT("amount"), TEXT("int"))})->AsArray());
			Fn->SetArrayField(TEXT("mutations"), CrowdyKitJson::ArrayOfObjects(
				{Mutation(TEXT("self"), TEXT("quantity"), TEXT("self.quantity - $amount")),
				 Mutation(TEXT("ref($to_id)"), TEXT("quantity"), TEXT("ref($to_id).quantity + $amount"))})->AsArray());
			Fn->SetStringField(TEXT("returnExpression"), TEXT("self.quantity"));
			Fn->SetStringField(TEXT("invokePolicyJson"), KitPolicyJson(AndPolicy(
				{OwnerOfSelfPolicy(),
				 ConditionPolicy(TEXT("$amount > 0 && self.quantity >= $amount && ref($to_id).item_id == self.item_id"))})));
			Fn->SetStringField(TEXT("description"), TEXT("Atomically move items between two stacks of the same item type."));
			Bp.Functions.Add(Fn);
		}

		return Bp;
	}
}
