
#include "NodeComponent.h"
#include "NodeJs.h"
#include "SIOMessageConvert.h"
#include "Async/Async.h"
#include "Runtime/Core/Public/HAL/PlatformFilemanager.h"
#include "Runtime/Core/Public/Misc/FileHelper.h"
#include "Runtime/Core/Public/Misc/Paths.h"

bool UNodeComponent::StartScript(const FNodeJsScriptParams& ScriptParams)
{
	FString LaunchMethod = TEXT("launchInline");
	if (!DefaultScriptParams.bInlineLaunchScript)
	{
		LaunchMethod = TEXT("launchSubprocess");
	}

	SendInput(FString::Printf(TEXT("%s %s %s"), *LaunchMethod, *ScriptParams.Script, *ScriptParams.ScriptPathRoot));

	if (DefaultScriptParams.bWatchFileForChanges)
	{
		SendInput(FString::Printf(TEXT("%s %s %s"), TEXT("watch"), *ScriptParams.Script, *ScriptParams.ScriptPathRoot));
	}
	return true;
}

bool UNodeComponent::StopScript(const FNodeJsScriptParams& ScriptParams)
{
	SendInput(FString::Printf(TEXT("%s %s"), TEXT("stop"), *ScriptParams.Script));


	return true;
}

// Sets default values for this component's properties
UNodeComponent::UNodeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	SyncCLIParams();
}

void UNodeComponent::SyncCLIParams()
{
	if (!NodeJsProcessParams.bSyncCLIParams)
	{
		return;
	}

	//Main options
	CLIParams.OptionalWorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + NodeJsProcessParams.ProcessPath);

	FString ProcessNameForCurrentPlatform = NodeJsProcessParams.ProcessName;
	
#if PLATFORM_WINDOWS
	ProcessNameForCurrentPlatform += TEXT(".exe");
#endif

	CLIParams.Url = CLIParams.OptionalWorkingDirectory + ProcessNameForCurrentPlatform;
	CLIParams.bProcessInBytes = NodeJsProcessParams.bProcessInBytes;

	//Basic params
	CLIParams.bLaunchHidden = true;
	CLIParams.bLaunchReallyHidden = true;
	CLIParams.bOutputToGameThread = false;

	//main process script to execute
	CLIParams.Params = NodeJsProcessParams.ProcessScriptPath + NodeJsProcessParams.ProcessScriptName;
}


//UCLIProcessComponent overrides
void UNodeComponent::StartProcess()
{
	//Ensure these are synced before we start
	SyncCLIParams();

	Super::StartProcess();
}



//UActorComponent overrides
void UNodeComponent::InitializeComponent()
{
	Super::InitializeComponent();

	ProcessHandler->OnProcessOutput = [this](const int32 ProcessId, const FString& OutputString)
	{
		//Broadcast to output text on bg thread, this is the full communication stream
		OnOutputText.Broadcast(OutputString);
		
		//Specialize the communication stream based on various categories

		//Handle main process actions
		if (OutputString.StartsWith(TEXT("~[Action]~ ")))
		{
			// Remove the prefix "~[Action]~ "
			FString RemainingString = OutputString.Mid(11); // 11 is the length of "~[Action]~ "

			// Split the remaining string into an array based on spaces
			TArray<FString> Params;
			RemainingString.ParseIntoArray(Params, TEXT(" "), true); // true = ignore empty tokens

			//invalid param
			if (Params.Num() == 0)
			{
				UE_LOG(LogTemp, Log, TEXT("%s has too few params"), *OutputString);
				return;
			}

			if (Params[0] == TEXT("reload") && Params.Num() == 2)
			{
				const FString ScriptPath = Params[1];
				//get the reload callback on game thread
				AsyncTask(ENamedThreads::GameThread, [this, ScriptPath]
				{
					OnScriptReloaded.Broadcast(ScriptPath);

					SendInput(FString::Printf(TEXT("reloadComplete %s"), *ScriptPath));
				});
				return;
			}
			else if (Params[0] == TEXT("end") && Params.Num() == 2)
			{
				const FString ScriptPath = Params[1];
				//get the reload callback on game thread
				AsyncTask(ENamedThreads::GameThread, [this, ScriptPath]
				{
					OnScriptEnd.Broadcast(ScriptPath);
				});
				return;
			}
			else if (Params[0] == TEXT("begin") && Params.Num() == 2)
			{
				const FString ScriptPath = Params[1];
				//get the reload callback on game thread
				AsyncTask(ENamedThreads::GameThread, [this, ScriptPath]
				{
					OnScriptBegin.Broadcast(ScriptPath);
				});
				return;
			}

		}
		else if (OutputString.StartsWith(TEXT("~[Process]~ ")))
		{
			const FString RemainingString = OutputString.Mid(12);

			if (NodeJsProcessParams.bScriptLogsOnGamethread)
			{
				//broadcast everything else as a console log
				AsyncTask(ENamedThreads::GameThread, [this, RemainingString]
				{
					OnProcessScriptLog.Broadcast(RemainingString);
				});
			}
			else
			{
				OnProcessScriptLog.Broadcast(RemainingString);
			}
		}
		//default fallback
		else
		{
			if (NodeJsProcessParams.bScriptLogsOnGamethread)
			{
				const FString RemainingString = OutputString;
				//broadcast everything else as a console log
				AsyncTask(ENamedThreads::GameThread, [this, RemainingString]
				{
					OnConsoleLog.Broadcast(RemainingString);
				});
			}
			else
			{
				OnConsoleLog.Broadcast(OutputString);
			}
		}
	};

	ProcessHandler->OnProcessOutputBytes = [this](const int32 ProcessId, const TArray<uint8>& OutputBytes)
	{
		OnOutputBytes.Broadcast(OutputBytes);
	};
}

void UNodeComponent::UninitializeComponent()
{
	Super::UninitializeComponent();
}

// Called when the game starts
void UNodeComponent::BeginPlay()
{
	Super::BeginPlay();
}


void UNodeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}


// Called every frame
void UNodeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}