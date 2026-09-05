// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/Kit/CrowdyKitJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace CrowdyKitJson
{
	namespace
	{
		void AppendEscapedString(FString& Out, const FString& Value)
		{
			Out.AppendChar(TEXT('"'));
			for (int32 Index = 0; Index < Value.Len(); ++Index)
			{
				const TCHAR C = Value[Index];
				switch (C)
				{
				case TEXT('"'): Out.Append(TEXT("\\\"")); break;
				case TEXT('\\'): Out.Append(TEXT("\\\\")); break;
				case TEXT('\b'): Out.Append(TEXT("\\b")); break;
				case TEXT('\f'): Out.Append(TEXT("\\f")); break;
				case TEXT('\n'): Out.Append(TEXT("\\n")); break;
				case TEXT('\r'): Out.Append(TEXT("\\r")); break;
				case TEXT('\t'): Out.Append(TEXT("\\t")); break;
				default:
					if (C < 0x20)
					{
						Out.Append(FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C)));
					}
					else
					{
						Out.AppendChar(C);
					}
					break;
				}
			}
			Out.AppendChar(TEXT('"'));
		}

		void AppendNumber(FString& Out, double Number)
		{
			// An integral value within the int64 range emits as an integer with no decimal point, matching the
			// sibling SDK's dump of a JSON integer.
			if (FMath::IsFinite(Number) && Number == FMath::TruncToDouble(Number)
				&& Number >= -9.2e18 && Number <= 9.2e18)
			{
				Out.Append(FString::Printf(TEXT("%lld"), static_cast<int64>(Number)));
				return;
			}

			// A non-integral value emits at 17 significant digits, matching the sibling SDK's double serialization
			// (printf %.17g) so the two SDKs produce byte-identical wire text for the same double.
			Out.Append(FString::Printf(TEXT("%.17g"), Number));
		}

		void AppendValue(FString& Out, const TSharedPtr<FJsonValue>& Value);

		void AppendObject(FString& Out, const TSharedPtr<FJsonObject>& Object)
		{
			Out.AppendChar(TEXT('{'));
			if (Object.IsValid())
			{
				TArray<TPair<FString, TSharedPtr<FJsonValue>>> Pairs;
				Pairs.Reserve(Object->Values.Num());
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					Pairs.Add(Pair);
				}
				// Case-sensitive byte-order sort, matching the sibling SDK's std::map key ordering.
				Pairs.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A,
					const TPair<FString, TSharedPtr<FJsonValue>>& B)
				{
					return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0;
				});

				bool bFirst = true;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Pairs)
				{
					if (!bFirst)
					{
						Out.AppendChar(TEXT(','));
					}
					bFirst = false;
					AppendEscapedString(Out, Pair.Key);
					Out.AppendChar(TEXT(':'));
					AppendValue(Out, Pair.Value);
				}
			}
			Out.AppendChar(TEXT('}'));
		}

		void AppendValue(FString& Out, const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid())
			{
				Out.Append(TEXT("null"));
				return;
			}

			switch (Value->Type)
			{
			case EJson::Null:
				Out.Append(TEXT("null"));
				break;
			case EJson::Boolean:
				Out.Append(Value->AsBool() ? TEXT("true") : TEXT("false"));
				break;
			case EJson::Number:
				AppendNumber(Out, Value->AsNumber());
				break;
			case EJson::String:
				AppendEscapedString(Out, Value->AsString());
				break;
			case EJson::Array:
			{
				Out.AppendChar(TEXT('['));
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					if (Index > 0)
					{
						Out.AppendChar(TEXT(','));
					}
					AppendValue(Out, Items[Index]);
				}
				Out.AppendChar(TEXT(']'));
				break;
			}
			case EJson::Object:
				AppendObject(Out, Value->AsObject());
				break;
			default:
				Out.Append(TEXT("null"));
				break;
			}
		}
	}

	FString Canonical(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		AppendObject(Out, Object);
		return Out;
	}

	FString Canonical(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		AppendValue(Out, Value);
		return Out;
	}

	TSharedPtr<FJsonValue> ObjectValue(const TSharedPtr<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonValue> ArrayOfObjects(const TArray<TSharedPtr<FJsonObject>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Objects.Num());
		for (const TSharedPtr<FJsonObject>& Object : Objects)
		{
			Values.Add(MakeShared<FJsonValueObject>(Object));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Values));
	}
}
