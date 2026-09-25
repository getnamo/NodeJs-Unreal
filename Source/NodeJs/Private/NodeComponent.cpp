
#include "NodeComponent.h"
#include "NodeJs.h"
#include "NodeJsSubsystem.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Json.h"

namespace
{
	FString ToCondensedJson(const TArray<TSharedPtr<FJsonValue>>& Array)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Array, Writer);
		return Out;
	}

	FString ToCondensedJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	FString HeaderString(const TSharedPtr<FJsonObject>& Header, const TCHAR* Field)
	{
		FString Value;
		if (Header.IsValid())
		{
			Header->TryGetStringField(Field, Value);
		}
		return Value;
	}

	const TCHAR* LaunchModeName(ENodeScriptLaunchMode Mode)
	{
		switch (Mode)
		{
		case ENodeScriptLaunchMode::Worker: return TEXT("worker");
		case ENodeScriptLaunchMode::Subprocess: return TEXT("subprocess");
		default: return TEXT("inline");
		}
	}

	FNodeEventData MakeEventData(const TSharedPtr<FJsonObject>& Header, const TArray<uint8>& Binary)
	{
		FNodeEventData Data;
		if (Header.IsValid())
		{
			Header->TryGetStringField(TEXT("name"), Data.EventName);
			Header->TryGetStringField(TEXT("script"), Data.ScriptName);
			const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
			if (Header->TryGetArrayField(TEXT("args"), Args) && Args)
			{
				Data.Args = *Args;
			}
		}
		Data.JsonArgs = ToCondensedJson(Data.Args);

		TArray<TArray<uint8>> Buffers;
		FNodeFrameCodec::ParseBinaryTable(Binary, Buffers);
		Data.Buffers.Reserve(Buffers.Num());
		for (TArray<uint8>& Buffer : Buffers)
		{
			Data.Buffers.Emplace(MoveTemp(Buffer));
		}
		return Data;
	}

	//Accepted: (FString JsonArgs), (FString JsonArgs, TArray<uint8> Binary), (FNodeEventData Event)
	bool CheckEventFunctionSignature(UFunction* Function, FString& OutError)
	{
		TArray<FProperty*> Params;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Params.Add(*It);
			}
		}

		const bool bSingleString = Params.Num() == 1 && Params[0]->IsA<FStrProperty>();
		const bool bSingleData = Params.Num() == 1 && Params[0]->IsA<FStructProperty>()
			&& CastField<FStructProperty>(Params[0])->Struct == FNodeEventData::StaticStruct();
		const FArrayProperty* ArrayParam = Params.Num() == 2 ? CastField<FArrayProperty>(Params[1]) : nullptr;
		const bool bStringAndBinary = Params.Num() == 2 && Params[0]->IsA<FStrProperty>()
			&& ArrayParam && ArrayParam->Inner->IsA<FByteProperty>();

		if (bSingleString || bSingleData || bStringAndBinary)
		{
			return true;
		}
		OutError = FString::Printf(TEXT("%s must take (FString JsonArgs), (FString JsonArgs, TArray<uint8> Binary) or (FNodeEventData Event)."), *Function->GetName());
		return false;
	}

	void CallEventFunction(UObject* Target, UFunction* Function, const FNodeEventData& Data)
	{
		uint8* Params = (uint8*)FMemory_Alloca(Function->ParmsSize);
		FMemory::Memzero(Params, Function->ParmsSize);

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FProperty* Prop = *It;
			Prop->InitializeValue_InContainer(Params);
			if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			{
				StrProp->SetPropertyValue_InContainer(Params, Data.JsonArgs);
			}
			else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				*StructProp->ContainerPtrToValuePtr<FNodeEventData>(Params) = Data;
			}
			else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				TArray<uint8>* Binary = ArrayProp->ContainerPtrToValuePtr<TArray<uint8>>(Params);
				if (Data.Buffers.Num() > 0)
				{
					*Binary = Data.Buffers[0].Data;
				}
			}
		}

		Target->ProcessEvent(Function, Params);

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params);
		}
	}

	uint32 NextOwnerNumber = 0;
}

//~ Params -------------------------------------------------------------------

void UNodeComponent::RetireProcessHandler(TSharedPtr<FSubProcessHandler> Handler)
{
	if (Handler.IsValid())
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Handler](float) { return false; }), 1.0f);
	}
}

TArray<uint8> UNodeComponent::MakeExitFrame(float GraceSeconds)
{
	const int32 GraceMs = FMath::Max(0, FMath::RoundToInt(GraceSeconds * 1000.f));
	return FNodeFrameCodec::Encode(ENodeFrameType::Control, FString::Printf(TEXT("{\"cmd\":\"exit\",\"grace\":%d}"), GraceMs));
}

void UNodeComponent::ApplyProcessParams(const FNodeJsProcessParams& NodeParams, FProcessParams& OutParams)
{
	FString ProcessName = NodeParams.ProcessName;
#if PLATFORM_WINDOWS
	ProcessName += TEXT(".exe");
#endif

	FString WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + NodeParams.ProcessPath);
	if (!FPaths::FileExists(FPaths::Combine(WorkingDirectory, ProcessName)))
	{
		//Plugin installed elsewhere (renamed folder, engine plugins): use its own bundled node.
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeJs")))
		{
			const FString PluginNodeDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/ThirdParty/node/")));
			if (FPaths::FileExists(FPaths::Combine(PluginNodeDir, ProcessName)))
			{
				WorkingDirectory = PluginNodeDir;
			}
		}
	}
	if (!WorkingDirectory.EndsWith(TEXT("/")))
	{
		WorkingDirectory += TEXT("/");
	}

	OutParams.OptionalWorkingDirectory = WorkingDirectory;
	OutParams.Url = WorkingDirectory + ProcessName;

	//The bridge always runs in bytes mode: the framed protocol interweaves
	//logs, events and binary on the single stdio stream.
	OutParams.bProcessInBytes = true;

	//Basic params
	OutParams.bLaunchHidden = true;
	OutParams.bLaunchReallyHidden = true;
	OutParams.bOutputToGameThread = false;

	//main process script to execute
	OutParams.Params = NodeParams.ProcessScriptPath + NodeParams.ProcessScriptName;
}

//~ Script control ---------------------------------------------------------

TSharedRef<FJsonObject> UNodeComponent::MakeCommand(const FString& Cmd, const FString& ScriptName) const
{
	TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
	Command->SetStringField(TEXT("cmd"), Cmd);
	Command->SetStringField(TEXT("owner"), OwnerId);
	if (!ScriptName.IsEmpty())
	{
		Command->SetStringField(TEXT("script"), ScriptName);
	}
	return Command;
}

bool UNodeComponent::StartScript(const FNodeJsScriptParams& ScriptParams)
{
	TSharedRef<FJsonObject> Command = MakeCommand(TEXT("launch"), ScriptParams.Script);
	Command->SetStringField(TEXT("path"), ScriptParams.ScriptPathRoot);
	Command->SetStringField(TEXT("mode"), LaunchModeName(ScriptParams.GetEffectiveLaunchMode()));

	TArray<TSharedPtr<FJsonValue>> Args;
	for (const FString& Arg : ScriptParams.Args)
	{
		Args.Add(MakeShared<FJsonValueString>(Arg));
	}
	Command->SetArrayField(TEXT("args"), Args);
	Command->SetBoolField(TEXT("npmAutoResolve"), NodeJsProcessParams.bAutoResolveNpmDependencies);
	Command->SetBoolField(TEXT("watch"), ScriptParams.bWatchFileForChanges);
	Command->SetBoolField(TEXT("reload"), ScriptParams.bReloadOnChange);

	SendControl(Command);
	return true;
}

bool UNodeComponent::StopScript(const FNodeJsScriptParams& ScriptParams)
{
	SendControl(MakeCommand(TEXT("stop"), ScriptParams.Script));
	return true;
}

void UNodeComponent::WatchScript(const FNodeJsScriptParams& ScriptParams)
{
	TSharedRef<FJsonObject> Command = MakeCommand(TEXT("watch"), ScriptParams.Script);
	Command->SetBoolField(TEXT("reload"), ScriptParams.bReloadOnChange);
	SendControl(Command);
}

void UNodeComponent::UnwatchScript(const FNodeJsScriptParams& ScriptParams)
{
	SendControl(MakeCommand(TEXT("unwatch"), ScriptParams.Script));
}

bool UNodeComponent::IsScriptRunning(const FString& ScriptName) const
{
	return RunningScripts.Contains(TargetScript(ScriptName));
}

TArray<FString> UNodeComponent::GetRunningScripts() const
{
	return RunningScripts.Array();
}

FString UNodeComponent::GetScriptFullPath(const FNodeJsScriptParams& ScriptParams) const
{
	const FString Primary = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), ScriptParams.ScriptPathRoot, ScriptParams.Script));
	if (FPaths::FileExists(Primary))
	{
		return Primary;
	}
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeJs")))
	{
		const FString Fallback = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), ScriptParams.ScriptPathRoot, ScriptParams.Script));
		if (FPaths::FileExists(Fallback))
		{
			return Fallback;
		}
	}
	return Primary;
}

FString UNodeComponent::TargetScript(const FString& ScriptName) const
{
	return ScriptName.IsEmpty() ? DefaultScriptParams.Script : ScriptName;
}

//~ Npm --------------------------------------------------------------------

void UNodeComponent::ResolveNpmDependencies(const FNodeJsScriptParams& ScriptParams)
{
	TSharedRef<FJsonObject> Command = MakeCommand(TEXT("npmInstall"), ScriptParams.Script);
	Command->SetStringField(TEXT("path"), ScriptParams.ScriptPathRoot);
	SendControl(Command);
}

TArray<FString> UNodeComponent::PackageDependencies(const FNodeJsScriptParams& ScriptParams) const
{
	TArray<FString> Dependencies;

	//Same lookup as process.js: walk up from the script to the nearest package.json.
	FString Dir = FPaths::GetPath(GetScriptFullPath(ScriptParams));
	for (int32 Level = 0; Level < 8 && !Dir.IsEmpty(); ++Level)
	{
		const FString PackagePath = FPaths::Combine(Dir, TEXT("package.json"));
		if (FPaths::FileExists(PackagePath))
		{
			FString Contents;
			TSharedPtr<FJsonObject> Package;
			if (FFileHelper::LoadFileToString(Contents, *PackagePath)
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Contents), Package) && Package.IsValid())
			{
				const TSharedPtr<FJsonObject>* Deps = nullptr;
				if (Package->TryGetObjectField(TEXT("dependencies"), Deps) && Deps)
				{
					for (const auto& Pair : (*Deps)->Values)
					{
						Dependencies.Add(FString(*Pair.Key));
					}
				}
			}
			break;
		}
		const FString Parent = FPaths::GetPath(Dir);
		if (Parent == Dir)
		{
			break;
		}
		Dir = Parent;
	}
	return Dependencies;
}

//~ Event emit -------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> UNodeComponent::BuildArgs(const FString& JsonArgs, int32 NumBuffers) const
{
	TArray<TSharedPtr<FJsonValue>> Args;

	//Parse the caller-provided JSON value (the single event argument).
	if (!JsonArgs.IsEmpty())
	{
		TSharedPtr<FJsonValue> ArgValue;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonArgs);
		if (!FJsonSerializer::Deserialize(Reader, ArgValue) || !ArgValue.IsValid())
		{
			//Fall back to treating it as a raw string argument.
			ArgValue = MakeShared<FJsonValueString>(JsonArgs);
		}
		Args.Add(ArgValue);
	}

	//Append a placeholder per binary buffer unless the JSON places them itself.
	if (!JsonArgs.Contains(TEXT("\"_bin\"")))
	{
		for (int32 i = 0; i < NumBuffers; ++i)
		{
			TSharedRef<FJsonObject> Placeholder = MakeShared<FJsonObject>();
			Placeholder->SetNumberField(TEXT("_bin"), i);
			Args.Add(MakeShared<FJsonValueObject>(Placeholder));
		}
	}
	return Args;
}

void UNodeComponent::EmitEvent(const FString& EventName, const FString& JsonArgs, const FString& ScriptName)
{
	SendEventFrame(EventName, BuildArgs(JsonArgs, 0), TArray<TArray<uint8>>(), ScriptName, 0);
}

void UNodeComponent::EmitEventWithBinary(const FString& EventName, const FString& JsonArgs, const TArray<uint8>& Binary, const FString& ScriptName)
{
	TArray<TArray<uint8>> Buffers;
	Buffers.Add(Binary);
	SendEventFrame(EventName, BuildArgs(JsonArgs, 1), Buffers, ScriptName, 0);
}

void UNodeComponent::EmitEventWithBuffers(const FString& EventName, const FString& JsonArgs, const TArray<FNodeBuffer>& Buffers, const FString& ScriptName)
{
	TArray<TArray<uint8>> Raw;
	Raw.Reserve(Buffers.Num());
	for (const FNodeBuffer& Buffer : Buffers)
	{
		Raw.Add(Buffer.Data);
	}
	SendEventFrame(EventName, BuildArgs(JsonArgs, Raw.Num()), Raw, ScriptName, 0);
}

void UNodeComponent::EmitEventWithCallback(const FString& EventName, const FString& JsonArgs, FNodeEventCallback Callback, const FString& ScriptName)
{
	const int32 AckId = AddPendingCallback(TargetScript(ScriptName), Callback, nullptr);
	SendEventFrame(EventName, BuildArgs(JsonArgs, 0), TArray<TArray<uint8>>(), ScriptName, AckId);
}

void UNodeComponent::EmitEvent(const FString& EventName, const TSharedRef<FJsonObject>& JsonArg, const FString& ScriptName)
{
	TArray<TSharedPtr<FJsonValue>> Args;
	Args.Add(MakeShared<FJsonValueObject>(JsonArg));
	SendEventFrame(EventName, Args, TArray<TArray<uint8>>(), ScriptName, 0);
}

void UNodeComponent::EmitEventNative(const FString& EventName, const TArray<TSharedPtr<FJsonValue>>& Args, const TArray<TArray<uint8>>& Buffers,
	const FString& ScriptName, FNodeNativeEventHandler Callback)
{
	const int32 AckId = Callback ? AddPendingCallback(TargetScript(ScriptName), FNodeEventCallback(), MoveTemp(Callback)) : 0;
	SendEventFrame(EventName, Args, Buffers, ScriptName, AckId);
}

int32 UNodeComponent::AddPendingCallback(const FString& ScriptName, FNodeEventCallback Callback, FNodeNativeEventHandler Native)
{
	const int32 AckId = NextAckId++;
	FPendingCallback& Pending = PendingCallbacks.Add(AckId);
	Pending.ScriptName = ScriptName;
	Pending.Delegate = Callback;
	Pending.Native = MoveTemp(Native);
	return AckId;
}

void UNodeComponent::SendEventFrame(const FString& EventName, const TArray<TSharedPtr<FJsonValue>>& Args, const TArray<TArray<uint8>>& Buffers, const FString& ScriptName, int32 AckId)
{
	TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetStringField(TEXT("owner"), OwnerId);
	Header->SetStringField(TEXT("script"), TargetScript(ScriptName));
	Header->SetStringField(TEXT("name"), EventName);
	Header->SetArrayField(TEXT("args"), Args);
	if (AckId > 0)
	{
		Header->SetNumberField(TEXT("ack"), AckId);
	}

	SendFrame(FNodeFrameCodec::Encode(ENodeFrameType::Event, ToCondensedJson(Header), FNodeFrameCodec::BuildBinaryTable(Buffers)));
}

void UNodeComponent::SendControl(const TSharedRef<FJsonObject>& Command)
{
	SendFrame(FNodeFrameCodec::Encode(ENodeFrameType::Control, ToCondensedJson(Command)));
}

void UNodeComponent::SendFrame(const TArray<uint8>& Frame)
{
	if (UNodeJsSubsystem* Shared = SharedProcess.Get())
	{
		if (bLazyAutoStartProcess)
		{
			Shared->EnsureStarted(NodeJsProcessParams);
		}
		Shared->SendFrame(Frame);
		return;
	}

	if (bProcessIsRunning && ProcessHandler.IsValid())
	{
		ProcessHandler->SendInput(Frame);
		return;
	}

	//Process not up yet (it starts asynchronously): queue until it is.
	PendingFrames.Add(Frame);
	if (bLazyAutoStartProcess)
	{
		StartProcess();
	}
}

void UNodeComponent::FlushPendingFrames()
{
	TArray<TArray<uint8>> Frames = MoveTemp(PendingFrames);
	PendingFrames.Reset();
	for (const TArray<uint8>& Frame : Frames)
	{
		SendFrame(Frame);
	}
}

//~ Binding ----------------------------------------------------------------

void UNodeComponent::BindEvent(const FString& EventName, FNodeEventCallback Callback)
{
	FBoundHandler& Handler = BoundEvents.FindOrAdd(EventName).AddDefaulted_GetRef();
	Handler.Delegate = Callback;
}

bool UNodeComponent::BindEventToFunction(const FString& EventName, const FString& FunctionName, UObject* Target)
{
	if (!Target)
	{
		Target = GetOwner();
	}
	UFunction* Function = Target ? Target->FindFunction(FName(*FunctionName)) : nullptr;
	if (!Function)
	{
		UE_LOG(LogNodeJs, Warning, TEXT("BindEventToFunction: no function '%s' on %s"), *FunctionName, *GetNameSafe(Target));
		return false;
	}

	FString Error;
	if (!CheckEventFunctionSignature(Function, Error))
	{
		UE_LOG(LogNodeJs, Warning, TEXT("BindEventToFunction: %s"), *Error);
		return false;
	}

	FBoundHandler& Handler = BoundEvents.FindOrAdd(EventName).AddDefaulted_GetRef();
	Handler.Target = Target;
	Handler.FunctionName = Function->GetFName();
	return true;
}

void UNodeComponent::UnbindEvent(const FString& EventName)
{
	BoundEvents.Remove(EventName);
}

void UNodeComponent::BindEventNative(const FString& EventName, FNodeNativeEventHandler Handler)
{
	BoundEvents.FindOrAdd(EventName).AddDefaulted_GetRef().Native = MoveTemp(Handler);
}

void UNodeComponent::DispatchBoundHandlers(const FNodeEventData& Data)
{
	const TArray<FBoundHandler>* Found = BoundEvents.Find(Data.EventName);
	if (!Found)
	{
		return;
	}

	//Copy: a handler may bind/unbind while we iterate.
	const TArray<FBoundHandler> Handlers = *Found;
	for (const FBoundHandler& Handler : Handlers)
	{
		if (Handler.Native)
		{
			Handler.Native(Data);
		}
		else if (Handler.Delegate.IsBound())
		{
			Handler.Delegate.Execute(Data);
		}
		else if (UObject* Target = Handler.Target.Get())
		{
			if (UFunction* Function = Target->FindFunction(Handler.FunctionName))
			{
				CallEventFunction(Target, Function, Data);
			}
		}
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
	ApplyProcessParams(NodeJsProcessParams, CLIParams);
}

bool UNodeComponent::IsUsingSharedProcess() const
{
	return SharedProcess.IsValid();
}

//~ Process lifecycle ------------------------------------------------------

void UNodeComponent::StartProcess()
{
	if (UNodeJsSubsystem* Shared = SharedProcess.Get())
	{
		Shared->EnsureStarted(NodeJsProcessParams);
		if (Shared->IsProcessRunning())
		{
			MaybeStartDefaultScript();
		}
		return;
	}

	//Starting twice before the first launch lands would spawn a second node.
	if (bProcessStartRequested || bProcessIsRunning || !ProcessHandler.IsValid())
	{
		return;
	}

	//A handler runs one process: restarts get a fresh one (and a fresh decoder).
	if (bHandlerUsed)
	{
		RetireProcessHandler(MoveTemp(ProcessHandler));
		ProcessHandler = MakeShared<FSubProcessHandler>();
		InstallProcessHandler();
	}
	bHandlerUsed = true;
	bProcessStartRequested = true;

	//Ensure these are synced before we start
	SyncCLIParams();

	Super::StartProcess();
}

void UNodeComponent::StopProcess()
{
	if (UNodeJsSubsystem* Shared = SharedProcess.Get())
	{
		//The shared process stays up for other components; just stop ours.
		SendControl(MakeCommand(TEXT("stopOwner")));
		ResetScriptState();
		return;
	}

	GracefulExit();
	Super::StopProcess();

	//Don't wait for the async end: allow an immediate restart and ignore that late end.
	const bool bWasActive = bProcessIsRunning || bProcessStartRequested;
	++ProcessGeneration;
	bProcessIsRunning = false;
	bProcessStartRequested = false;
	ResetScriptState();
	if (bWasActive)
	{
		OnEndProcessing.Broadcast(TEXT("Stopped"));
	}
}

void UNodeComponent::BeginProcessingExtraHandler(const FString& StartUpState)
{
	if (StartUpState.EndsWith(TEXT("0")))
	{
		UE_LOG(LogNodeJs, Error, TEXT("Failed to launch node at %s"), *CLIParams.Url);
		PendingFrames.Reset();
		return;
	}
	OnOwnProcessReady();
}

void UNodeComponent::EndProcessingExtraHandler(const FString& EndState)
{
	bProcessStartRequested = false;
	ResetScriptState();
}

void UNodeComponent::OnOwnProcessReady()
{
	//Point process.js at the project root (absolute, so plugin location doesn't matter).
	TSharedRef<FJsonObject> ScriptsPath = MakeCommand(TEXT("scriptsPath"));
	ScriptsPath->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	SendControl(ScriptsPath);

	//Launch before flushing so events queued at BeginPlay find their script.
	MaybeStartDefaultScript();
	FlushPendingFrames();
}

void UNodeComponent::OnSharedProcessBegin()
{
	MaybeStartDefaultScript();
}

void UNodeComponent::OnSharedProcessEnd()
{
	ResetScriptState();
}

void UNodeComponent::MaybeStartDefaultScript()
{
	if (HasBegunPlay() && NodeJsProcessParams.bStartDefaultScriptOnBeginPlay && !bDefaultScriptStarted)
	{
		bDefaultScriptStarted = true;
		StartScript(DefaultScriptParams);
	}
}

void UNodeComponent::GracefulExit()
{
	if (!bProcessIsRunning || !ProcessHandler.IsValid() || !Bridge.IsValid())
	{
		return;
	}

	const float Grace = FMath::Max(0.f, NodeJsProcessParams.ShutdownGraceSeconds);
	Bridge->bExitAcked = false;
	ProcessHandler->SendInput(MakeExitFrame(Grace));

	//process.js acks once scripts are down (at most Grace later). The ack is flagged
	//on the pipe reader thread, so waiting here doesn't deadlock.
	const double Deadline = FPlatformTime::Seconds() + Grace + 0.25;
	while (!Bridge->bExitAcked && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.005f);
	}
}

void UNodeComponent::ResetScriptState()
{
	RunningScripts.Reset();
	PendingCallbacks.Reset();
	bDefaultScriptStarted = false;
}

//~ Frame routing ----------------------------------------------------------

void UNodeComponent::HandleFrame(uint8 Type, const TSharedPtr<FJsonObject>& Header, const FString& RawHeader, const TArray<uint8>& Binary)
{
	switch (Type)
	{
	case ENodeFrameType::Log:
	{
		OnConsoleLog.Broadcast(Header.IsValid() ? HeaderString(Header, TEXT("msg")) : RawHeader);
		break;
	}
	case ENodeFrameType::ProcessLog:
	{
		OnProcessScriptLog.Broadcast(RawHeader);
		break;
	}
	case ENodeFrameType::Action:
	{
		const FString Verb = HeaderString(Header, TEXT("verb"));
		const FString Script = HeaderString(Header, TEXT("script"));
		const FString ScriptPath = HeaderString(Header, TEXT("path"));

		if (Verb == TEXT("begin"))
		{
			RunningScripts.Add(Script);
			OnScriptBegin.Broadcast(ScriptPath);
		}
		else if (Verb == TEXT("end"))
		{
			RunningScripts.Remove(Script);

			//Replies can no longer arrive for events this instance received. Later ids may
			//belong to a restarted instance, so keep those.
			double MaxAck = 0.0;
			if (Header.IsValid())
			{
				Header->TryGetNumberField(TEXT("maxAck"), MaxAck);
			}
			for (auto It = PendingCallbacks.CreateIterator(); It; ++It)
			{
				if (It.Value().ScriptName == Script && It.Key() <= MaxAck)
				{
					It.RemoveCurrent();
				}
			}
			OnScriptEnd.Broadcast(ScriptPath);
		}
		else if (Verb == TEXT("reload"))
		{
			OnScriptReloaded.Broadcast(ScriptPath);
		}
		else if (Verb == TEXT("changed"))
		{
			OnScriptChanged.Broadcast(ScriptPath);
		}
		break;
	}
	case ENodeFrameType::Event:
	{
		const FNodeEventData Data = MakeEventData(Header, Binary);
		const TArray<uint8> FirstBuffer = Data.Buffers.Num() > 0 ? Data.Buffers[0].Data : TArray<uint8>();

		OnEvent.Broadcast(Data.EventName, Data.JsonArgs, FirstBuffer);
		OnEventWithBuffers.Broadcast(Data);
		DispatchBoundHandlers(Data);
		break;
	}
	case ENodeFrameType::Ack:
	{
		int32 AckId = 0;
		if (!Header.IsValid() || !Header->TryGetNumberField(TEXT("ack"), AckId))
		{
			break;
		}
		FPendingCallback Pending;
		if (!PendingCallbacks.RemoveAndCopyValue(AckId, Pending))
		{
			break;
		}
		const FString AckError = HeaderString(Header, TEXT("error"));
		if (!AckError.IsEmpty())
		{
			UE_LOG(LogNodeJs, Warning, TEXT("Callback for script '%s' dropped: %s"), *Pending.ScriptName, *AckError);
			break;
		}
		const FNodeEventData Data = MakeEventData(Header, Binary);
		if (Pending.Native)
		{
			Pending.Native(Data);
		}
		else
		{
			Pending.Delegate.ExecuteIfBound(Data);
		}
		break;
	}
	case ENodeFrameType::Error:
	{
		const FString Script = HeaderString(Header, TEXT("script"));
		const FString Message = Header.IsValid() ? HeaderString(Header, TEXT("message")) : RawHeader;

		if (NodeJsProcessParams.bLogScriptErrorsToOutput)
		{
			UE_LOG(LogNodeJs, Error, TEXT("[%s] %s"), *Script, *Message);
		}
		OnScriptError.Broadcast(Script, Message);
		break;
	}
	case ENodeFrameType::Npm:
	{
		bool bInstalled = false;
		if (Header.IsValid())
		{
			Header->TryGetBoolField(TEXT("installed"), bInstalled);
		}
		OnNpmDependenciesResolved.Broadcast(bInstalled, HeaderString(Header, TEXT("error")));
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

	EnsureOwnerId();

	//handle script at startup if relevant
	OnBeginProcessing.AddUniqueDynamic(this, &UNodeComponent::BeginProcessingExtraHandler);
	OnEndProcessing.AddUniqueDynamic(this, &UNodeComponent::EndProcessingExtraHandler);

	InstallProcessHandler();
}

void UNodeComponent::InstallProcessHandler()
{
	if (!ProcessHandler.IsValid())
	{
		return;
	}

	//Fresh decoder per process, so a partial frame from a killed node can't eat the next one's frames.
	Bridge = MakeShared<FNodeBridgeState, ESPMode::ThreadSafe>();
	const uint32 Generation = ++ProcessGeneration;

	TWeakObjectPtr<UNodeComponent> WeakThis(this);
	FNodeBridgeState* State = Bridge.Get();
	const bool bLogsOnGameThread = NodeJsProcessParams.bScriptLogsOnGamethread;

	//Runs on the pipe reader thread: parse there, deliver on the game thread.
	Bridge->Decoder.OnFrame = [WeakThis, State, bLogsOnGameThread, Generation](uint8 Type, const FString& RawHeader, const TArray<uint8>& Binary)
	{
		TSharedPtr<FJsonObject> Header = FNodeFrameCodec::ParseJsonHeader(Type, RawHeader);
		if (FNodeFrameCodec::IsExitAck(Type, Header))
		{
			State->bExitAcked = true;
		}

		if (!bLogsOnGameThread && (Type == ENodeFrameType::Log || Type == ENodeFrameType::ProcessLog))
		{
			if (UNodeComponent* Component = WeakThis.Get())
			{
				Component->HandleFrame(Type, Header, RawHeader, Binary);
			}
			return;
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Type, Header, RawHeader, Binary, Generation]
		{
			UNodeComponent* Component = WeakThis.Get();
			if (Component && Component->ProcessGeneration == Generation)
			{
				Component->HandleFrame(Type, Header, RawHeader, Binary);
			}
		});
	};

	//All output arrives framed via the bytes channel; feed the decoder.
	TSharedPtr<FNodeBridgeState, ESPMode::ThreadSafe> BridgeRef = Bridge;
	ProcessHandler->OnProcessOutputBytes = [BridgeRef](const int32 ProcessId, const TArray<uint8>& OutputBytes)
	{
		BridgeRef->Decoder.Feed(OutputBytes);
	};
	ProcessHandler->OnProcessOutput = [](const int32 ProcessId, const FString& Output) {};

	//Replace UCLIProcessComponent's begin/end handlers so a late end from an older process
	//can't mark a newer one as stopped. Both run on the game thread.
	ProcessHandler->OnProcessBegin = [WeakThis, Generation](const int32 ProcessId, bool bStartSucceeded)
	{
		UNodeComponent* Component = WeakThis.Get();
		if (!Component || Component->ProcessGeneration != Generation)
		{
			return;
		}
		Component->bProcessIsRunning = true;
		Component->OnBeginProcessing.Broadcast(FString::Printf(TEXT("Startup Success: %d"), bStartSucceeded));
	};
	ProcessHandler->OnProcessEnd = [WeakThis, Generation](const int32 ProcessId, int32 ReturnCode)
	{
		UNodeComponent* Component = WeakThis.Get();
		if (!Component || Component->ProcessGeneration != Generation)
		{
			return;
		}
		Component->bProcessIsRunning = false;
		Component->OnEndProcessing.Broadcast(FString::Printf(TEXT("ReturnCode: %d"), ReturnCode));
	};
}

void UNodeComponent::UninitializeComponent()
{
	Super::UninitializeComponent();
}

void UNodeComponent::EnsureOwnerId()
{
	if (OwnerId.IsEmpty())
	{
		OwnerId = FString::Printf(TEXT("nc%u"), ++NextOwnerNumber);
	}
}

void UNodeComponent::BeginPlay()
{
	EnsureOwnerId();

	if (NodeJsProcessParams.bShareMainProcess)
	{
		UWorld* World = GetWorld();
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UNodeJsSubsystem* Shared = GameInstance ? GameInstance->GetSubsystem<UNodeJsSubsystem>() : nullptr;
		if (Shared)
		{
			SharedProcess = Shared;
			Shared->Register(this);
		}
		else
		{
			UE_LOG(LogNodeJs, Warning, TEXT("%s: bShareMainProcess needs a game instance; using its own node process."), *GetName());
		}
	}

	if (UNodeJsSubsystem* Shared = SharedProcess.Get())
	{
		//Skip UCLIProcessComponent::BeginPlay, which would start a process of our own.
		UActorComponent::BeginPlay();

		if (bStartProcessOnBeginPlay || NodeJsProcessParams.bStartDefaultScriptOnBeginPlay)
		{
			Shared->EnsureStarted(NodeJsProcessParams);
		}
		if (Shared->IsProcessRunning())
		{
			MaybeStartDefaultScript();
		}
		return;
	}

	Super::BeginPlay();

	//Process was already started (e.g. lazily, before BeginPlay).
	if (bProcessIsRunning)
	{
		MaybeStartDefaultScript();
	}
}

void UNodeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UNodeJsSubsystem* Shared = SharedProcess.Get())
	{
		Shared->Unregister(this);
		SharedProcess = nullptr;
	}
	else
	{
		GracefulExit();
	}

	ResetScriptState();
	PendingFrames.Reset();

	Super::EndPlay(EndPlayReason);
}

void UNodeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}
