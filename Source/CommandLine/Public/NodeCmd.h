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

	FString ProcessDirectory;

	FThreadSafeBool bShouldMainRun;
	FThreadSafeBool bIsMainRunning;
};