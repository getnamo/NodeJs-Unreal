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

//todo: rehide implementation

class COMMANDLINE_API FNodeCmd
{
public:
	FNodeCmd();
	FThreadSafeBool bShouldRun;

	void RunScript(const FString& ScriptRelativePath);

private:
	HANDLE g_hChildStd_OUT_Rd;
	HANDLE g_hChildStd_OUT_Wr;
	HANDLE g_hChildStd_ERR_Rd;
	HANDLE g_hChildStd_ERR_Wr;
	HANDLE g_hChildStd_IN_Rd;

	PROCESS_INFORMATION CreateChildProcess(const FString& Process, const FString& Commands);
	void ReadFromPipe();
	void WriteToPipe(FString Data);

	FSocketIONative Socket;
	FString ProcessDirectory;
};