// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"

namespace
{
	// BigInt travels as a decimal string on the wire, never a JSON number.
	void SetBigIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int64 Value)
	{
		Object->SetStringField(Field, FString::Printf(TEXT("%lld"), Value));
	}

	// Optional string inputs are dropped when empty so an upsert leaves the server's existing value alone instead of
	// blanking it.
	void SetOptionalStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Object->SetStringField(Field, Value);
		}
	}

	// A field the sync fully owns: send its value, else an explicit JSON null to clear it, so the server read-back
	// equals the authored value and a re-sync is a true no-op.
	void SetOwnedStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			Object->SetField(Field, MakeShared<FJsonValueNull>());
		}
		else
		{
			Object->SetStringField(Field, Value);
		}
	}
}

namespace CrowdyGameModelMarshalling
{
	TSharedPtr<FJsonObject> BuildFunctionUpsertInput(const FCrowdyGameModelFunctionInput& Fn, int64 AppId)
	{
		const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		SetBigIntField(Input, TEXT("appId"), AppId);
		Input->SetStringField(TEXT("name"), Fn.Name);
		SetOptionalStringField(Input, TEXT("containerTypeName"), Fn.ContainerTypeName);
		SetOptionalStringField(Input, TEXT("invokeScope"), Fn.InvokeScope);
		Input->SetBoolField(TEXT("autonomousInvocable"), Fn.bAutonomousInvocable);
		SetOwnedStringField(Input, TEXT("description"), Fn.Description);
		SetOwnedStringField(Input, TEXT("returnType"), Fn.ReturnType);
		SetOwnedStringField(Input, TEXT("returnExpression"), Fn.ReturnExpression);

		// An empty policy sends an explicit JSON null so the server clears any prior policy. This is a live path, not
		// a defensive one: a helper reachable only from other effects has no caller to gate, so it deliberately
		// lowers no policy, and switching an existing function to that scope has to clear the policy it used to have.
		if (Fn.InvokePolicyJson.IsEmpty())
		{
			Input->SetField(TEXT("invokePolicyJson"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Input->SetStringField(TEXT("invokePolicyJson"), Fn.InvokePolicyJson);
		}

		TArray<TSharedPtr<FJsonValue>> Params;
		Params.Reserve(Fn.Parameters.Num());
		for (const FCrowdyGameModelFunctionParam& P : Fn.Parameters)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), P.Name);
			Object->SetStringField(TEXT("valueType"), P.ValueType);
			Object->SetBoolField(TEXT("required"), P.bRequired);
			if (!P.DefaultValueJson.IsEmpty())
			{
				Object->SetStringField(TEXT("defaultValueJson"), P.DefaultValueJson);
			}
			if (!P.Description.IsEmpty())
			{
				Object->SetStringField(TEXT("description"), P.Description);
			}
			Object->SetNumberField(TEXT("sortOrder"), P.SortOrder);
			Params.Add(MakeShared<FJsonValueObject>(Object));
		}
		Input->SetArrayField(TEXT("parameters"), Params);

		TArray<TSharedPtr<FJsonValue>> Mutations;
		Mutations.Reserve(Fn.Mutations.Num());
		for (const FCrowdyGameModelMutation& M : Fn.Mutations)
		{
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("target"), M.Target);
			Object->SetStringField(TEXT("property"), M.Property);
			Object->SetStringField(TEXT("expression"), M.Expression);
			Mutations.Add(MakeShared<FJsonValueObject>(Object));
		}
		Input->SetArrayField(TEXT("mutations"), Mutations);

		// Re-emit the preserved notifications so an upsert never wipes seed / console-authored notifications. Omitted
		// when there are none (a create, or a function with no server notifications) - the correct no-op.
		if (Fn.Notifications.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Notifications;
			Notifications.Reserve(Fn.Notifications.Num());
			for (const FCrowdyGameModelNotification& N : Fn.Notifications)
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("kind"), N.Kind);
				if (!N.EmitAs.IsEmpty())
				{
					Object->SetStringField(TEXT("emitAs"), N.EmitAs);
				}
				TArray<TSharedPtr<FJsonValue>> Args;
				Args.Reserve(N.Args.Num());
				for (const FCrowdyGameModelNotificationArg& A : N.Args)
				{
					const TSharedPtr<FJsonObject> ArgObject = MakeShared<FJsonObject>();
					ArgObject->SetStringField(TEXT("name"), A.Name);
					ArgObject->SetStringField(TEXT("expression"), A.Expression);
					Args.Add(MakeShared<FJsonValueObject>(ArgObject));
				}
				Object->SetArrayField(TEXT("args"), Args);
				Notifications.Add(MakeShared<FJsonValueObject>(Object));
			}
			Input->SetArrayField(TEXT("notifications"), Notifications);
		}

		// The effect is the sole author of its timers, so unlike notifications there is nothing server-side to
		// preserve. Still omitted when empty, matching the notifications convention: a function that declares none
		// leaves the key off entirely rather than sending an empty array.
		if (Fn.Timers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Timers;
			Timers.Reserve(Fn.Timers.Num());
			for (const FCrowdyGameModelTimer& T : Fn.Timers)
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("functionName"), T.FunctionName);
				Object->SetStringField(TEXT("delayMsExpression"), T.DelayMsExpression);
				SetOptionalStringField(Object, TEXT("target"), T.Target);
				SetOptionalStringField(Object, TEXT("dedupeKeyExpression"), T.DedupeKeyExpression);
				if (T.Params.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> TimerParams;
					TimerParams.Reserve(T.Params.Num());
					for (const FCrowdyGameModelTimerParam& P : T.Params)
					{
						const TSharedPtr<FJsonObject> ParamObject = MakeShared<FJsonObject>();
						ParamObject->SetStringField(TEXT("name"), P.Name);
						ParamObject->SetStringField(TEXT("expression"), P.Expression);
						TimerParams.Add(MakeShared<FJsonValueObject>(ParamObject));
					}
					Object->SetArrayField(TEXT("params"), TimerParams);
				}
				Timers.Add(MakeShared<FJsonValueObject>(Object));
			}
			Input->SetArrayField(TEXT("timers"), Timers);
		}

		return Input;
	}

	TSharedPtr<FJsonObject> BuildAutomationUpsertInput(const FCrowdyGameModelAutomationInput& Automation, int64 AppId)
	{
		const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		SetBigIntField(Input, TEXT("appId"), AppId);
		Input->SetStringField(TEXT("name"), Automation.Name);
		SetOwnedStringField(Input, TEXT("description"), Automation.Description);
		Input->SetBoolField(TEXT("enabled"), Automation.bEnabled);
		Input->SetStringField(TEXT("actionKind"), Automation.ActionKind);
		Input->SetStringField(TEXT("functionName"), Automation.FunctionName);
		Input->SetStringField(TEXT("targetMode"), Automation.TargetMode);
		SetOwnedStringField(Input, TEXT("selfContainerId"), Automation.SelfContainerId);
		SetOwnedStringField(Input, TEXT("targetTypeName"), Automation.TargetTypeName);
		SetOwnedStringField(Input, TEXT("sessionId"), Automation.SessionId);
		// paramsJson is non-null on the server (defaults to "{}"); emit that default when unauthored so the read-back
		// is a stable "{}" instead of null.
		Input->SetStringField(TEXT("paramsJson"), Automation.ParamsJson.IsEmpty() ? TEXT("{}") : Automation.ParamsJson);
		SetOwnedStringField(Input, TEXT("selectorJson"), Automation.SelectorJson);
		Input->SetStringField(TEXT("triggerType"), Automation.TriggerType);
		// Schedule fields apply only to a schedule trigger, and within that only the field the kind uses (intervalMs
		// for interval, cronExpr for cron). Emit the rest as null so an event/manual automation is stored
		// non-scheduled and a re-plan is a true no-op; the schema diff skips these fields the same way.
		if (Automation.TriggerType == TEXT("schedule"))
		{
			Input->SetStringField(TEXT("scheduleKind"), Automation.ScheduleKind);
			if (Automation.ScheduleKind == TEXT("cron"))
			{
				Input->SetField(TEXT("intervalMs"), MakeShared<FJsonValueNull>());
				SetOwnedStringField(Input, TEXT("cronExpr"), Automation.CronExpr);
			}
			else
			{
				Input->SetNumberField(TEXT("intervalMs"), Automation.IntervalMs);
				Input->SetField(TEXT("cronExpr"), MakeShared<FJsonValueNull>());
			}
		}
		else
		{
			Input->SetField(TEXT("scheduleKind"), MakeShared<FJsonValueNull>());
			Input->SetField(TEXT("intervalMs"), MakeShared<FJsonValueNull>());
			Input->SetField(TEXT("cronExpr"), MakeShared<FJsonValueNull>());
		}
		Input->SetNumberField(TEXT("maxTargets"), Automation.MaxTargets);
		Input->SetNumberField(TEXT("gasLimit"), Automation.GasLimit);
		Input->SetNumberField(TEXT("runTimeoutMs"), Automation.RunTimeoutMs);
		Input->SetNumberField(TEXT("maxRunsPerMinute"), Automation.MaxRunsPerMinute);
		Input->SetNumberField(TEXT("failureThreshold"), Automation.FailureThreshold);
		Input->SetNumberField(TEXT("cooldownMs"), Automation.CooldownMs);
		return Input;
	}

	TSharedPtr<FJsonObject> BuildAutomationTriggerUpsertInput(const FCrowdyGameModelAutomationTriggerInput& Trigger, int64 AppId)
	{
		const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		SetBigIntField(Input, TEXT("appId"), AppId);
		Input->SetStringField(TEXT("automationName"), Trigger.AutomationName);
		Input->SetStringField(TEXT("onEvent"), Trigger.OnEvent);
		SetOwnedStringField(Input, TEXT("functionName"), Trigger.FunctionName);
		SetOwnedStringField(Input, TEXT("containerTypeName"), Trigger.ContainerTypeName);
		SetOwnedStringField(Input, TEXT("propertyKey"), Trigger.PropertyKey);
		// Omitted rather than nulled when unset: the server rejects a filter the event cannot match, so a
		// non-property-changed trigger must not carry the field at all.
		SetOptionalStringField(Input, TEXT("writeSource"), Trigger.WriteSource);
		Input->SetNumberField(TEXT("debounceMs"), Trigger.DebounceMs);
		return Input;
	}
}
