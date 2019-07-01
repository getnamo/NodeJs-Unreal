// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SocketIOClientComponent.h"
#include "NodeCmd.h"
#include "NodeComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeConsoleLogSignature, FString, LogMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FNodeScriptEndSignature, FString, FinishedScriptRelativePath);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FNodeScriptErrorSignature, FString, ScriptRelativePath, FString, ErrorMessage);

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
	FNodeScriptEndSignature OnScriptEnd;

	UPROPERTY(BlueprintAssignable, Category = "NodeJs Events")
	FNodeScriptErrorSignature OnScriptError;

	//set this to true if you'd like the default script to start with the component
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	bool bRunDefaultScriptOnBeginPlay;

	//this should always be true
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	bool bRunMainScriptOnBeginPlay;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = NodeJsProperties)
	FString DefaultScript;

	//todo: remove blueprint visibility for this, it's an internal function
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void RunMainScript(const FString& ScriptRelativePath);

	/** Run your scripts here */
	UFUNCTION(BlueprintCallable, Category = "NodeJs Functions")
	void RunScript(const FString& ScriptRelativePath);

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

	UNodeComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	bool CallBPFunctionWithResponse(UObject* Target, const FString& FunctionName, TArray<TSharedPtr<FJsonValue>> Response);

private:
	TSharedPtr<FNodeCmd> Cmd;

	FString MainScript;
};
