// Copyright getnamo. NodeJs-Unreal v2.1.0

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NodeFrameCodec.h"
#include "NodeComponent.h"
#include "NodeJsSubsystem.generated.h"

class FSubProcessHandler;

/**
 * Owns the single node process shared by every UNodeComponent in this game instance
 * that sets NodeJsProcessParams.bShareMainProcess. Components are routed by owner id,
 * so their scripts, events and logs stay separate. The process starts with the first
 * component that needs it and lives until the game instance shuts down.
 */
UCLASS()
class NODEJS_API UNodeJsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	void Register(UNodeComponent* Component);

	//Stops the component's scripts; the process keeps running for the others.
	void Unregister(UNodeComponent* Component);

	//Starts the shared process with these params if it isn't running (first caller wins).
	void EnsureStarted(const FNodeJsProcessParams& Params);

	//Sends a frame, queueing it until the process is up.
	void SendFrame(const TArray<uint8>& Frame);

	bool IsProcessRunning() const { return bProcessRunning; }

	int32 NumRegisteredComponents() const { return Components.Num(); }

private:
	void OnProcessBegin(bool bStartSucceeded);
	void OnProcessEnd();
	void RouteFrame(uint8 Type, const TSharedPtr<FJsonObject>& Header, const FString& RawHeader, const TArray<uint8>& Binary);
	void ShutdownProcess();

	TSharedPtr<FSubProcessHandler> Handler;
	TSharedPtr<FNodeBridgeState, ESPMode::ThreadSafe> Bridge;
	TMap<FString, TWeakObjectPtr<UNodeComponent>> Components;
	TArray<TArray<uint8>> PendingFrames;
	FNodeJsProcessParams ProcessParams;
	bool bProcessRunning = false;
	bool bStartRequested = false;

	//Bumped per start/shutdown; callbacks from an older process are ignored.
	uint32 Generation = 0;
};
