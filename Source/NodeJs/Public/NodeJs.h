// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FNodeCmd;

class INodeJsModule : public IModuleInterface
{
public:

private:

	virtual TSharedPtr<FNodeCmd> NewValidNativePointer() { return nullptr; };
	virtual void ReleaseNativePointer(TSharedPtr<FNodeCmd> PointerToRelease) {};
};
