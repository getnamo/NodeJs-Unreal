// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SocketIOClientComponent.h"
#include "NodeComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeSciptBeginSignature, int32, ProcessId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeConsoleLogSignature, FString, LogMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeScriptPathSignature, FString, ScriptRelativePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNodeScriptErrorSignature, FString, ScriptRelativePath, FString, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpmInstallResultSignature, bool, bIsInstalled, FString, ErrorMessage);

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class NODEJS_API UNodeComponent : public UActorComponent
{
	GENERATED_BODY()

public:	

	//Whenever your script emits an event that you've bound to it will emit here (unless bound to function)
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FSIOCEventJsonSignature OnEvent;

	//Any console.log message will be sent here
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeConsoleLogSignature OnConsoleLog;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeSciptBeginSignature OnScriptBegin;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptPathSignature OnScriptEnd;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptErrorSignature OnScriptError;

	UPROPERTY(BlueprintAssignable, Category = "Npm Events")
	FNpmInstallResultSignature OnNpmDependenciesResolved;

	UNodeComponent();

protected:

	virtual void BeginPlay() override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
};
