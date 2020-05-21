
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
	bStartMainScriptIfNeededOnBeginPlay = true;
	bResolveDependenciesOnScriptModuleError = true;
	bAutoRunOnNpmInstall = true;

	bRunDefaultScriptOnBeginPlay = true;
	bStopMainScriptOnNoListeners = false;
	DefaultScriptPath = TEXT("child.js");
	bScriptIsRunning = false;
	ScriptId = -1;

	bWatchFileOnBeginPlay = false;
	bReloadOnChange = true;
	bIsRestartStop = false;
	bAllowPreBinding = false;
	bBeginPlayScriptHandled = false;

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
	}
}


void UNodeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Cmd->bShouldStopMainScriptOnNoListeners = bStopMainScriptOnNoListeners;
	if (bScriptIsRunning)
	{
		Cmd->StopWatchingScript(DefaultScriptPath);
		Cmd->StopChildScript(ScriptId);

		//we won't receive the network signal in time so call the stop script event manually
		Listener->OnChildScriptEnd(ScriptId);
	}

	Cmd->RemoveEventListener(Listener);
}

void UNodeComponent::LinkAndStartWrapperScript()
{
	Listener->OnMainScriptBegin = [this](const FString& ScriptRelativePath)
	{
		//We wait until after main script started to run our beginplay script
		if (bRunDefaultScriptOnBeginPlay && !bBeginPlayScriptHandled)
		{
			bBeginPlayScriptHandled = true;
			RunDefaultScript();

			//watch scripts?
			if (bWatchFileOnBeginPlay)
			{
				Cmd->WatchScriptForChanges(DefaultScriptPath, [&](const FString& WatchedScriptPath)
					{
						if (bReloadOnChange)
						{
							//Restart
							if (bScriptIsRunning)
							{
								//Stop and re-start script
								bIsRestartStop = true;
								StopScript();
							}
							//Just start
							else
							{
								RunDefaultScript();
							}
						}
						OnScriptChanged.Broadcast(WatchedScriptPath);
					});
			}
		}
	};

	Listener->OnChildScriptEnd = [this](int32 ProcessId)
	{
		OnScriptEnd.Broadcast(DefaultScriptPath);
		bScriptIsRunning = false;
		UnbindAllScriptEvents();
		Listener->ProcessId = -1;
		ScriptId = -1;

		//Is this a restart?
		if (bIsRestartStop)
		{
			bIsRestartStop = false;

			//Delay by 100ms to give time time for shutdown
			FCULambdaRunnable::SetTimeout([this]
			{
				RunDefaultScript();
			}, 0.1f);
			
		}
	};
	Listener->OnScriptError = [this](const FString& ScriptPath, const FString& ErrorMessage)
	{
		//Auto-fix npm dependency error
		if (bResolveDependenciesOnScriptModuleError) 
		{
			const FString ModuleErrorMatch = TEXT("Error: Cannot find module ");
			int32 ModuleErrorStart = ErrorMessage.Find(ModuleErrorMatch) + ModuleErrorMatch.Len();
			//Check our error message for the magic line
			if (ModuleErrorStart != INDEX_NONE)
			{
				FString SubStringError = ErrorMessage.Mid(ModuleErrorStart);
				bool SingleQuote = SubStringError.StartsWith(TEXT("'"));
				bool DoubleQuote = SubStringError.StartsWith(TEXT("\""));
				SubStringError = SubStringError.Mid(1); //left shift one

				bool isLocal = SubStringError.StartsWith(".");

				//but ignore local modules requires
				if (!isLocal)
				{
					int32 EndQuote = INDEX_NONE;
					if (SingleQuote)
					{
						EndQuote = SubStringError.Find(TEXT("'"));
					}
					else
					{
						EndQuote = SubStringError.Find(TEXT("\""));
					}
					if (EndQuote == INDEX_NONE) 
					{
						return;
					}
					FString ModuleName = SubStringError.Left(EndQuote);

					//Ok so far it's a valid external dependency. Check if our package.json can resolve it
					TArray<FString> Packages = PackageDependencies();
					
					if (Packages.Contains(ModuleName))
					{
						UE_LOG(LogTemp, Log, TEXT("package.json contains missing dependency %s, auto-resolving."), *ModuleName);
						//It can resolve it, run the installation
						ResolveNpmDependencies();
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("Your package.json at %s is missing '%s' dependency, cannot auto-resolve."),
							*(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + ProjectRootRelativeScriptFolder())), *ModuleName);
					}
				}
			}
		}
		OnScriptError.Broadcast(ScriptPath, ErrorMessage);
		bScriptIsRunning = false;
	};
	Listener->OnScriptConsoleLog = [this](const FString& ConsoleMessage)
	{
		OnConsoleLog.Broadcast(ConsoleMessage);
	};
	Listener->OnChildScriptBegin = [this](int32 ProcessId)
	{
		ScriptId = ProcessId;
		Listener->ProcessId = ScriptId;
		bScriptIsRunning = true;

		if (bAllowPreBinding)
		{
			//run any delayed binds due to process not running
			for (auto& BindEventFunction : DelayedBindEvents)
			{
				BindEventFunction();
			}
		}

		OnScriptBegin.Broadcast(ProcessId);
	};

	Cmd->AddEventListener(Listener);
}

void UNodeComponent::RunScript(const FString& ScriptRelativePath)
{
	DefaultScriptPath = ScriptRelativePath;

	if (!bScriptIsRunning) 
	{
		Cmd->RunChildScript(DefaultScriptPath, Listener.Get());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("NodeComponent: Script did not start, already running."));
	}
}

void UNodeComponent::RunDefaultScript()
{
	RunScript(DefaultScriptPath);
}

void UNodeComponent::StopScript()
{
	Cmd->StopChildScript(ScriptId);
}

void UNodeComponent::ResolveNpmDependencies()
{
	//expects project root relative folder
	Cmd->ResolveNpmDependencies(ProjectRootRelativeScriptFolder(), [this](bool bIsInstalled, const FString& ErrorMsg)
	{
		UE_LOG(LogTemp, Log, TEXT("package.json dependencies for %s have been installed."), *ProjectRootRelativeScriptFolder());
		OnNpmDependenciesResolved.Broadcast(bIsInstalled, ErrorMsg);

		if (bAutoRunOnNpmInstall)
		{
			RunDefaultScript();
		}
	});
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

void UNodeComponent::BindEventToFunction(const FString& EventName, const FString& FunctionName, UObject* Target, const FString& Namespace /*= FString(TEXT("/"))*/)
{
	if (!FunctionName.IsEmpty())
	{
		if (Target == nullptr)
		{
			Target = (UObject*)GetOwner();
		}
		/*const FString SafeEventName = EventName;
		const FString SafeNamespace = Namespace;
		const FString SafeFunctionName = FunctionName;
		const UObject* SafeTarget = Target;*/

		TFunction<void()> BindFunction = [EventName, FunctionName, Namespace, Target, this]
		{
			
			//format: pid@EventName, needs to be assessed when we have a process/script id
			Cmd->Socket->OnRawEvent(FullEventName(EventName), [&, EventName, Target, FunctionName](const FString& Event, const sio::message::ptr& RawMessage) 
			{
				USIOJsonValue* NewValue = NewObject<USIOJsonValue>();
				auto Value = USIOMessageConvert::ToJsonValue(RawMessage);
				NewValue->SetRootValue(Value);
				CallBPFunctionWithMessage(Target, FunctionName, Value);
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
	else
	{
		//if we forgot our function name, fallback to regular bind event
		BindEvent(EventName, Namespace);
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
	TFieldIterator<FProperty> Iterator(Function);

	TArray<FProperty*> Properties;
	while (Iterator && (Iterator->PropertyFlags & CPF_Parm))
	{
		FProperty* Prop = *Iterator;
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
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Properties[0]);

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

bool UNodeComponent::CallBPFunctionWithMessage(UObject* Target, const FString& FunctionName, TSharedPtr<FJsonValue> Message)
{
	TArray<TSharedPtr<FJsonValue>> Response;
	Response.Add(Message);

	return CallBPFunctionWithResponse(Target, FunctionName, Response);
}

FString UNodeComponent::FullEventName(const FString& EventName)
{
	return FString::Printf(TEXT("%d@%s"), ScriptId, *EventName);
}

FString UNodeComponent::ProjectRootRelativeScriptFolder()
{
	FString FullPath = TEXT("Content/Scripts/") + DefaultScriptPath;
	int32 FoundPos = FullPath.Find("/", ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	return FullPath.Left(FoundPos);
}

TArray<FString> UNodeComponent::PackageDependencies()
{
	TArray<FString> Packages;

	FString AbsoluteFilePath = FPaths::ProjectDir() + ProjectRootRelativeScriptFolder() + TEXT("/package.json");

	TArray<uint8> OutBytes;
	bool bValidFile = FFileHelper::LoadFileToArray(OutBytes, *AbsoluteFilePath);

	if (!bValidFile)
	{
		return Packages;
	}

	FString JsonString;
	FFileHelper::BufferToString(JsonString, OutBytes.GetData(), OutBytes.Num());

	auto JsonObject = USIOJConvert::ToJsonObject(JsonString);

	const TSharedPtr<FJsonObject>* DependenciesPtr;
	bool bValidFormat = JsonObject->TryGetObjectField(TEXT("dependencies"), DependenciesPtr);

	if (!bValidFormat)
	{
		UE_LOG(LogTemp, Warning, TEXT("Invalid package.json: %s"), *USIOJConvert::ToJsonString(JsonObject));
		return Packages;
	}
	TSharedPtr<FJsonObject> Dependencies = *DependenciesPtr;
	for (auto& Value : Dependencies->Values)
	{
		Packages.Add(Value.Key);
	}

	return Packages;
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

