// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "NodeJs.h"
#include "CommandLine.h"
#include "Core.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FNodeJsModule"

void FNodeJsModule::StartupModule()
{

}

void FNodeJsModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FNodeJsModule, NodeJs)
