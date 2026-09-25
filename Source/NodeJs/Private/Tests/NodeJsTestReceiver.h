// Copyright getnamo. NodeJs-Unreal v2.1.0

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "NodeComponent.h"
#include "NodeJs.h"
#include "NodeJsTestReceiver.generated.h"

//Automation test helper: target for BindEvent / BindEventToFunction / OnEvent. Not for gameplay use.
UCLASS(Transient, NotBlueprintable, HideDropdown)
class UNodeJsTestReceiver : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnJson(const FString& JsonArgs) { JsonCalls.Add(JsonArgs); }

	UFUNCTION()
	void OnJsonAndBinary(const FString& JsonArgs, const TArray<uint8>& Binary) { BinaryCalls.Add(Binary); }

	UFUNCTION()
	void OnData(const FNodeEventData& Event) { DataCalls.Add(Event); }

	UFUNCTION()
	void OnBadSignature(int32 Value) {}

	UFUNCTION()
	void OnAnyEvent(const FString& EventName, const FString& JsonArgs, const TArray<uint8>& Binary) { AnyEvents.Add(EventName); }

	//Surface node output in the test log.
	UFUNCTION()
	void OnLog(FString Message) { UE_LOG(LogNodeJs, Display, TEXT("[node] %s"), *Message); }

	UFUNCTION()
	void OnError(FString Script, FString Message) { UE_LOG(LogNodeJs, Display, TEXT("[node error] %s: %s"), *Script, *Message); }

	TArray<FString> JsonCalls;
	TArray<TArray<uint8>> BinaryCalls;
	TArray<FNodeEventData> DataCalls;
	TArray<FString> AnyEvents;
};
