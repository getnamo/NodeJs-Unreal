#include "NodeCmd.h"
#include "SIOJConvert.h"

//Windows Includes
#include "Windows/PreWindowsApi.h"
#include "Windows/AllowWindowsPlatformTypes.h"
//#include "AllowWindowsPlatformAtomics.h"

#include "Windows.h"
#include <tchar.h>
#include <stdio.h> 
#include <strsafe.h>
#include <string>
#include <iostream>
#pragma comment(lib, "User32.lib")

//#include "HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"
#include "Windows/PostWindowsApi.h"
//End Windows

#include "CULambdaRunnable.h"

#define BUFSIZE 4096

//windows vars set as static such that they don't get exposed. Only one main process is run anyway.
HANDLE g_hChildStd_OUT_Rd = nullptr;
HANDLE g_hChildStd_OUT_Wr = nullptr;
HANDLE g_hChildStd_ERR_Rd = nullptr;
HANDLE g_hChildStd_ERR_Wr = nullptr;
HANDLE g_hChildStd_IN_Rd = nullptr;
PROCESS_INFORMATION CreateChildProcess(const FString& Process, const FString& Commands, const FString& InProcessDirectory);

FNodeCmd::FNodeCmd()
{
	DefaultMainScript = TEXT("nodeWrapper.js");
	DefaultPort = 4269;
	bShouldMainRun = true;

	ProcessDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + TEXT("Plugins/NodeJs-Unreal/Source/ThirdParty/node"));
	PluginContentRelativePath = TEXT("../../../Content/Scripts/");
	Socket = MakeShareable(new FSocketIONative);
	bShouldStopMainScriptOnNoListeners = false;
	bIsWatchingScript = false;

	//swap this to debug main script with an external local server, otherwise crashes will be relatively opaque
	bUseRemoteMainScript = false;

	NodeExe = TEXT("node.exe");
	MainScriptRelativePath = TEXT("");
}

FNodeCmd::~FNodeCmd()
{
	//todo: convert to listener & static alloc
	StopMainScriptSync();

	//block until the other thread quits
	while (bIsMainRunning)
	{

	}
}

void FNodeCmd::StartupMainScriptIfNeeded()
{
	if (!bIsMainRunning) 
	{
		RunMainScript(PluginContentRelativePath + DefaultMainScript, DefaultPort);
	}
}

void FNodeCmd::AddEventListener(TSharedPtr<FNodeEventListener> Listener)
{
	Listeners.AddUnique(Listener);

	if (bIsMainRunning)
	{
		Listener->OnMainScriptBegin(MainScriptRelativePath);
	}
	else
	{
		StartupMainScriptIfNeeded();
	}
}

void FNodeCmd::RemoveEventListener(TSharedPtr<FNodeEventListener> Listener)
{
	Listeners.Remove(Listener);

	UE_LOG(LogTemp, Log, TEXT("Removed a listener, %d listeners left"), Listeners.Num());
	
	//removed last listener? stop main script
	if (bShouldStopMainScriptOnNoListeners && Listeners.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Stopping main script."));
		StopMainScript();
	}
}

void FNodeCmd::WatchScriptForChanges(const FString& ScriptRelativePath, TFunction<void(const FString& ScriptRelativePath)> OnChildScriptChanged)
{
	if (!bIsMainRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("Can't watch %s because mainscript isn't running."), *ScriptRelativePath);
		return;
	}
	Socket->Emit(TEXT("watchScriptFile"), ScriptRelativePath);
	
	Socket->OnEvent(TEXT("watchCallback@") + ScriptRelativePath, [&, OnChildScriptChanged](const FString& EventName, const TSharedPtr<FJsonValue>& Message)
	{
		//Obtain result and notify callback
		FString ReceivedScriptRelativePath = Message->AsString();
		if (OnChildScriptChanged)
		{
			OnChildScriptChanged(ReceivedScriptRelativePath);
		}
	});

	bIsWatchingScript = true;
}

void FNodeCmd::StopWatchingScript(const FString& ScriptRelativePath)
{
	if (!bIsWatchingScript)
	{
		return;
	}

	bIsWatchingScript = false;
	if (!bIsMainRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("Can't watch %s because mainscript isn't running."), *ScriptRelativePath);
		return;
	}
	Socket->Emit(TEXT("unwatchScriptFile"), ScriptRelativePath);
}

bool FNodeCmd::RunMainScript(FString ScriptRelativePath, int32 Port)
{
	//Script already running? return false
	if (bIsMainRunning) 
	{
		return false;
	}

	if (Socket->bIsConnected) 
	{
		Socket->SyncDisconnect();	//this will block for ~1 sec
	}

	UE_LOG(LogTemp, Log, TEXT("RunScriptStart"));
	Socket->OnConnectedCallback = [&, ScriptRelativePath](const FString& SocketId, const FString& SessionId)
	{
		UE_LOG(LogTemp, Log, TEXT("Main script Connected."));

		MainScriptRelativePath = ScriptRelativePath;

		for (auto Listener : Listeners)
		{
			Listener->OnMainScriptBegin(ScriptRelativePath);
		}
	};
	Socket->OnReconnectionCallback = [&](uint32 AttemptCount, uint32 DelayInMs) 
	{
		UE_LOG(LogTemp, Error, TEXT("Main script connection error! Likely crash, stopping main script."));
		bShouldMainRun = false;

		MainScriptRelativePath = TEXT("");
	};

	//Mainscript console.log
	Socket->OnEvent(TEXT("console.log"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		UE_LOG(LogTemp, Log, TEXT("console.log: %s"), *USIOJConvert::ToJsonString(Message));
		for (auto Listener : Listeners)
		{
			if (Listener->OnConsoleLog)
			{
				Listener->OnConsoleLog(USIOJConvert::ToJsonString(Message));
			}
		}
	});

	//Only forwards script console.log
	Socket->OnEvent(TEXT("script.log"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		auto ArrayMessage = Message->AsArray();
		if (ArrayMessage.Num() != 2)
		{
			UE_LOG(LogTemp, Error, TEXT("script.log error, incorrect response"));
			return;
		}

		FString MessageString = USIOJConvert::ToJsonString(ArrayMessage[0]);
		int32 Pid = (int32)ArrayMessage[1]->AsNumber();

		//UE_LOG(LogTemp, Log, TEXT("console.log %s"), *USIOJConvert::ToJsonString(Message));
		for (auto Listener : Listeners)
		{
			if (Listener->OnScriptConsoleLog && Listener->ProcessId == Pid)
			{
				Listener->OnScriptConsoleLog(MessageString);
			}
		}
	});
	Socket->OnEvent(TEXT("mainScriptEnd"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		UE_LOG(LogTemp, Log, TEXT("mainScriptEnd %s"), *USIOJConvert::ToJsonString(Message));
		Socket->Disconnect();
		bShouldMainRun = false;
	});
	Socket->OnEvent(TEXT("childScriptEnd"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		int32 ProcessId = (int32)Message->AsNumber();
		for (auto Listener : Listeners)
		{
			if (Listener->OnChildScriptEnd && Listener->ProcessId == ProcessId)
			{
				Listener->OnChildScriptEnd(ProcessId);
			}
		}
	});
	Socket->OnEvent(TEXT("childScriptError"), [&, ScriptRelativePath](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		UE_LOG(LogTemp, Error, TEXT("Script Error: %s"), *USIOJConvert::ToJsonString(Message));
		const FString SafePath = ScriptRelativePath;
		const FString SafeErrorMessage = USIOJConvert::ToJsonString(Message);
		
		for (auto Listener : Listeners)
		{
			if (Listener->OnScriptError)
			{
				Listener->OnScriptError(SafePath, SafeErrorMessage);
			}
		}
	});

	Socket->Connect(FString::Printf(TEXT("http://localhost:%d"), Port));

	//bypass actually running the script in this scenario
	if (bUseRemoteMainScript)
	{
		FCULambdaRunnable::RunLambdaOnBackGroundThread([&, ScriptRelativePath]
		{
			bIsMainRunning = true;
			bShouldMainRun = true;
			while (bShouldMainRun)
			{
				FPlatformProcess::Sleep(0.1f);
			}
			const FString FinishPath = ScriptRelativePath;

			FCULambdaRunnable::RunShortLambdaOnGameThread([this, FinishPath]
			{
				bIsMainRunning = false;
				for (auto Listener : Listeners)
				{
					if (Listener->OnMainScriptEnd)
					{
						Listener->OnMainScriptEnd(FinishPath);
					}
				}
			});
		});
		return true;
	}

	FCULambdaRunnable::RunLambdaOnBackGroundThread([&, ScriptRelativePath]
	{
		UE_LOG(LogTemp, Log, TEXT("node thread start"));
		bIsMainRunning = true;

		SECURITY_ATTRIBUTES sa;
		// Set the bInheritHandle flag so pipe handles are inherited. 
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = 1;
		sa.lpSecurityDescriptor = NULL;
		// Create a pipe for the child process's STDERR. 
		if (!CreatePipe(&g_hChildStd_ERR_Rd, &g_hChildStd_ERR_Wr, &sa, 0)) {
			return;
		}
		// Ensure the read handle to the pipe for STDERR is not inherited.
		if (!SetHandleInformation(g_hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0)) {
			return;
		}
		// Create a pipe for the child process's STDOUT. 
		if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &sa, 0)) {
			return;
		}
		// Ensure the read handle to the pipe for STDOUT is not inherited
		if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
			return;
		}

		bShouldMainRun = true;

		UE_LOG(LogTemp, Log, TEXT("Starting %s at %s with %s"), *NodeExe, *ProcessDirectory, *ScriptRelativePath)

		PROCESS_INFORMATION piProcInfo = CreateChildProcess(NodeExe, ScriptRelativePath, ProcessDirectory);

		if (piProcInfo.dwProcessId == (DWORD)0)
		{
			UE_LOG(LogTemp, Warning, TEXT("Node process terminated early, see earlier error message."));
			bShouldMainRun = false;
		}

		while (bShouldMainRun)
		{
			FPlatformProcess::Sleep(0.1f);
		}
		if (Socket->bIsConnected) 
		{
			Socket->SyncDisconnect();
		}

		TerminateProcess(piProcInfo.hProcess, 1);

		UE_LOG(LogTemp, Log, TEXT("RunScriptTerminated"));
		const FString FinishPath = ScriptRelativePath;

		FCULambdaRunnable::RunShortLambdaOnGameThread([this, FinishPath] 
		{
			bIsMainRunning = false;
			for (auto Listener : Listeners)
			{
				if (Listener->OnMainScriptEnd)
				{
					Listener->OnMainScriptEnd(FinishPath);
				}
			}
		});
	});

	return true;
}

void FNodeCmd::RunChildScript(const FString& ScriptRelativePath, FNodeEventListener* Listener /*= nullptr*/)
{
	//StartupMainScriptIfNeeded();

	if (bIsMainRunning)
	{
		Socket->Emit(TEXT("runChildScript"), ScriptRelativePath, [this, Listener](const TArray<TSharedPtr<FJsonValue>>& ResponseArray) {
			int32 ProcessId = ResponseArray[0]->AsNumber();
			RunningChildScripts.Add(ProcessId);

			if (Listener)
			{
				Listener->OnChildScriptBegin(ProcessId);
			}
		});
	}
}

void FNodeCmd::StopMainScript()
{
	if (bIsMainRunning)
	{
		FCULambdaRunnable::RunLambdaOnBackGroundThread([this]
		{
			StopMainScriptSync();
		});
	}
}

void FNodeCmd::StopMainScriptSync()
{
	UE_LOG(LogTemp, Log, TEXT("StopMainScriptSync"));
	if (Socket->bIsConnected)
	{
		UE_LOG(LogTemp, Log, TEXT("Socket->bIsConnected"));

		Socket->Emit(TEXT("stopMainScript"), TEXT("ForceStop"));
		Socket->SyncDisconnect();
	}
	bShouldMainRun = false;
}

void FNodeCmd::StopChildScript(int32 ProcessId)
{
	if (bIsMainRunning && Socket->bIsConnected)
	{
		Socket->Emit(TEXT("stopChildScript"), (double)ProcessId);
	}
}

bool FNodeCmd::IsMainScriptRunning()
{
	return bIsMainRunning;
}

void FNodeCmd::ResolveNpmDependencies(const FString& ProjectRootRelativePath, TFunction<void(bool, const FString&)> ResultCallback)
{
	if (bIsMainRunning && Socket->bIsConnected)
	{
		Socket->Emit(TEXT("npmInstall"), ProjectRootRelativePath, [ResultCallback](const TArray<TSharedPtr<FJsonValue>>& ResponseArray)
		{
			//Response will be { isInstalled: true } or { err: string }
			auto Response = ResponseArray[0]->AsObject();
			bool bIsInstalled = false;

			auto Err = Response->TryGetField("err");
			FString ErrorMsg = TEXT("No error.");
			if(Err.IsValid())
			{
				ErrorMsg = Err->AsString();
			}
			else 
			{
				bIsInstalled = Response->GetBoolField(TEXT("isInstalled"));
			}
			ResultCallback(bIsInstalled, ErrorMsg);
		});
	}
}

PROCESS_INFORMATION CreateChildProcess(const FString& Process, const FString& Commands, const FString& InProcessDirectory) {

	//largely from https://stackoverflow.com/questions/14147138/capture-output-of-spawned-process-to-string
	//pipe architecture no longer used in favor of socket.io pipe for ipc
	
	PROCESS_INFORMATION piProcInfo;
	STARTUPINFO siStartInfo;
	bool bSuccess = 0;

	// Set up members of the PROCESS_INFORMATION structure. 
	ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

	// Set up members of the STARTUPINFO structure. 
	// This structure specifies the STDERR and STDOUT handles for redirection.
	ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
	siStartInfo.cb = sizeof(STARTUPINFO);
	siStartInfo.hStdError = g_hChildStd_ERR_Wr;
	siStartInfo.hStdOutput = g_hChildStd_OUT_Wr;
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	FString ProcessPath = InProcessDirectory + TEXT("/") + Process;
	FString Command = Process + TEXT(" ") + Commands;

	// Create the child process. 
	bSuccess = CreateProcessW(*ProcessPath,
		(LPWSTR)*Command,		// command line 
		NULL,			// process security attributes 
		NULL,			// primary thread security attributes 
		1,				// handles are inherited 
		CREATE_NO_WINDOW,     // creation flags, no window
		NULL,				  // use parent's environment 
		*InProcessDirectory,       // use parent's current directory 
		&siStartInfo,  // STARTUPINFO pointer 
		&piProcInfo);  // receives PROCESS_INFORMATION
	CloseHandle(g_hChildStd_ERR_Wr);
	CloseHandle(g_hChildStd_OUT_Wr);
	// If an error occurs, exit the application. 
	if (!bSuccess) 
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to launch process at %s with %s. Likely due to plugin path error, compare actual path to expected one printed here."), *ProcessPath, *Command);
	}

	return piProcInfo;
}

