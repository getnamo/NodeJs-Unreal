
#include "NodeComponent.h"
#include "SIOMessageConvert.h"

void UNodeComponent::RunScript(const FString& ScriptRelativePath)
{
	//for now hardwire port
	Cmd->RunScript(ScriptRelativePath, 3000);
}

void UNodeComponent::Emit(const FString& EventName, USIOJsonValue* Message /*= nullptr*/, const FString& Namespace /*= FString(TEXT("/"))*/)
{
	//Set the message is not null
	TSharedPtr<FJsonValue> JsonMessage = nullptr;
	if (Message != nullptr)
	{
		JsonMessage = Message->GetRootValue();
	}
	else
	{
		JsonMessage = MakeShareable(new FJsonValueNull);
	}

	Cmd->Socket->Emit(EventName, JsonMessage, nullptr, Namespace);
}

void UNodeComponent::BindEvent(const FString& EventName, const FString& Namespace /*= FString(TEXT("/"))*/)
{
	Cmd->Socket->OnRawEvent(EventName, [&](const FString& Event, const sio::message::ptr& RawMessage) {
		USIOJsonValue* NewValue = NewObject<USIOJsonValue>();
		auto Value = USIOMessageConvert::ToJsonValue(RawMessage);
		NewValue->SetRootValue(Value);
		OnEvent.Broadcast(Event, NewValue);

	}, Namespace);
}

// Sets default values for this component's properties
UNodeComponent::UNodeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	bRunDefaultScriptOnBeginPlay = false;
	DefaultScript = TEXT("Testbed.js");

	Cmd = MakeShareable(new FNodeCmd);
}


// Called when the game starts
void UNodeComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bRunDefaultScriptOnBeginPlay)
	{
		RunScript(DefaultScript);
	}
}


void UNodeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Cmd->IsScriptRunning())
	{
		Cmd->StopScript();
	}
}

// Called every frame
void UNodeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

