#pragma once

#include "CoreMinimal.h"
#include "Runtime/Core/Public/Async/Async.h"

//Windows Includes
#include "PreWindowsApi.h"
#include "AllowWindowsPlatformTypes.h"
//#include "AllowWindowsPlatformAtomics.h"

#include "Windows.h"
#include <tchar.h>
#include <stdio.h> 
#include <strsafe.h>
#include <string>
#include <iostream>
#pragma comment(lib, "User32.lib")

//#include "HideWindowsPlatformAtomics.h"
#include "HideWindowsPlatformTypes.h"
#include "PostWindowsApi.h"
//End Windows

#include "SocketIONative.h"
#include "Runtime/Core/Public/HAL/ThreadSafeBool.h"


class COMMANDLINE_API FNodeCmd
{
public:
	FNodeCmd();
	~FNodeCmd();
	
	//Start a node.js script
	void RunChildScript(const FString& ScriptRelativePath);

	//Todo: add std-emit and on event binds for this pipe
	void Emit(const FString& Data);

	//Forcefully stop the script
	void StopMainScript();
	void StopChildScript();

	bool IsMainScriptRunning();

	void StartupMainScriptIfNeeded();

	TFunction<void(const FString& LogMsg)> OnConsoleLog;
	TFunction<void(const FString& ScriptRelativePath)> OnMainScriptEnd;
	TFunction<void(const FString& ScriptRelativePath)> OnChildScriptEnd;
	TFunction<void(const FString& ScriptRelativePath, const FString& ScriptErrorMessage)> OnScriptError;

	TSharedPtr<FSocketIONative> Socket;

	FString DefaultMainScript;
	int32 DefaultPort;

private:
	//start wrapper script
	bool RunMainScript(const FString& ScriptRelativePath, int32 Port = 3000);

	HANDLE g_hChildStd_OUT_Rd;
	HANDLE g_hChildStd_OUT_Wr;
	HANDLE g_hChildStd_ERR_Rd;
	HANDLE g_hChildStd_ERR_Wr;
	HANDLE g_hChildStd_IN_Rd;

	PROCESS_INFORMATION CreateChildProcess(const FString& Process, const FString& Commands);
	void ReadFromPipe();
	void WriteToPipe(FString Data);


	FString ProcessDirectory;

	static FThreadSafeBool bShouldMainRun;
	static FThreadSafeBool bIsMainRunning;
};