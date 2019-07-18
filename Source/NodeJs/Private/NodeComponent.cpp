
#include "NodeComponent.h"
#include "NodeJs.h"
#include "SIOMessageConvert.h"

// Sets default values for this component's properties
UNodeComponent::UNodeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bStartMainScriptIfNeededOnBeginPlay = true;

	bRunDefaultScriptOnBeginPlay = true;
	bReloadOnChange = true;
	bStopMainScriptOnNoListeners = false;
	DefaultScriptPath = TEXT("child.js");
	bScriptIsRunning = false;
	ScriptId = -1;

	Cmd = INodeJsModule::Get().ValidSharedNativePointer(TEXT("main"));
	Listener = MakeShareable(new FNodeEventListener());
}


// Called when the game starts
void UNodeComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bStartMainScriptIfNeededOnBeginPlay)
	{
		//Start the parent script which hosts all scripts
		LinkAndStartWrapperScript();
		if (bRunDefaultScriptOnBeginPlay)
		{
			RunScript(DefaultScriptPath);
		}
	}
}


void UNodeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Cmd->bShouldStopMainScriptOnNoListeners = bStopMainScriptOnNoListeners;
	if (bScriptIsRunning)
	{
		Cmd->StopChildScript(ScriptId);

		//we won't receive the network signal in time so call the stop script event manually
		Listener->OnChildScriptEnd(ScriptId);
	}

	Cmd->RemoveEventListener(Listener);
}

void UNodeComponent::LinkAndStartWrapperScript()
{
	Listener->OnChildScriptEnd = [this](int32 ProcessId)
	{
		OnScriptEnd.Broadcast(DefaultScriptPath);
		bScriptIsRunning = false;
		UnbindAllScriptEvents();
		Listener->ProcessId = -1;
		ScriptId = -1;
	};
	Listener->OnScriptError = [this](const FString& ScriptPath, const FString& ErrorMessage)
	{
		OnScriptError.Broadcast(ScriptPath, ErrorMessage);
	};
	Listener->OnScriptConsoleLog = [this](const FString& ConsoleMessage)
	{
		OnConsoleLog.Broadcast(ConsoleMessage);
	};
	Listener->OnChildScriptBegin = [this](int32 ProcessId)
	{
		ScriptId = ProcessId;
		Listener->ProcessId = ScriptId;
		OnScriptBegin.Broadcast(ProcessId);
		bScriptIsRunning = true;

		//run any delayed binds due to process not running
		for (auto& BindEventFunction : DelayedBindEvents)
		{
			BindEventFunction();
		}
	};

	Cmd->AddEventListener(Listener);
}

void UNodeComponent::RunScript(const FString& ScriptRelativePath)
{
	DefaultScriptPath = ScriptRelativePath;

	if (!bScriptIsRunning) 
	{
		Cmd->RunChildScript(ScriptRelativePath, Listener.Get());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("NodeComponent: Script did not start, already running."));
	}
}

void UNodeComponent::StopScript()
{
	Cmd->StopChildScript(ScriptId);
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

	Cmd->Socket->Emit(FullEventName(EventName), JsonMessage, nullptr, Namespace);
}

void UNodeComponent::EmitWithCallBack(const FString& EventName, USIOJsonValue* Message /*= nullptr*/, const FString& CallbackFunctionName /*= FString("")*/, UObject* Target /*= nullptr*/, const FString& Namespace /*= FString(TEXT("/"))*/)
{
	if (!CallbackFunctionName.IsEmpty())
	{
		if (Target == nullptr)
		{
			Target = GetOwner();
		}

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

		Cmd->Socket->Emit(FullEventName(EventName), JsonMessage, [&, Target, CallbackFunctionName, this](const TArray<TSharedPtr<FJsonValue>>& Response)
		{
			CallBPFunctionWithResponse(Target, CallbackFunctionName, Response);
		}, Namespace);
	}
	else
	{
		Cmd->Socket->Emit(EventName, Message->GetRootValue(), nullptr, Namespace);
	}
}

void UNodeComponent::BindEvent(const FString& EventName, const FString& Namespace /*= FString(TEXT("/"))*/)
{
	TFunction<void()> BindFunction = [EventName, Namespace, this]
	{
		//format: pid@EventName, needs to be assessed when we have a process/script id
		Cmd->Socket->OnRawEvent(FullEventName(EventName), [&, EventName](const FString& Event, const sio::message::ptr& RawMessage) {
		USIOJsonValue* NewValue = NewObject<USIOJsonValue>();
		auto Value = USIOMessageConvert::ToJsonValue(RawMessage);
		NewValue->SetRootValue(Value);
		OnEvent.Broadcast(EventName, NewValue);
		}, Namespace);

		BoundEventNames.AddUnique(FullEventName(EventName));
	};

	if (bScriptIsRunning)
	{
		BindFunction();
	}
	else
	{
		//delay binding until our script runs and we have a pid
		DelayedBindEvents.Add(BindFunction);
	}
	
}

bool UNodeComponent::CallBPFunctionWithResponse(UObject* Target, const FString& FunctionName, TArray<TSharedPtr<FJsonValue>> Response)
{
	if (!Target->IsValidLowLevel())
	{
		UE_LOG(LogTemp, Warning, TEXT("CallFunctionByNameWithArguments: Target not found for '%s'"), *FunctionName);
		return false;
	}

	UFunction* Function = Target->FindFunction(FName(*FunctionName));
	if (nullptr == Function)
	{
		UE_LOG(LogTemp, Warning, TEXT("CallFunctionByNameWithArguments: Function not found '%s'"), *FunctionName);
		return false;
	}

	//Check function signature
	TFieldIterator<UProperty> Iterator(Function);

	TArray<UProperty*> Properties;
	while (Iterator && (Iterator->PropertyFlags & CPF_Parm))
	{
		UProperty* Prop = *Iterator;
		Properties.Add(Prop);
		++Iterator;
	}

	auto ResponseJsonValue = USIOJConvert::ToSIOJsonValue(Response);

	bool bResponseNumNotZero = Response.Num() > 0;
	bool bNoFunctionParams = Properties.Num() == 0;
	bool bNullResponse = ResponseJsonValue->IsNull();

	if (bNullResponse && bNoFunctionParams)
	{
		Target->ProcessEvent(Function, nullptr);
		return true;
	}
	else if (bResponseNumNotZero)
	{
		//function has too few params
		if (bNoFunctionParams)
		{
			UE_LOG(LogTemp, Warning, TEXT("CallFunctionByNameWithArguments: Function '%s' has too few parameters, callback parameters ignored : <%s>"), *FunctionName, *ResponseJsonValue->EncodeJson());
			Target->ProcessEvent(Function, nullptr);
			return true;
		}
		struct FDynamicArgs
		{
			void* Arg01 = nullptr;
			USIOJsonValue* Arg02 = nullptr;
		};
		//create the container
		FDynamicArgs Args = FDynamicArgs();

		//add the full response array as second param
		Args.Arg02 = ResponseJsonValue;
		const FString& FirstParam = Properties[0]->GetCPPType();
		auto FirstFJsonValue = Response[0];

		//Is first param...
		//SIOJsonValue?
		if (FirstParam.Equals("USIOJsonValue*"))
		{
			//convenience wrapper, response is a single object
			USIOJsonValue* Value = NewObject<USIOJsonValue>();
			Value->SetRootValue(FirstFJsonValue);
			Args.Arg01 = Value;
			Target->ProcessEvent(Function, &Args);
			return true;
		}
		//SIOJsonObject?
		else if (FirstParam.Equals("USIOJsonObject*"))
		{
			//convenience wrapper, response is a single object
			USIOJsonObject* ObjectValue = NewObject<USIOJsonObject>();
			ObjectValue->SetRootObject(FirstFJsonValue->AsObject());
			Args.Arg01 = ObjectValue;
			Target->ProcessEvent(Function, &Args);
			return true;
		}
		//String?
		else if (FirstParam.Equals("FString"))
		{
			FString	StringValue = USIOJConvert::ToJsonString(FirstFJsonValue);

			Target->ProcessEvent(Function, &StringValue);
			return true;
		}
		//Float?
		else if (FirstParam.Equals("float"))
		{
			float NumberValue = (float)FirstFJsonValue->AsNumber();
			Target->ProcessEvent(Function, &NumberValue);
			return true;
		}
		//Int?
		else if (FirstParam.Equals("int32"))
		{
			int NumberValue = (int)FirstFJsonValue->AsNumber();
			Target->ProcessEvent(Function, &NumberValue);
			return true;
		}
		//bool?
		else if (FirstParam.Equals("bool"))
		{
			bool BoolValue = FirstFJsonValue->AsBool();
			Target->ProcessEvent(Function, &BoolValue);
			return true;
		}
		//array?
		else if (FirstParam.Equals("TArray"))
		{
			UArrayProperty* ArrayProp = Cast<UArrayProperty>(Properties[0]);

			FString Inner;
			ArrayProp->GetCPPMacroType(Inner);

			//byte array is the only supported version
			if (Inner.Equals("uint8"))
			{
				TArray<uint8> Bytes = ResponseJsonValue->AsArray()[0]->AsBinary();
				Target->ProcessEvent(Function, &Bytes);
				return true;
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("CallFunctionByNameWithArguments: Function '%s' signature not supported expected <%s>"), *FunctionName, *ResponseJsonValue->EncodeJson());
	return false;
}

FString UNodeComponent::FullEventName(const FString& EventName)
{
	return FString::Printf(TEXT("%d@%s"), ScriptId, *EventName);
}

void UNodeComponent::UnbindAllScriptEvents()
{
	if (Cmd->IsMainScriptRunning())
	{
		for (const FString& EventName : BoundEventNames)
		{
			Cmd->Socket->UnbindEvent(EventName);
		}
	}
}

// Called every frame
void UNodeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

