// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "CommandLine.h"
#include "Core.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"

//#include "NodeCmd.h"

#define LOCTEXT_NAMESPACE "FCommandLine"

void FCommandLineModule::StartupModule()
{

}

void FCommandLineModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FCommandLineModule, CommandLine)
