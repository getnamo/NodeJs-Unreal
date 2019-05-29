#include "NodeCmd.h"
#include "CoreMinimal.h"

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

#define BUFSIZE 4096

//largely from https://stackoverflow.com/questions/14147138/capture-output-of-spawned-process-to-string
//TODO: working bi-directional pipe
HANDLE g_hChildStd_OUT_Rd = NULL;
HANDLE g_hChildStd_OUT_Wr = NULL;
HANDLE g_hChildStd_ERR_Rd = NULL;
HANDLE g_hChildStd_ERR_Wr = NULL;

PROCESS_INFORMATION CreateChildProcess(void);
void ReadFromPipe(PROCESS_INFORMATION);

int TestPipe() {
	SECURITY_ATTRIBUTES sa;
	printf("\n->Start of parent execution.\n");
	// Set the bInheritHandle flag so pipe handles are inherited. 
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = 1;
	sa.lpSecurityDescriptor = NULL;
	// Create a pipe for the child process's STDERR. 
	if (!CreatePipe(&g_hChildStd_ERR_Rd, &g_hChildStd_ERR_Wr, &sa, 0)) {
		return 1;
	}
	// Ensure the read handle to the pipe for STDERR is not inherited.
	if (!SetHandleInformation(g_hChildStd_ERR_Rd, HANDLE_FLAG_INHERIT, 0)) {
		return 1;
	}
	// Create a pipe for the child process's STDOUT. 
	if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &sa, 0)) {
		return 1;
	}
	// Ensure the read handle to the pipe for STDOUT is not inherited
	if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
		return 1;
	}
	// Create the child process. 
	PROCESS_INFORMATION piProcInfo = CreateChildProcess();

	// Read from pipe that is the standard output for child process. 
	ReadFromPipe(piProcInfo);
	/*ReadFromPipe(piProcInfo);
	ReadFromPipe(piProcInfo);*/


	// The remaining open handles are cleaned up when this process terminates. 
	// To avoid resource leaks in a larger application, 
	//   close handles explicitly.
	return 0;
}

// Create a child process that uses the previously created pipes
//  for STDERR and STDOUT.
PROCESS_INFORMATION CreateChildProcess() {
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
	CloseHandle(g_hChildStd_ERR_Wr);
	CloseHandle(g_hChildStd_OUT_Wr);
	// If an error occurs, exit the application. 
	if (!bSuccess) {
		exit(1);
	}
	return piProcInfo;
}

// Read output from the child process's pipe for STDOUT
// and write to the parent process's pipe for STDOUT. 
// Stop when there is no more data. 
void ReadFromPipe(PROCESS_INFORMATION piProcInfo) {
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
	UE_LOG(LogTemp, Log, TEXT("out: %s"), *FString(UTF8_TO_TCHAR(out.c_str())) );

	std::cout << "stdout:" << out << std::endl;
	std::cout << "stderr:" << err << std::endl;
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

