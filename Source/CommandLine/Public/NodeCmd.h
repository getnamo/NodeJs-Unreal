#pragma once

#include "CoreMinimal.h"
#include "Runtime/Core/Public/Async/Async.h"
#include "SocketIONative.h"
#include "Runtime/Core/Public/HAL/ThreadSafeBool.h"


class COMMANDLINE_API FNodeCmd
{
public:
	FNodeCmd();
	~FNodeCmd();
	
	//Start a node.js script
	void RunChildScript(const FString& ScriptRelativePath);

	//Forcefully stop the script
	void StopMainScript();
	void StopChildScript();

	bool IsMainScriptRunning();

	void StartupMainScriptIfNeeded();

	TFunction<void(const FString& LogMsg)> OnConsoleLog;
	TFunction<void(const FString& ScriptRelativePath)> OnMainScriptEnd;
	TFunction<void(int32 ProcessId)> OnChildScriptBegin;
	TFunction<void(const FString& ScriptRelativePath)> OnChildScriptEnd;
	TFunction<void(const FString& ScriptRelativePath, const FString& ScriptErrorMessage)> OnScriptError;

	TSharedPtr<FSocketIONative> Socket;

	FString DefaultMainScript;
	int32 DefaultPort;
	int32 ProcessId;

private:
	//start wrapper script
	bool RunMainScript(FString ScriptRelativePath, int32 Port = 3000);

	FString ProcessDirectory;
	FString PluginContentRelativePath;

	FThreadSafeBool bShouldMainRun;
	FThreadSafeBool bIsMainRunning;
};