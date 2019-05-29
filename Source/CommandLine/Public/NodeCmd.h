#pragma once

class COMMANDLINE_API FNodeCmd
{
public:
	void RunScript(const FString& ScriptPath, const FString& Args = TEXT(""));
};