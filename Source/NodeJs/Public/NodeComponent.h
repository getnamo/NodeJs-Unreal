// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SocketIOClientComponent.h"
#include "NodeCmd.h"
#include "NodeComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeSciptBeginSignature, int32, ProcessId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeConsoleLogSignature, FString, LogMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeScriptEndSignature, FString, FinishedScriptRelativePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNodeScriptErrorSignature, FString, ScriptRelativePath, FString, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNpmInstallResultSignature, bool, bIsInstalled, FString, ErrorMessage);

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class NODEJS_API UNodeComponent : public UActorComponent
{
	GENERATED_BODY()

public:	

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FSIOCEventJsonSignature OnEvent;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeConsoleLogSignature OnConsoleLog;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeSciptBeginSignature OnScriptBegin;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptEndSignature OnScriptEnd;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptErrorSignature OnScriptError;

	UPROPERTY(BlueprintAssignable, Category = "Npm Events")
	FNpmInstallResultSignature OnNpmDependenciesResolved;

	//set this to true if you'd like the default script to start with the component
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	bool bRunDefaultScriptOnBeginPlay;

	//not yet implemented
	//UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	bool bReloadOnChange;

	//This should always be true, removed from BP exposure
	//UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	bool bStartMainScriptIfNeededOnBeginPlay;

	//This will cleanup our main script thread whenever there are no listeners. May slow down quick map travels. Default off.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	bool bStopMainScriptOnNoListeners;

	//Relative to {project root}/Content/Scripts
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	FString DefaultScriptPath;

	//we only allow one script per component
	UPROPERTY(BlueprintReadOnly, Category = NodeJsProperties)
	bool bScriptIsRunning;

	//-1 if invalid
	UPROPERTY(BlueprintReadOnly, Category = NodeJsProperties)
	int32 ScriptId;

	/** Run a different script than default */
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void RunScript(const FString& ScriptRelativePath);

	/** If you didn't run it at begin play, call this function to run the default script */
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void RunDefaultScript();

	/** forcibly stop the script */
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void StopScript();

	/** Checks your DefaultScriptPath package.json dependencies and installs them if needed */
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void ResolveNpmDependencies();

	/**
	* Emit an event with a JsonValue message
	*
	* @param Name		Event name
	* @param Message	SIOJJsonValue
	* @param Namespace	Namespace within socket.io
	*/
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void Emit(const FString& EventName, USIOJsonValue* Message = nullptr, const FString& Namespace = FString(TEXT("/")));

	/**
	* Emit an event with a JsonValue message with a callback function defined by CallBackFunctionName
	* This may not work due to ipc-event-emitter support, untested.
	*
	* @param Name					Event name
	* @param Message				SIOJsonValue
	* @param CallbackFunctionName	Name of the optional callback function with signature (String, SIOJsonValue)
	* @param Target					CallbackFunction target object, typically self where this is called.
	* @param Namespace				Namespace within socket.io
	*/
	UFUNCTION(BlueprintCallable, Category = "SocketIO Functions")
	void EmitWithCallBack(const FString& EventName,
			USIOJsonValue* Message = nullptr,
			const FString& CallbackFunctionName = FString(""),
			UObject* Target = nullptr,
			const FString& Namespace = FString(TEXT("/")));

	/**
	* Bind an event, then respond to it with 'On' multi-cast delegate
	*
	* @param EventName	Event name
	* @param Namespace	Optional namespace, defaults to default namespace
	*/
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void BindEvent(const FString& EventName, const FString& Namespace = FString(TEXT("/")));

	/**
	* Bind an event to a function with the given name.
	* Expects a String message signature which can be decoded from JSON into SIOJsonObject
	*
	* @param EventName		Event name
	* @param FunctionName	The function that gets called when the event is received
	* @param Target			Optional, defaults to owner. Change to delegate to another class.
	*/
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void BindEventToFunction(const FString& EventName,
			const FString& FunctionName,
			UObject* Target,
			const FString& Namespace = FString(TEXT("/")));

	UNodeComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;



protected:
	void LinkAndStartWrapperScript();

	virtual void BeginPlay() override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	bool CallBPFunctionWithResponse(UObject* Target, const FString& FunctionName, TArray<TSharedPtr<FJsonValue>> Response);
	bool CallBPFunctionWithMessage(UObject* Target, const FString& FunctionName, TSharedPtr<FJsonValue> Message);

private:
	TSharedPtr<FNodeCmd> Cmd;
	TSharedPtr<FNodeEventListener> Listener;
	TArray<TFunction<void()>> DelayedBindEvents;
	TArray<FString> BoundEventNames;

	//append process id for mux routing in main script
	FString FullEventName(const FString& EventName);
	void UnbindAllScriptEvents();
};
