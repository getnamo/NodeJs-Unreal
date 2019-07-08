// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FNodeCmd;

class INodeJsModule : public IModuleInterface
{
public:

	/**
	* Singleton - like access to this module's interface.  This is just for convenience!
	* Beware of calling this during the shutdown phase, though.Your module might have been unloaded already.
	*
	* @return Returns singleton instance, loading the module on demand if needed
	*/
	static inline INodeJsModule& Get()
	{
		return FModuleManager::LoadModuleChecked< INodeJsModule >("NodeJs");
	}

	/**
	* Checks to see if this module is loaded and ready.  It is only valid to call Get() if IsAvailable() returns true.
	*
	* @return True if the module is loaded and ready to use
	*/
	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("NodeJs");
	}

	//Plugin scoped memory management
	virtual TSharedPtr<FNodeCmd> NewValidNativePointer() { return nullptr; };
	virtual TSharedPtr<FNodeCmd> ValidSharedNativePointer(FString SharedId) { return nullptr; };
	virtual void ReleaseNativePointer(TSharedPtr<FNodeCmd> PointerToRelease) {};
};
