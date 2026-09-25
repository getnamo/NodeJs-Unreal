// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CLIProcessComponent.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "NodeFrameCodec.h"
#include "NodeComponent.generated.h"

class FJsonObject;
class UNodeJsSubsystem;

UENUM(BlueprintType)
enum class ENodeScriptLaunchMode : uint8
{
	//Loaded into the node process. Lowest latency. Stop/reload clears the script's timers and
	//listeners and fires 'shouldExit' (close sockets/servers there).
	Inline,
	//Runs on a worker thread inside the node process. Stop/reload terminates it outright.
	Worker,
	//Runs in its own forked node process. Stop fires 'shouldExit', then disconnects, then kills.
	Subprocess
};

//A single binary buffer; wrapped so arrays of buffers can cross into Blueprint.
USTRUCT(BlueprintType)
struct NODEJS_API FNodeBuffer
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs")
	TArray<uint8> Data;

	FNodeBuffer() {}
	FNodeBuffer(const TArray<uint8>& InData) : Data(InData) {}
	FNodeBuffer(TArray<uint8>&& InData) : Data(MoveTemp(InData)) {}
};

//An event (or callback reply) from a script.
USTRUCT(BlueprintType)
struct NODEJS_API FNodeEventData
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "NodeJs")
	FString EventName;

	UPROPERTY(BlueprintReadOnly, Category = "NodeJs")
	FString ScriptName;

	//The args as a JSON array string. Buffer args appear as {"_bin":i} placeholders indexing Buffers.
	UPROPERTY(BlueprintReadOnly, Category = "NodeJs")
	FString JsonArgs;

	UPROPERTY(BlueprintReadOnly, Category = "NodeJs")
	TArray<FNodeBuffer> Buffers;

	//C++ convenience: the parsed args array.
	TArray<TSharedPtr<FJsonValue>> Args;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeSciptBeginSignature, int32, ProcessId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeConsoleLogSignature, FString, LogMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeScriptPathSignature, FString, ScriptRelativePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNodeScriptErrorSignature, FString, ScriptRelativePath, FString, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpmInstallResultSignature, bool, bIsInstalled, FString, ErrorMessage);

// Emitted when a script emits an event back to Unreal. JsonArgs is the JSON-encoded
// args array; Binary carries the first interweaved binary buffer (empty if none).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FNodeEventSignature, const FString&, EventName, const FString&, JsonArgs, const TArray<uint8>&, Binary);

// As above, with every buffer and the script name.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeEventDataSignature, const FNodeEventData&, Event);

// Single-cast handler for BindEvent and EmitEventWithCallback.
DECLARE_DYNAMIC_DELEGATE_OneParam(FNodeEventCallback, const FNodeEventData&, Event);

// C++ handler for BindEventNative / EmitEventNative callbacks.
using FNodeNativeEventHandler = TFunction<void(const FNodeEventData&)>;

USTRUCT(BlueprintType)
struct NODEJS_API FNodeJsProcessParams
{
	GENERATED_USTRUCT_BODY()

	//Relative to the project root. If node isn't found there, the NodeJs plugin's own
	//Source/ThirdParty/node/ is used, wherever the plugin is installed.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessPath = TEXT("Plugins/NodeJs-Unreal/Source/ThirdParty/node/");

	//As ProcessPath, used on Linux (node's own layout: the binary lives in bin/).
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessPathLinux = TEXT("Plugins/NodeJs-Unreal/Source/ThirdParty/node-linux/bin/");

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessName = TEXT("node");

	//main entry point script that wraps the IPC bridge for communication handling
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessScriptName = TEXT("process.js");

	//This is relative to the process path. If process.js isn't found there, the plugin's own Content/Scripts is used.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessScriptPath = TEXT("../../../Content/Scripts/");

	//Unused since v2.1: the project root is sent to process.js as an absolute path.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ProcessToProjectRoot= TEXT("../../../../../");

	//Share one node process between every NodeComponent in the game instance that enables this
	//(instead of one process per component). Scripts stay isolated per component.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bShareMainProcess = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bProcessInBytes = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bDetectErrorsInPipe = false;

	//Dump script errors to the Output Log (in addition to the OnScriptError event) so they're
	//visible without having to wire the event up. Uses the LogNodeJs category.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bLogScriptErrorsToOutput = true;

	//If a script fails on a missing npm module that IS listed in your Scripts package.json,
	//auto-run npm install in that folder and reload the script.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bAutoResolveNpmDependencies = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bSyncCLIParams = true;

	//If false, console logs are broadcast on the pipe thread (own process only). Keep true for Blueprint use.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bScriptLogsOnGamethread = true;

	//if false, you need to call StartScript directly
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bStartDefaultScriptOnBeginPlay = true;

	//On EndPlay, how long to wait for scripts to run their 'shouldExit' cleanup before node is terminated.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	float ShutdownGraceSeconds = 0.3f;

	//How long the pipe reader sleeps when node has no new output (CLISystem IdleReadSleepSeconds).
	//0 polls continuously (lowest latency, but a busy CPU core per node process).
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	float PipeIdleSleepSeconds = 0.001f;
};

USTRUCT(BlueprintType)
struct NODEJS_API FNodeJsScriptParams
{
	GENERATED_USTRUCT_BODY()

	//Relative to Project Root. Falls back to the NodeJs plugin's own folder if the script isn't found.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString ScriptPathRoot = TEXT("Content/Scripts/");

	//The script you want to run for this component
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	FString Script = TEXT("script.js");

	//set to true if you want to dev
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bWatchFileForChanges = false;

	//When watching: reload the script on save. If false, saves only fire OnScriptChanged.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	bool bReloadOnChange = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	ENodeScriptLaunchMode LaunchMode = ENodeScriptLaunchMode::Inline;

	//Passed to the script as process.argv (after the script path) and as ipc.args.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "NodeJs Params")
	TArray<FString> Args;

	//Legacy (v2.0): false forces Subprocess regardless of LaunchMode. Prefer LaunchMode.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, AdvancedDisplay, Category = "NodeJs Params")
	bool bInlineLaunchScript = true;

	ENodeScriptLaunchMode GetEffectiveLaunchMode() const
	{
		return bInlineLaunchScript ? LaunchMode : ENodeScriptLaunchMode::Subprocess;
	}
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

	//Same events as OnEvent, with every interweaved buffer and the script name.
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeEventDataSignature OnEventWithBuffers;

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

	//A watched script was saved (fires whether or not it reloads, see bReloadOnChange)
	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptPathSignature OnScriptChanged;

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

	//~ Script control

	//Starts a script. Starting a script that is already running restarts it.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	bool StartScript(const FNodeJsScriptParams& ScriptParams);

	//You can cancel a long running script here without stopping the full process
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	bool StopScript(const FNodeJsScriptParams& ScriptParams);

	//Watch a running script for saves. Uses ScriptParams.bReloadOnChange.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void WatchScript(const FNodeJsScriptParams& ScriptParams);

	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void UnwatchScript(const FNodeJsScriptParams& ScriptParams);

	//Leave ScriptName empty to check the component's default script.
	UFUNCTION(BlueprintPure, Category = "NodeJs Functions")
	bool IsScriptRunning(const FString& ScriptName = TEXT("")) const;

	UFUNCTION(BlueprintPure, Category = "NodeJs Functions")
	TArray<FString> GetRunningScripts() const;

	//Full path the script resolves to (project first, then the plugin's own folder).
	UFUNCTION(BlueprintPure, Category = "NodeJs Functions")
	FString GetScriptFullPath(const FNodeJsScriptParams& ScriptParams) const;

	//~ Npm

	//Runs npm install for the package.json nearest to this script. Result arrives on OnNpmDependenciesResolved.
	UFUNCTION(BlueprintCallable, Category = "Npm Functions")
	void ResolveNpmDependencies(const FNodeJsScriptParams& ScriptParams);

	//Dependency names listed in the package.json nearest to this script.
	UFUNCTION(BlueprintCallable, Category = "Npm Functions")
	TArray<FString> PackageDependencies(const FNodeJsScriptParams& ScriptParams) const;

	//~ Emit

	//Emit an event to your script. JsonArgs is a single JSON value (object/array/number/etc)
	//that becomes the first argument of the script's ipc.on(EventName, (arg) => {...}).
	//Leave ScriptName empty to target the component's default script.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void EmitEvent(const FString& EventName, const FString& JsonArgs, const FString& ScriptName = TEXT(""));

	//As EmitEvent, but interweaves a binary buffer delivered to the script as a trailing Buffer arg.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void EmitEventWithBinary(const FString& EventName, const FString& JsonArgs, const TArray<uint8>& Binary, const FString& ScriptName = TEXT(""));

	//As EmitEvent with several buffers. They arrive as trailing Buffer args, unless JsonArgs
	//places them itself with {"_bin":i} placeholders (e.g. {"image":{"_bin":0}}).
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void EmitEventWithBuffers(const FString& EventName, const FString& JsonArgs, const TArray<FNodeBuffer>& Buffers, const FString& ScriptName = TEXT(""));

	//As EmitEvent, and the script's handler gets a trailing reply function:
	//ipc.on('ask', (arg, reply) => reply(result)). The reply arrives on Callback.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void EmitEventWithCallback(const FString& EventName, const FString& JsonArgs, FNodeEventCallback Callback, const FString& ScriptName = TEXT(""));

	//C++ convenience overload taking a structured json object as the single arg.
	void EmitEvent(const FString& EventName, const TSharedRef<FJsonObject>& JsonArg, const FString& ScriptName = TEXT(""));

	//C++: full control over args and buffers ({"_bin":i} placeholders in Args index Buffers), optional reply callback.
	void EmitEventNative(const FString& EventName, const TArray<TSharedPtr<FJsonValue>>& Args, const TArray<TArray<uint8>>& Buffers = TArray<TArray<uint8>>(),
		const FString& ScriptName = TEXT(""), FNodeNativeEventHandler Callback = nullptr);

	//~ Binding (bound handlers fire in addition to OnEvent)

	//Route a named script event to a Blueprint event/function.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void BindEvent(const FString& EventName, FNodeEventCallback Callback);

	//Route a named script event to a function on Target by name. The function takes either
	//(FString JsonArgs), (FString JsonArgs, TArray<uint8> Binary) or (FNodeEventData Event).
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	bool BindEventToFunction(const FString& EventName, const FString& FunctionName, UObject* Target);

	//Removes every handler bound to EventName.
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void UnbindEvent(const FString& EventName);

	void BindEventNative(const FString& EventName, FNodeNativeEventHandler Handler);

	UNodeComponent();

	void SyncCLIParams();

	//Resolves FNodeJsProcessParams into CLISystem process params (shared with UNodeJsSubsystem).
	static void ApplyProcessParams(const FNodeJsProcessParams& NodeParams, FProcessParams& OutParams);

	//CLISystem's pipe reader thread may still touch a stopped handler briefly; keep it alive a moment.
	static void RetireProcessHandler(TSharedPtr<FSubProcessHandler> Handler);

	//The 'exit' control frame, telling process.js how long scripts get for 'shouldExit'.
	static TArray<uint8> MakeExitFrame(float GraceSeconds);

	//Unique id used to route frames when the node process is shared.
	const FString& GetOwnerId() const { return OwnerId; }

	//Routes one decoded frame from process.js to this component's delegates. Game thread only.
	void HandleFrame(uint8 Type, const TSharedPtr<FJsonObject>& Header, const FString& RawHeader, const TArray<uint8>& Binary);

	//Called by UNodeJsSubsystem when the shared process comes up / goes down.
	void OnSharedProcessBegin();
	void OnSharedProcessEnd();

	//UCLIProcessComponent overrides
	virtual void StartProcess() override;
	virtual void StopProcess() override;

protected:

	UFUNCTION()
	void BeginProcessingExtraHandler(const FString& StartUpState);

	UFUNCTION()
	void EndProcessingExtraHandler(const FString& EndState);

	//Frame helpers towards process.js.
	void SendFrame(const TArray<uint8>& Frame);
	void SendControl(const TSharedRef<FJsonObject>& Command);
	TSharedRef<FJsonObject> MakeCommand(const FString& Cmd, const FString& ScriptName = TEXT("")) const;
	void SendEventFrame(const FString& EventName, const TArray<TSharedPtr<FJsonValue>>& Args, const TArray<TArray<uint8>>& Buffers, const FString& ScriptName, int32 AckId);
	TArray<TSharedPtr<FJsonValue>> BuildArgs(const FString& JsonArgs, int32 NumBuffers) const;
	int32 AddPendingCallback(const FString& ScriptName, FNodeEventCallback Callback, FNodeNativeEventHandler Native);

	//Own process up: point node at the project, start the default script, flush queued frames.
	void OnOwnProcessReady();
	void MaybeStartDefaultScript();
	void FlushPendingFrames();

	//Ask process.js to stop its scripts and wait (bounded) for the ack.
	void GracefulExit();

	//Fresh decoder + handler callbacks for the next own process (bumps ProcessGeneration).
	void InstallProcessHandler();
	void ResetScriptState();
	void EnsureOwnerId();
	bool IsUsingSharedProcess() const;
	FString TargetScript(const FString& ScriptName) const;
	void DispatchBoundHandlers(const FNodeEventData& Data);

	//UActorComponent overrides
	virtual void InitializeComponent() override;
	virtual void UninitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FBoundHandler
	{
		FNodeEventCallback Delegate;
		TWeakObjectPtr<UObject> Target;
		FName FunctionName;
		FNodeNativeEventHandler Native;
	};

	struct FPendingCallback
	{
		FString ScriptName;
		FNodeEventCallback Delegate;
		FNodeNativeEventHandler Native;
	};

	FString OwnerId;

	//Decoder + exit ack flag, kept alive by the pipe reader callback.
	TSharedPtr<FNodeBridgeState, ESPMode::ThreadSafe> Bridge;

	//Frames sent before the own process is up.
	TArray<TArray<uint8>> PendingFrames;

	bool bProcessStartRequested = false;
	bool bDefaultScriptStarted = false;

	//Bumped per own-process start/stop; callbacks from an older process are ignored.
	uint32 ProcessGeneration = 0;
	bool bHandlerUsed = false;

	TSet<FString> RunningScripts;
	TMap<FString, TArray<FBoundHandler>> BoundEvents;
	TMap<int32, FPendingCallback> PendingCallbacks;
	int32 NextAckId = 1;

	TWeakObjectPtr<UNodeJsSubsystem> SharedProcess;
};
