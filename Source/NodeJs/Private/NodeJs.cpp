// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "NodeJs.h"
#include "CommandLine.h"
#include "Core.h"
#include "Modules/ModuleManager.h"
#include "LambdaRunnable.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FNodeJsModule"

class FNodeJsModule : public INodeJsModule
{
public:

	virtual TSharedPtr<FNodeCmd> NewValidNativePointer() override;
	virtual void ReleaseNativePointer(TSharedPtr<FNodeCmd> PointerToRelease) override;

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TArray<TSharedPtr<FNodeCmd>> PluginNativePointers;
	FThreadSafeBool bHasActiveNativePointers;
	FCriticalSection DeleteSection;
};

TSharedPtr<FNodeCmd> FNodeJsModule::NewValidNativePointer()
{
	TSharedPtr<FNodeCmd> NewPointer = MakeShareable(new FNodeCmd);
	PluginNativePointers.Add(NewPointer);
	bHasActiveNativePointers = true;
	return NewPointer;
}

void FNodeJsModule::ReleaseNativePointer(TSharedPtr<FNodeCmd> PointerToRelease)
{
	FLambdaRunnable::RunLambdaOnBackGroundThreadPool([PointerToRelease, this]
	{
		if (PointerToRelease.IsValid())
		{
			//Ensure only one thread at a time removes from array 
			{
				FScopeLock Lock(&DeleteSection);
				PluginNativePointers.Remove(PointerToRelease);
			}
			//Disconnect, this can happen simultaneously
			if (PointerToRelease->Socket && PointerToRelease->Socket->bIsConnected)
			{
				PointerToRelease->Socket->SyncDisconnect();
			}

			//Update our active status
			bHasActiveNativePointers = PluginNativePointers.Num() > 0;
		}
	});
}

void FNodeJsModule::StartupModule()
{
	PluginNativePointers.Empty();
}

void FNodeJsModule::ShutdownModule()
{
	auto AllActivePointers = PluginNativePointers;
	for (auto& Pointer : AllActivePointers)
	{
		ReleaseNativePointer(Pointer);
	}
	AllActivePointers.Empty();

	float Elapsed = 0.f;
	while (bHasActiveNativePointers)
	{
		FPlatformProcess::Sleep(0.01f);
		Elapsed += 0.01f;

		//if it takes more than 5 seconds, just quit
		if (Elapsed > 5.f)
		{
			UE_LOG(LogTemp, Warning, TEXT("FNodeJsModule::ShutdownModule force quit due to long wait to quit."));
			break;
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FNodeJsModule, NodeJs)
