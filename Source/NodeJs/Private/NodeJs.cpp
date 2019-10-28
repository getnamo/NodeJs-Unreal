// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "NodeJs.h"
#include "CommandLine.h"
#include "Core.h"
#include "Modules/ModuleManager.h"
#include "CULambdaRunnable.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FNodeJsModule"

class FNodeJsModule : public INodeJsModule
{
public:

	virtual TSharedPtr<FNodeCmd> NewValidNativePointer() override;
	virtual void ReleaseNativePointer(TSharedPtr<FNodeCmd> PointerToRelease) override;
	virtual TSharedPtr<FNodeCmd> ValidSharedNativePointer(FString SharedId) override;


	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TArray<TSharedPtr<FNodeCmd>> PluginNativePointers;

	//Shared pointers, these will typically be alive past game world lifecycles
	TMap<FString, TSharedPtr<FNodeCmd>> SharedNativePointers;
	TSet<TSharedPtr<FNodeCmd>> AllSharedPtrs;	//reverse lookup

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
	//Remove shared ptr references if any
	if (AllSharedPtrs.Contains(PointerToRelease))
	{
		AllSharedPtrs.Remove(PointerToRelease);
		for (auto& Pair : SharedNativePointers)
		{
			if (Pair.Value == PointerToRelease)
			{
				SharedNativePointers.Remove(Pair.Key);
				break;
			}
		}
	}

	FCULambdaRunnable::RunLambdaOnBackGroundThread([PointerToRelease, this]
	{
		if (PointerToRelease.IsValid())
		{
			//Ensure only one thread at a time removes from array 
			{
				FScopeLock Lock(&DeleteSection);
				if (PointerToRelease->IsMainScriptRunning())
				{
					PointerToRelease->StopMainScript();
				}
				PluginNativePointers.Remove(PointerToRelease);
			}
			while (PointerToRelease->IsMainScriptRunning())
			{
				FPlatformProcess::Sleep(0.01f);
			}

			//Update our active status
			bHasActiveNativePointers = PluginNativePointers.Num() > 0;
		}
	});
}

TSharedPtr<FNodeCmd> FNodeJsModule::ValidSharedNativePointer(FString SharedId)
{
	//Found key? return it
	if (SharedNativePointers.Contains(SharedId))
	{
		return SharedNativePointers[SharedId];
	}
	//Otherwise request a new id and return it
	else
	{
		TSharedPtr<FNodeCmd> NewNativePtr = NewValidNativePointer();
		SharedNativePointers.Add(SharedId, NewNativePtr);
		AllSharedPtrs.Add(NewNativePtr);
		return NewNativePtr;
	}
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
	float SleepInc = 0.01f;
	/*while (bHasActiveNativePointers)
	{
		FPlatformProcess::Sleep(SleepInc);
		Elapsed += SleepInc;

		//if it takes more than 5 seconds, just quit
		if (Elapsed > 5.f)
		{
			UE_LOG(LogTemp, Warning, TEXT("FNodeJsModule::ShutdownModule force quit due to long wait to quit."));
			break;
		}
	}*/
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FNodeJsModule, NodeJs)
