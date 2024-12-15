// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CLIProcessComponent.h"
#include "Components/ActorComponent.h"
#include "SocketIOClientComponent.h"
#include "NodeComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeSciptBeginSignature, int32, ProcessId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeConsoleLogSignature, FString, LogMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeScriptPathSignature, FString, ScriptRelativePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNodeScriptErrorSignature, FString, ScriptRelativePath, FString, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpmInstallResultSignature, bool, bIsInstalled, FString, ErrorMessage);

USTRUCT(BlueprintType)
struct FNodeJsProcessParams
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessPath = TEXT("Plugins/NodeJs-Unreal/Source/ThirdParty/node/");

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessName = TEXT("node");

	//main entry point script that wraps the IPC bridge for communication handling
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessScriptName = TEXT("process.js");

	//This is relative to the process path
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessScriptPath = TEXT("../../../Content/Scripts/");

	//Combine with ScriptPathRoot to have correct run path
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessToProjectRoot= TEXT("../../../../../");

	//NB: not yet implemented, so we don't expose it
	//UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bShareMainProcess = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bProcessInBytes = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bSyncCLIParams = true;
};

USTRUCT(BlueprintType)
struct FNodeJsScriptParams
{
	GENERATED_USTRUCT_BODY()

	//Relative to Project Root
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ScriptPathRoot = TEXT("Content/Scripts/");

	//The script you want to run for this component
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString Script = TEXT("script.js");

	//set to true if you want to dev
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bWatchFileForChanges = false;
};


UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class NODEJS_API UNodeComponent : public UCLIProcessComponent
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

	//Customize these for your script
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Parameters")
	FNodeJsScriptParams ScriptParams;

	//Core process parameters for establishing the process bridge
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Parameters")
	FNodeJsProcessParams NodeJsProcessParams;

	UNodeComponent();

	void SyncCLIParams();

protected:

	//UCLIProcessComponent overrides
	virtual void StartProcess() override;

	//UActorComponent overrides
	virtual void InitializeComponent() override;
	virtual void UninitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
};
