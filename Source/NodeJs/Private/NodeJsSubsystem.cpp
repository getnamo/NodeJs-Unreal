// Copyright getnamo. NodeJs-Unreal v2.1.0

#include "NodeJsSubsystem.h"
#include "NodeJs.h"
#include "SubProcess.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Misc/Paths.h"
#include "Json.h"

namespace
{
	TArray<uint8> ControlFrame(const TSharedRef<FJsonObject>& Command)
	{
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Command, Writer);
		return FNodeFrameCodec::Encode(ENodeFrameType::Control, Json);
	}
}

void UNodeJsSubsystem::Deinitialize()
{
	ShutdownProcess();
	Components.Reset();
	Super::Deinitialize();
}

void UNodeJsSubsystem::Register(UNodeComponent* Component)
{
	if (Component)
	{
		Components.Add(Component->GetOwnerId(), Component);
	}
}

void UNodeJsSubsystem::Unregister(UNodeComponent* Component)
{
	if (!Component)
	{
		return;
	}
	Components.Remove(Component->GetOwnerId());

	//Always send (queued if the process isn't up yet) so a still-queued launch doesn't
	//start a script nobody owns.
	{
		TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("cmd"), TEXT("stopOwner"));
		Command->SetStringField(TEXT("owner"), Component->GetOwnerId());
		SendFrame(ControlFrame(Command));
	}
}

void UNodeJsSubsystem::EnsureStarted(const FNodeJsProcessParams& Params)
{
	if (bProcessRunning || bStartRequested)
	{
		return;
	}
	bStartRequested = true;
	ProcessParams = Params;
	const uint32 Gen = ++Generation;

	Bridge = MakeShared<FNodeBridgeState, ESPMode::ThreadSafe>();
	FNodeBridgeState* State = Bridge.Get();
	TWeakObjectPtr<UNodeJsSubsystem> WeakThis(this);

	//Runs on the pipe reader thread: parse there, route on the game thread.
	Bridge->Decoder.OnFrame = [WeakThis, State, Gen](uint8 Type, const FString& RawHeader, const TArray<uint8>& Binary)
	{
		TSharedPtr<FJsonObject> Header = FNodeFrameCodec::ParseJsonHeader(Type, RawHeader);
		if (FNodeFrameCodec::IsExitAck(Type, Header))
		{
			State->bExitAcked = true;
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Type, Header, RawHeader, Binary, Gen]
		{
			UNodeJsSubsystem* Subsystem = WeakThis.Get();
			if (Subsystem && Subsystem->Generation == Gen)
			{
				Subsystem->RouteFrame(Type, Header, RawHeader, Binary);
			}
		});
	};

	//A handler runs one process (e.g. restarting after node crashed): retire the old one.
	UNodeComponent::RetireProcessHandler(MoveTemp(Handler));
	Handler = MakeShared<FSubProcessHandler>();

	TSharedPtr<FNodeBridgeState, ESPMode::ThreadSafe> BridgeRef = Bridge;
	Handler->OnProcessOutputBytes = [BridgeRef](const int32 ProcessId, const TArray<uint8>& OutputBytes)
	{
		BridgeRef->Decoder.Feed(OutputBytes);
	};
	Handler->OnProcessOutput = [](const int32 ProcessId, const FString& Output) {};

	//CLISystem invokes these on the game thread.
	Handler->OnProcessBegin = [WeakThis, Gen](const int32 ProcessId, bool bStartSucceeded)
	{
		UNodeJsSubsystem* Subsystem = WeakThis.Get();
		if (Subsystem && Subsystem->Generation == Gen)
		{
			Subsystem->OnProcessBegin(bStartSucceeded);
		}
	};
	Handler->OnProcessEnd = [WeakThis, Gen](const int32 ProcessId, int32 ReturnCode)
	{
		UNodeJsSubsystem* Subsystem = WeakThis.Get();
		if (Subsystem && Subsystem->Generation == Gen)
		{
			Subsystem->OnProcessEnd();
		}
	};

	FProcessParams CLIParams;
	UNodeComponent::ApplyProcessParams(ProcessParams, CLIParams);
	Handler->StartProcess(CLIParams);
}

void UNodeJsSubsystem::SendFrame(const TArray<uint8>& Frame)
{
	if (bProcessRunning && Handler.IsValid())
	{
		Handler->SendInput(Frame);
		return;
	}
	PendingFrames.Add(Frame);
}

void UNodeJsSubsystem::OnProcessBegin(bool bStartSucceeded)
{
	if (!bStartSucceeded)
	{
		UE_LOG(LogNodeJs, Error, TEXT("Failed to launch shared node process (%s%s)."), *ProcessParams.ProcessPath, *ProcessParams.ProcessName);
		PendingFrames.Reset();
		return;
	}
	bProcessRunning = true;

	TSharedRef<FJsonObject> ScriptsPath = MakeShared<FJsonObject>();
	ScriptsPath->SetStringField(TEXT("cmd"), TEXT("scriptsPath"));
	ScriptsPath->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	SendFrame(ControlFrame(ScriptsPath));

	//Launch default scripts before flushing so queued events find their script.
	TArray<TWeakObjectPtr<UNodeComponent>> Registered;
	Components.GenerateValueArray(Registered);
	for (const TWeakObjectPtr<UNodeComponent>& Weak : Registered)
	{
		if (UNodeComponent* Component = Weak.Get())
		{
			Component->OnSharedProcessBegin();
		}
	}

	TArray<TArray<uint8>> Frames = MoveTemp(PendingFrames);
	PendingFrames.Reset();
	for (const TArray<uint8>& Frame : Frames)
	{
		SendFrame(Frame);
	}
}

void UNodeJsSubsystem::OnProcessEnd()
{
	bProcessRunning = false;
	bStartRequested = false;

	TArray<TWeakObjectPtr<UNodeComponent>> Registered;
	Components.GenerateValueArray(Registered);
	for (const TWeakObjectPtr<UNodeComponent>& Weak : Registered)
	{
		if (UNodeComponent* Component = Weak.Get())
		{
			Component->OnSharedProcessEnd();
		}
	}
}

void UNodeJsSubsystem::RouteFrame(uint8 Type, const TSharedPtr<FJsonObject>& Header, const FString& RawHeader, const TArray<uint8>& Binary)
{
	FString Owner;
	if (Header.IsValid())
	{
		Header->TryGetStringField(TEXT("owner"), Owner);
	}

	if (!Owner.IsEmpty())
	{
		if (const TWeakObjectPtr<UNodeComponent>* Found = Components.Find(Owner))
		{
			if (UNodeComponent* Component = Found->Get())
			{
				Component->HandleFrame(Type, Header, RawHeader, Binary);
			}
		}
		return;
	}

	//Process-level frames (bridge logs, unattributed errors) go to everyone.
	TArray<TWeakObjectPtr<UNodeComponent>> Registered;
	Components.GenerateValueArray(Registered);
	for (const TWeakObjectPtr<UNodeComponent>& Weak : Registered)
	{
		if (UNodeComponent* Component = Weak.Get())
		{
			Component->HandleFrame(Type, Header, RawHeader, Binary);
		}
	}
}

void UNodeJsSubsystem::ShutdownProcess()
{
	if (!Handler.IsValid())
	{
		return;
	}

	if (bProcessRunning && Bridge.IsValid())
	{
		const float Grace = FMath::Max(0.f, ProcessParams.ShutdownGraceSeconds);
		Bridge->bExitAcked = false;
		Handler->SendInput(UNodeComponent::MakeExitFrame(Grace));

		const double Deadline = FPlatformTime::Seconds() + Grace + 0.25;
		while (!Bridge->bExitAcked && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.005f);
		}
	}

	Handler->StopProcess();

	//Ignore anything the old process still reports, e.g. a begin from a start still in flight
	//(the retired handler terminates that process when it is released).
	++Generation;
	UNodeComponent::RetireProcessHandler(MoveTemp(Handler));
	bProcessRunning = false;
	bStartRequested = false;
	PendingFrames.Reset();
}
