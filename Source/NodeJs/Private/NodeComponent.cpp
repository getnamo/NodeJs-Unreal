
#include "NodeComponent.h"
#include "NodeJs.h"
#include "SIOMessageConvert.h"
#include "CULambdaRunnable.h"
#include "Runtime/Core/Public/HAL/PlatformFilemanager.h"
#include "Runtime/Core/Public/Misc/FileHelper.h"
#include "Runtime/Core/Public/Misc/Paths.h"

// Sets default values for this component's properties
UNodeComponent::UNodeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}


// Called when the game starts
void UNodeComponent::BeginPlay()
{

}


void UNodeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
}


// Called every frame
void UNodeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

