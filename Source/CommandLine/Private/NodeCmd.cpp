#include "NodeCmd.h"
#include "SIOJConvert.h"


#define BUFSIZE 4096

//largely from https://stackoverflow.com/questions/14147138/capture-output-of-spawned-process-to-string


int FNodeCmd::TestPipe() 
{
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

		PROCESS_INFORMATION piProcInfo = CreateChildProcess();
		
		//ReadFromPipe();
		/*while (bShouldRun)
		{
			FPlatformProcess::Sleep(0.1f);
			ReadFromPipe();
		}*/
		
	};

	Async(EAsyncExecution::Thread, Task);
	return 0;
}

// Create a child process that uses the previously created pipes
//  for STDERR and STDOUT.
PROCESS_INFORMATION FNodeCmd::CreateChildProcess() {
	// Set the text I want to run
	//char szCmdline[] = "test --log_level=all --report_level=detailed";
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

	FString NodeDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + "Plugins/nodejs-ue4/Source/ThirdParty/node");
	FString NodePath = NodeDirectory + TEXT("/node.exe");
	FString Command = TEXT("node.exe hello.js");
	LPTSTR cmdArgs = L"node.exe hello.js";

	// Create the child process. 
	bSuccess = CreateProcess(*NodePath,
		cmdArgs,		// command line 
		NULL,			// process security attributes 
		NULL,			// primary thread security attributes 
		1,				// handles are inherited 
		CREATE_NO_WINDOW,     // creation flags, no window
		NULL,				  // use parent's environment 
		*NodeDirectory,       // use parent's current directory 
		&siStartInfo,  // STARTUPINFO pointer 
		&piProcInfo);  // receives PROCESS_INFORMATION
	//CloseHandle(g_hChildStd_ERR_Wr);
	//CloseHandle(g_hChildStd_OUT_Wr);
	// If an error occurs, exit the application. 
	if (!bSuccess) {
		exit(1);
	}
	return piProcInfo;
}

// Read output from the child process's pipe for STDOUT
// and write to the parent process's pipe for STDOUT. 
// Stop when there is no more data. 
void FNodeCmd::ReadFromPipe() {
	DWORD dwRead;
	CHAR chBuf[BUFSIZE];
	bool bSuccess = false;
	std::string out = "", err = "";
	for (;;) {
		bSuccess = ReadFile(g_hChildStd_OUT_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0) break;

		std::string s(chBuf, dwRead);
		out += s;
	}
	dwRead = 0;
	for (;;) {
		bSuccess = ReadFile(g_hChildStd_ERR_Rd, chBuf, BUFSIZE, &dwRead, NULL);
		if (!bSuccess || dwRead == 0) break;

		std::string s(chBuf, dwRead);
		err += s;
	}

	if (out.length() > 0) {
		UE_LOG(LogTemp, Log, TEXT("out: %s"), *FString(UTF8_TO_TCHAR(out.c_str())));
	}

	//std::cout << "stdout:" << out << std::endl;
	//std::cout << "stderr:" << err << std::endl;
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
}

void FNodeCmd::RunScript(const FString& ScriptPath, const FString& Args)
{
	UE_LOG(LogTemp, Log, TEXT("TestStart"));
	TestPipe();
	UE_LOG(LogTemp, Log, TEXT("TestEnd"));

	/*
	Basic tests
	FString NodeDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + "Plugins/nodejs-ue4/Source/ThirdParty/node");
	FString NodePath = NodeDirectory + TEXT("/node.exe");
	FString Command = TEXT("node.exe hello.js");
	LPTSTR cmdArgs = L"node.exe hello.js";

	STARTUPINFO Si;
	PROCESS_INFORMATION Pi;

	

	ZeroMemory(&Si, sizeof(Si));
	Si.cb = sizeof(Si);
	ZeroMemory(&Pi, sizeof(Pi));

	CreateProcess(*NodePath,   // Path to app
		cmdArgs,			// Command line //(LPWSTR)TCHAR_TO_UTF8(*NodePath)
		NULL,           // Process handle not inheritable
		NULL,           // Thread handle not inheritable
		false,          // Set handle inheritance to FALSE
		0,              // No creation flags
		NULL,           // Use parent's environment block
		*NodeDirectory,	// Use parent's starting directory
		&Si,            // Pointer to STARTUPINFO structure
		&Pi);           // Pointer to PROCESS_INFORMATION structure

	WaitForSingleObject(Pi.hProcess, INFINITE);

	// Close process and thread handles. 
	CloseHandle(Pi.hProcess);
	CloseHandle(Pi.hThread);*/
}

