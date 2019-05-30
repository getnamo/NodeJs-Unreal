#include "NodeCmd.h"
#include "SIOJConvert.h"


#define BUFSIZE 4096

//largely from https://stackoverflow.com/questions/14147138/capture-output-of-spawned-process-to-string

PROCESS_INFORMATION FNodeCmd::CreateChildProcess(const FString& Process, const FString& Commands) {
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
	//siStartInfo.hStdInput = g_hChildStd_IN_Rd;
	siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

	FString ProcessPath = ProcessDirectory + TEXT("/") + Process;
	FString Command = Process + TEXT(" ") + Commands;

	// Create the child process. 
	bSuccess = CreateProcessW(*ProcessPath,
		(LPWSTR)*Command,		// command line 
		NULL,			// process security attributes 
		NULL,			// primary thread security attributes 
		1,				// handles are inherited 
		CREATE_NO_WINDOW,     // creation flags, no window
		NULL,				  // use parent's environment 
		*ProcessDirectory,       // use parent's current directory 
		&siStartInfo,  // STARTUPINFO pointer 
		&piProcInfo);  // receives PROCESS_INFORMATION
	CloseHandle(g_hChildStd_ERR_Wr);
	CloseHandle(g_hChildStd_OUT_Wr);
	// If an error occurs, exit the application. 
	if (!bSuccess) {
		exit(1);
	}
	return piProcInfo;
}


void FNodeCmd::ReadFromPipe() {
	DWORD dwRead;
	CHAR chBuf[BUFSIZE];
	bool bSuccess = false;
	std::string out = "", err = "";
	for (;;) 
	{
		bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0) break;

		std::string s(chBuf, dwRead);
		out += s;
	}
	dwRead = 0;
	for (;;) 
	{
		bSuccess = ReadFile(g_hChildStd_ERR_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0) break;

		std::string s(chBuf, dwRead);
		err += s;
	}

	if (out.length() > 0) 
	{
		UE_LOG(LogTemp, Log, TEXT("out: %s"), *FString(UTF8_TO_TCHAR(out.c_str())));
	}
}

void FNodeCmd::WriteToPipe(FString Data)
{
	//Currently broken

	/*DWORD dwWritten;
	CHAR chBuf[BUFSIZE];
	BOOL bSuccess = FALSE;

	for (;;)
	{
		bSuccess = WriteFile(g_hChildStd_OUT_Wr, *Data, Data.Len(), &dwWritten, NULL);
		if (!bSuccess) break;
	}

	// Close the pipe handle so the child process stops reading. 

	if (!CloseHandle(g_hChildStd_IN_Wr))
		ErrorExit(TEXT("StdInWr CloseHandle"));*/
}


FNodeCmd::FNodeCmd()
{
	bShouldRun = false;
	g_hChildStd_OUT_Rd = NULL;
	g_hChildStd_OUT_Wr = NULL;
	g_hChildStd_ERR_Rd = NULL;
	g_hChildStd_ERR_Wr = NULL;
	ProcessDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + "Plugins/nodejs-ue4/Source/ThirdParty/node");
}

void FNodeCmd::RunScript(const FString& ScriptRelativePath)
{
	FString NodeExe = TEXT("node.exe");

	UE_LOG(LogTemp, Log, TEXT("RunScriptStart"));
	Socket.OnConnectedCallback = [&](const FString& InSessionId)
	{
		UE_LOG(LogTemp, Log, TEXT("Connected!"));
	};
	Socket.OnEvent(TEXT("stdout"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		UE_LOG(LogTemp, Log, TEXT("%s"), *USIOJConvert::ToJsonString(Message));
	});
	Socket.OnEvent(TEXT("done"), [&](const FString& Event, const TSharedPtr<FJsonValue>& Message)
	{
		UE_LOG(LogTemp, Log, TEXT("%s"), *USIOJConvert::ToJsonString(Message));

		Socket.MaxReconnectionAttempts = 1;
		Socket.Disconnect();
	});
	Socket.Connect(TEXT("http://localhost:3000"));


	TFunction<void()> Task = [&]
	{
		UE_LOG(LogTemp, Log, TEXT("TestPipe Start"));
		UE_LOG(LogTemp, Log, TEXT("Waiting..."));
		//FPlatformProcess::Sleep(4.0);
		UE_LOG(LogTemp, Log, TEXT("Waited, connecting."));

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

		bShouldRun = true;

		PROCESS_INFORMATION piProcInfo = CreateChildProcess(NodeExe, ScriptRelativePath);

		//ReadFromPipe();
		/*while (bShouldRun)
		{
			FPlatformProcess::Sleep(0.1f);
			ReadFromPipe();
		}*/
	};

	Async(EAsyncExecution::Thread, Task);
	UE_LOG(LogTemp, Log, TEXT("RunScriptEnd"));
}

