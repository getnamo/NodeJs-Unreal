#include "NodeCmd.h"
#include "CoreMinimal.h"

//Windows Includes
#include "PreWindowsApi.h"
#include "AllowWindowsPlatformTypes.h"
//#include "AllowWindowsPlatformAtomics.h"

#include "Windows.h"
#pragma comment(lib, "User32.lib")

//#include "HideWindowsPlatformAtomics.h"
#include "HideWindowsPlatformTypes.h"
#include "PostWindowsApi.h"
//End Windows


void FNodeCmd::RunScript(const FString& ScriptPath, const FString& Args)
{
	FString NodeDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + "Plugins/nodejs-ue4/Source/ThirdParty/node");
	FString NodePath = NodeDirectory + TEXT("/node.exe");
	STARTUPINFO Si;
	PROCESS_INFORMATION Pi;

	ZeroMemory(&Si, sizeof(Si));
	Si.cb = sizeof(Si);
	ZeroMemory(&Pi, sizeof(Pi));

	CreateProcess(*NodePath,   // Path to app
		NULL,			// Command line //(LPWSTR)TCHAR_TO_UTF8(*NodePath)
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
	CloseHandle(Pi.hThread);
}

