// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CLIProcessComponent.h"
#include "Components/ActorComponent.h"
#include "NodeFrameCodec.h"
#include "NodeComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeSciptBeginSignature, int32, ProcessId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeConsoleLogSignature, FString, LogMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeScriptPathSignature, FString, ScriptRelativePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNodeScriptErrorSignature, FString, ScriptRelativePath, FString, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpmInstallResultSignature, bool, bIsInstalled, FString, ErrorMessage);

// Emitted when a script emits an event back to Unreal. JsonArgs is the JSON-encoded
// args array; Binary carries the first interweaved binary buffer (empty if none).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FNodeEventSignature, const FString&, EventName, const FString&, JsonArgs, const TArray<uint8>&, Binary);

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
	bool bDetectErrorsInPipe = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bSyncCLIParams = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bScriptLogsOnGamethread = true;

	//if false, you need to call StartScript directly
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bStartDefaultScriptOnBeginPlay = true;
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

	//if true this will be included as a module (require), otherwise it will run in a separate child process
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bInlineLaunchScript = true;
};


UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class NODEJS_API UNodeComponent : public UCLIProcessComponent
{
	GENERATED_BODY()

public:	

	//Whenever your script emits an event it will emit here. EventName is the event,
	//JsonArgs is the JSON-encoded args array, Binary is the first interweaved buffer (if any).
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeEventSignature OnEvent;

	//Any console.log message will be sent here (process.js logs are filtered out)
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeConsoleLogSignature OnConsoleLog;

	//Logs from the main process
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeConsoleLogSignature OnProcessScriptLog;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptPathSignature OnScriptBegin;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptPathSignature OnScriptEnd;

	//Called after a script has unloaded, but before it begins allowing Unreal side cleanup if needed
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptPathSignature OnScriptReloaded;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptErrorSignature OnScriptError;

	UPROPERTY(BlueprintAssignable, Category = "Npm Events")
	FNpmInstallResultSignature OnNpmDependenciesResolved;

	//CustoSmize these for your script
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Parameters")
	FNodeJsScriptParams DefaultScriptParams;

	//Core process parameters for establishing the process bridge. Generally you don't need to change these params.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Parameters")
	FNodeJsProcessParams NodeJsProcessParams;

	//Specify if you'd like the script to be watched in params in this call.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	bool StartScript(const FNodeJsScriptParams& ScriptParams);

	//You can cancel a long running script here without stopping the full process
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	bool StopScript(const FNodeJsScriptParams& ScriptParams);

	//Emit an event to your script. JsonArgs is a single JSON value (object/array/number/etc)
	//that becomes the first argument of the script's ipc.on(EventName, (arg) => {...}).
	//Leave ScriptName empty to target the component's default script.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void EmitEvent(const FString& EventName, const FString& JsonArgs, const FString& ScriptName = TEXT(""));

	//As EmitEvent, but interweaves a binary buffer delivered to the script as a trailing Buffer arg.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void EmitEventWithBinary(const FString& EventName, const FString& JsonArgs, const TArray<uint8>& Binary, const FString& ScriptName = TEXT(""));

	//C++ convenience overload taking a structured json object as the single arg.
	void EmitEvent(const FString& EventName, const TSharedRef<class FJsonObject>& JsonArg, const FString& ScriptName = TEXT(""));


	UNodeComponent();

	void SyncCLIParams();

protected:

	UFUNCTION()
	void BeginProcessingExtraHandler(const FString& StartUpState);

	//Decodes the framed byte stream coming back from process.js (runs on bg thread).
	FNodeFrameCodec Decoder;

	//Routes a single decoded frame to the relevant delegates.
	void HandleFrame(uint8 Type, const FString& Header, const TArray<uint8>& Binary);

	//Frame helpers towards process.js.
	void SendControl(const FString& CommandLine);
	void SendEventFrame(const FString& EventName, const FString& JsonArgs, const TArray<TArray<uint8>>& Buffers, const FString& ScriptName);

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
