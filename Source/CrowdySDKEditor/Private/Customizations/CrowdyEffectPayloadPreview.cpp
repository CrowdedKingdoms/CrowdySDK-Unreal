// Fill out your copyright notice in the Description page of Project Settings.

#include "Customizations/CrowdyEffectPayloadPreview.h"

#include "Dom/JsonObject.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Serialization/JsonSerializer.h"

namespace CrowdyEffectPayloadPreview
{
	const TCHAR* const AppIdPlaceholder = TEXT("<appId>");

	namespace
	{
		// Pretty-print an upsert input object, with the real appId swapped for the placeholder so an author-time
		// preview never implies a concrete app.
		FString ToPreviewJson(const TSharedPtr<FJsonObject>& Input)
		{
			if (!Input.IsValid())
			{
				return FString();
			}
			Input->SetStringField(TEXT("appId"), AppIdPlaceholder);

			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Input.ToSharedRef(), Writer);
			return Out;
		}

		FString BuildWireJson(const FCrowdyEffectLoweringResult& Result)
		{
			// AppId 0 is a filler; ToPreviewJson overwrites it with the placeholder.
			FString Text = FString::Printf(
				TEXT("appId is filled in at deploy; %s is a placeholder.\n\n"), AppIdPlaceholder);

			Text += TEXT("// gameModelUpsertFunction input\n");
			Text += ToPreviewJson(CrowdyGameModelMarshalling::BuildFunctionUpsertInput(Result.Function, 0));

			if (Result.Automation.IsSet())
			{
				Text += TEXT("\n\n// gameModelUpsertAutomation input\n");
				Text += ToPreviewJson(CrowdyGameModelMarshalling::BuildAutomationUpsertInput(Result.Automation.GetValue(), 0));
			}
			if (Result.Trigger.IsSet())
			{
				Text += TEXT("\n\n// gameModelUpsertAutomationTrigger input\n");
				Text += ToPreviewJson(CrowdyGameModelMarshalling::BuildAutomationTriggerUpsertInput(Result.Trigger.GetValue(), 0));
			}
			return Text;
		}

		FString BuildSummary(const FCrowdyEffectLoweringResult& Result, const FString& EffectiveFunctionName)
		{
			const FCrowdyGameModelFunctionInput& Fn = Result.Function;

			FString Text = FString::Printf(TEXT("Function: %s"),
				Fn.Name.IsEmpty() ? *EffectiveFunctionName : *Fn.Name);
			Text += FString::Printf(TEXT("\n  Container type: %s"),
				Fn.ContainerTypeName.IsEmpty() ? TEXT("(any)") : *Fn.ContainerTypeName);
			if (!Fn.InvokeScope.IsEmpty())
			{
				Text += FString::Printf(TEXT("\n  Invoke scope: %s"), *Fn.InvokeScope);
			}
			// A return type is optional, so keying the line on it would hide the answer of every effect that returns
			// an expression no type can be read from, which is most of them.
			if (!Fn.ReturnType.IsEmpty() || !Fn.ReturnExpression.IsEmpty())
			{
				Text += FString::Printf(TEXT("\n  Returns %s%s"),
					Fn.ReturnType.IsEmpty() ? TEXT("(untyped)") : *Fn.ReturnType,
					Fn.ReturnExpression.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" = %s"), *Fn.ReturnExpression));
			}
			Text += FString::Printf(TEXT("\n  Requires a Source object: %s"), Result.bSourceReferenced ? TEXT("yes") : TEXT("no"));

			Text += TEXT("\n  Parameters:");
			if (Fn.Parameters.Num() == 0)
			{
				Text += TEXT(" (none)");
			}
			for (const FCrowdyGameModelFunctionParam& P : Fn.Parameters)
			{
				Text += FString::Printf(TEXT("\n    - %s: %s%s"), *P.Name, *P.ValueType,
					P.bRequired ? TEXT(", required") : TEXT(""));
				if (!P.DefaultValueJson.IsEmpty())
				{
					Text += FString::Printf(TEXT(", default %s"), *P.DefaultValueJson);
				}
			}

			Text += TEXT("\n  Writes:");
			if (Fn.Mutations.Num() == 0)
			{
				Text += TEXT(" (none)");
			}
			for (const FCrowdyGameModelMutation& M : Fn.Mutations)
			{
				Text += FString::Printf(TEXT("\n    - %s.%s = %s"), *M.Target, *M.Property, *M.Expression);
			}

			Text += FString::Printf(TEXT("\n  Invoke policy: %s"),
				Fn.InvokePolicyJson.IsEmpty() ? TEXT("(none)") : *Fn.InvokePolicyJson);

			if (Fn.Notifications.Num() > 0)
			{
				Text += TEXT("\n  Notifications:");
				for (const FCrowdyGameModelNotification& N : Fn.Notifications)
				{
					Text += FString::Printf(TEXT("\n    - %s%s"), *N.Kind,
						N.EmitAs.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" as %s"), *N.EmitAs));
					for (const FCrowdyGameModelNotificationArg& A : N.Args)
					{
						Text += FString::Printf(TEXT("\n        %s = %s"), *A.Name, *A.Expression);
					}
				}
			}

			if (Result.Automation.IsSet())
			{
				const FCrowdyGameModelAutomationInput& A = Result.Automation.GetValue();
				Text += FString::Printf(TEXT("\n\nAutomation: %s"), *A.Name);
				Text += FString::Printf(TEXT("\n  Enabled: %s"), A.bEnabled ? TEXT("yes") : TEXT("no"));
				Text += FString::Printf(TEXT("\n  Trigger: %s"), *A.TriggerType);
				if (A.TriggerType == TEXT("schedule"))
				{
					Text += (A.ScheduleKind == TEXT("cron"))
						? FString::Printf(TEXT(" (cron %s)"), *A.CronExpr)
						: FString::Printf(TEXT(" (every %d ms)"), A.IntervalMs);
				}
				Text += FString::Printf(TEXT("\n  Target: %s"), *A.TargetMode);
				if (A.TargetMode == TEXT("type") && !A.TargetTypeName.IsEmpty())
				{
					Text += FString::Printf(TEXT(" %s"), *A.TargetTypeName);
				}
				else if (A.TargetMode == TEXT("container") && !A.SelfContainerId.IsEmpty())
				{
					Text += FString::Printf(TEXT(" %s"), *A.SelfContainerId);
				}
			}

			if (Result.Trigger.IsSet())
			{
				const FCrowdyGameModelAutomationTriggerInput& T = Result.Trigger.GetValue();
				Text += FString::Printf(TEXT("\n\nEvent trigger: on %s"), *T.OnEvent);
				if (!T.PropertyKey.IsEmpty())
				{
					Text += FString::Printf(TEXT(" (property %s)"), *T.PropertyKey);
				}
				else if (!T.FunctionName.IsEmpty())
				{
					Text += FString::Printf(TEXT(" (function %s)"), *T.FunctionName);
				}
				// Spelled out rather than left implicit: the filter decides which container the watched event has to
				// happen on, and reading it back is how an author notices it is not the type they meant.
				Text += FString::Printf(TEXT("\n  On container type: %s"),
					T.ContainerTypeName.IsEmpty() ? TEXT("any") : *T.ContainerTypeName);
				if (!T.WriteSource.IsEmpty())
				{
					Text += FString::Printf(TEXT("\n  Observes writes from: %s"), *T.WriteSource);
				}
				if (T.DebounceMs > 0)
				{
					Text += FString::Printf(TEXT("\n  Debounce: %d ms"), T.DebounceMs);
				}
			}

			return Text;
		}

		FString BuildDiagnostics(const FCrowdyEffectLoweringResult& Result)
		{
			FString Text = TEXT("This effect has compile errors, so it has no deploy payload yet:");
			for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
			{
				Text += TEXT("\n  ") + Diagnostic.ToString();
			}
			return Text;
		}
	}

	FString BuildPayloadPreview(const FCrowdyEffectLoweringResult& Result, const FString& EffectiveFunctionName, EMode Mode)
	{
		if (Result.HasErrors())
		{
			return BuildDiagnostics(Result);
		}
		return Mode == EMode::WireJson ? BuildWireJson(Result) : BuildSummary(Result, EffectiveFunctionName);
	}
}
