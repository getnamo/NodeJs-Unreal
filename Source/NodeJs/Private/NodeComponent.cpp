
#include "NodeComponent.h"
#include "NodeJs.h"
#include "Async/Async.h"
#include "Runtime/Core/Public/Misc/Paths.h"
#include "Json.h"

//~ Script control ---------------------------------------------------------

bool UNodeComponent::StartScript(const FNodeJsScriptParams& ScriptParams)
{
	//Push current options before launching so an early script error resolves correctly.
	SendControl(FString::Printf(TEXT("npmAutoResolve %d"), NodeJsProcessParams.bAutoResolveNpmDependencies ? 1 : 0));

	FString LaunchMethod = TEXT("launchInline");
	if (!ScriptParams.bInlineLaunchScript)
	{
		LaunchMethod = TEXT("launchSubprocess");
	}

	SendControl(FString::Printf(TEXT("%s %s %s"), *LaunchMethod, *ScriptParams.Script, *ScriptParams.ScriptPathRoot));

	//Delay watching by a frame so the launch lands first
	if (ScriptParams.bWatchFileForChanges)
	{
		const FNodeJsScriptParams Captured = ScriptParams;
		AsyncTask(ENamedThreads::GameThread, [this, Captured]
		{
			SendControl(FString::Printf(TEXT("watch %s %s"), *Captured.Script, *Captured.ScriptPathRoot));
		});
	}
	return true;
}

bool UNodeComponent::StopScript(const FNodeJsScriptParams& ScriptParams)
{
	SendControl(FString::Printf(TEXT("stop %s"), *ScriptParams.Script));
	return true;
}

//~ Event emit -------------------------------------------------------------

void UNodeComponent::EmitEvent(const FString& EventName, const FString& JsonArgs, const FString& ScriptName)
{
	SendEventFrame(EventName, JsonArgs, TArray<TArray<uint8>>(), ScriptName);
}

void UNodeComponent::EmitEventWithBinary(const FString& EventName, const FString& JsonArgs, const TArray<uint8>& Binary, const FString& ScriptName)
{
	TArray<TArray<uint8>> Buffers;
	Buffers.Add(Binary);
	SendEventFrame(EventName, JsonArgs, Buffers, ScriptName);
}

void UNodeComponent::EmitEvent(const FString& EventName, const TSharedRef<FJsonObject>& JsonArg, const FString& ScriptName)
{
	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(JsonArg, Writer);
	EmitEvent(EventName, Serialized, ScriptName);
}

void UNodeComponent::SendEventFrame(const FString& EventName, const FString& JsonArgs, const TArray<TArray<uint8>>& Buffers, const FString& ScriptName)
{
	const FString TargetScript = ScriptName.IsEmpty() ? DefaultScriptParams.Script : ScriptName;

	//Parse the caller-provided JSON value (the single event argument).
	TSharedPtr<FJsonValue> ArgValue;
	if (!JsonArgs.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonArgs);
		if (!FJsonSerializer::Deserialize(Reader, ArgValue) || !ArgValue.IsValid())
		{
			//Fall back to treating it as a raw string argument.
			ArgValue = MakeShared<FJsonValueString>(JsonArgs);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Args;
	if (ArgValue.IsValid())
	{
		Args.Add(ArgValue);
	}

	//Append a placeholder per binary buffer; the node bridge swaps them for Buffers.
	for (int32 i = 0; i < Buffers.Num(); ++i)
	{
		TSharedRef<FJsonObject> Placeholder = MakeShared<FJsonObject>();
		Placeholder->SetNumberField(TEXT("_bin"), i);
		Args.Add(MakeShared<FJsonValueObject>(Placeholder));
	}

	TSharedRef<FJsonObject> HeaderObj = MakeShared<FJsonObject>();
	HeaderObj->SetStringField(TEXT("script"), TargetScript);
	HeaderObj->SetStringField(TEXT("name"), EventName);
	HeaderObj->SetArrayField(TEXT("args"), Args);

	FString HeaderJson;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&HeaderJson);
	FJsonSerializer::Serialize(HeaderObj, Writer);

	const TArray<uint8> BinaryTable = FNodeFrameCodec::BuildBinaryTable(Buffers);
	const TArray<uint8> Frame = FNodeFrameCodec::Encode(ENodeFrameType::Event, HeaderJson, BinaryTable);

	if (bLazyAutoStartProcess && !bProcessIsRunning)
	{
		StartProcess();
	}
	if (ProcessHandler.IsValid())
	{
		ProcessHandler->SendInput(Frame);
	}
}

void UNodeComponent::SendControl(const FString& CommandLine)
{
	const TArray<uint8> Frame = FNodeFrameCodec::Encode(ENodeFrameType::Control, CommandLine);

	if (bLazyAutoStartProcess && !bProcessIsRunning)
	{
		StartProcess();
	}
	if (ProcessHandler.IsValid())
	{
		ProcessHandler->SendInput(Frame);
	}
}

//~ Construction / params --------------------------------------------------

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

	//The bridge always runs in bytes mode: the framed protocol interweaves
	//logs, events and binary on the single stdio stream.
	CLIParams.bProcessInBytes = true;

	//Basic params
	CLIParams.bLaunchHidden = true;
	CLIParams.bLaunchReallyHidden = true;
	CLIParams.bOutputToGameThread = false;

	//main process script to execute
	CLIParams.Params = NodeJsProcessParams.ProcessScriptPath + NodeJsProcessParams.ProcessScriptName;
}

//~ UCLIProcessComponent overrides -----------------------------------------

void UNodeComponent::StartProcess()
{
	//Ensure these are synced before we start
	SyncCLIParams();

	Super::StartProcess();
}

void UNodeComponent::BeginProcessingExtraHandler(const FString& StartUpState)
{
	if (HasBegunPlay() && NodeJsProcessParams.bStartDefaultScriptOnBeginPlay)
	{
		StartScript(DefaultScriptParams);
	}
}

//~ Frame routing ----------------------------------------------------------

void UNodeComponent::HandleFrame(uint8 Type, const FString& Header, const TArray<uint8>& Binary)
{
	switch (Type)
	{
	case ENodeFrameType::Log:
	{
		if (NodeJsProcessParams.bScriptLogsOnGamethread)
		{
			const FString Msg = Header;
			AsyncTask(ENamedThreads::GameThread, [this, Msg] { OnConsoleLog.Broadcast(Msg); });
		}
		else
		{
			OnConsoleLog.Broadcast(Header);
		}
		break;
	}
	case ENodeFrameType::ProcessLog:
	{
		if (NodeJsProcessParams.bScriptLogsOnGamethread)
		{
			const FString Msg = Header;
			AsyncTask(ENamedThreads::GameThread, [this, Msg] { OnProcessScriptLog.Broadcast(Msg); });
		}
		else
		{
			OnProcessScriptLog.Broadcast(Header);
		}
		break;
	}
	case ENodeFrameType::Action:
	{
		TArray<FString> Parts;
		Header.ParseIntoArray(Parts, TEXT(" "), true);
		if (Parts.Num() < 2)
		{
			UE_LOG(LogTemp, Log, TEXT("NodeJs: malformed action frame '%s'"), *Header);
			break;
		}
		const FString Verb = Parts[0];
		const FString ScriptPath = Parts[1];

		if (Verb == TEXT("reload"))
		{
			AsyncTask(ENamedThreads::GameThread, [this, ScriptPath]
			{
				OnScriptReloaded.Broadcast(ScriptPath);
				SendControl(FString::Printf(TEXT("reloadComplete %s"), *ScriptPath));
			});
		}
		else if (Verb == TEXT("end"))
		{
			AsyncTask(ENamedThreads::GameThread, [this, ScriptPath] { OnScriptEnd.Broadcast(ScriptPath); });
		}
		else if (Verb == TEXT("begin"))
		{
			AsyncTask(ENamedThreads::GameThread, [this, ScriptPath] { OnScriptBegin.Broadcast(ScriptPath); });
		}
		break;
	}
	case ENodeFrameType::Event:
	{
		//Header: { "script":..., "name":..., "args":[ ... ] }
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Header);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("NodeJs: bad event header '%s'"), *Header);
			break;
		}

		FString EventName;
		Obj->TryGetStringField(TEXT("name"), EventName);

		//Re-serialize the args array as the delegate payload.
		FString ArgsJson = TEXT("[]");
		const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
		if (Obj->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray)
		{
			ArgsJson.Reset();
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
			FJsonSerializer::Serialize(*ArgsArray, Writer);
		}

		//First interweaved buffer (if any) is surfaced directly to Blueprint.
		TArray<TArray<uint8>> Buffers;
		FNodeFrameCodec::ParseBinaryTable(Binary, Buffers);
		const TArray<uint8> FirstBuffer = Buffers.Num() > 0 ? Buffers[0] : TArray<uint8>();

		AsyncTask(ENamedThreads::GameThread, [this, EventName, ArgsJson, FirstBuffer]
		{
			OnEvent.Broadcast(EventName, ArgsJson, FirstBuffer);
		});
		break;
	}
	case ENodeFrameType::Error:
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Header);
		FString ScriptPath, Message;
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
		{
			Obj->TryGetStringField(TEXT("script"), ScriptPath);
			Obj->TryGetStringField(TEXT("message"), Message);
		}
		else
		{
			Message = Header;
		}

		if (NodeJsProcessParams.bLogScriptErrorsToOutput)
		{
			UE_LOG(LogNodeJs, Error, TEXT("[%s] %s"), *ScriptPath, *Message);
		}

		AsyncTask(ENamedThreads::GameThread, [this, ScriptPath, Message]
		{
			OnScriptError.Broadcast(ScriptPath, Message);
		});
		break;
	}
	case ENodeFrameType::Npm:
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Header);
		bool bInstalled = false;
		FString ErrorMessage;
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
		{
			Obj->TryGetBoolField(TEXT("installed"), bInstalled);
			Obj->TryGetStringField(TEXT("error"), ErrorMessage);
		}
		AsyncTask(ENamedThreads::GameThread, [this, bInstalled, ErrorMessage]
		{
			OnNpmDependenciesResolved.Broadcast(bInstalled, ErrorMessage);
		});
		break;
	}
	default:
		break;
	}
}

//~ UActorComponent overrides ----------------------------------------------

void UNodeComponent::InitializeComponent()
{
	Super::InitializeComponent();

	//handle script at startup if relevant
	OnBeginProcessing.AddDynamic(this, &UNodeComponent::BeginProcessingExtraHandler);

	Decoder.OnFrame = [this](uint8 Type, const FString& Header, const TArray<uint8>& Binary)
	{
		HandleFrame(Type, Header, Binary);
	};

	//All output arrives framed via the bytes channel; feed the decoder.
	ProcessHandler->OnProcessOutputBytes = [this](const int32 ProcessId, const TArray<uint8>& OutputBytes)
	{
		Decoder.Feed(OutputBytes);
	};
}

void UNodeComponent::UninitializeComponent()
{
	Super::UninitializeComponent();
}

void UNodeComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UNodeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void UNodeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}
