
#include "NodeComponent.h"
#include "NodeJs.h"
#include "SIOMessageConvert.h"
#include "CULambdaRunnable.h"
#include "Runtime/Core/Public/HAL/PlatformFilemanager.h"
#include "Runtime/Core/Public/Misc/FileHelper.h"
#include "Runtime/Core/Public/Misc/Paths.h"

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