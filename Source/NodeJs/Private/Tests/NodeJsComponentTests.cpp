// Copyright getnamo. NodeJs-Unreal v2.1.0
//
// In-engine tests for UNodeComponent against the real bundled node + process.js.
// Run headless:
//   UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests NodeJs; Quit" -unattended -nullrhi -nopause -log

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "NodeComponent.h"
#include "NodeJsSubsystem.h"
#include "NodeJsTestReceiver.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Tests/AutomationCommon.h"
#include "Json.h"

namespace NodeJsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
	constexpr double Timeout = 15.0;

	//A standalone game instance + world, so both own-process and shared-process components work.
	struct FEnv
	{
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		UNodeJsTestReceiver* Receiver = nullptr;
		TArray<TWeakObjectPtr<AActor>> Actors;

		void Create()
		{
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->AddToRoot();
			GameInstance->InitializeStandalone();
			World = GameInstance->GetWorld();
			World->InitializeActorsForPlay(FURL());

			Receiver = NewObject<UNodeJsTestReceiver>();
			Receiver->AddToRoot();
		}

		//Configure runs before registration/BeginPlay, like editing the component's details.
		UNodeComponent* AddComponent(TFunctionRef<void(UNodeComponent*)> Configure)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			UNodeComponent* Component = NewObject<UNodeComponent>(Actor);
			Configure(Component);
			Component->OnConsoleLog.AddDynamic(Receiver, &UNodeJsTestReceiver::OnLog);
			Component->OnProcessScriptLog.AddDynamic(Receiver, &UNodeJsTestReceiver::OnLog);
			Component->OnScriptError.AddDynamic(Receiver, &UNodeJsTestReceiver::OnError);
			Component->NodeJsProcessParams.bLogScriptErrorsToOutput = false; //errors go to OnError instead
			Component->RegisterComponent();
			Actor->DispatchBeginPlay();
			Actors.Add(Actor);
			return Component;
		}

		void DestroyActor(UNodeComponent* Component)
		{
			if (Component && IsValid(Component->GetOwner()))
			{
				Component->GetOwner()->Destroy();
			}
		}

		void Destroy()
		{
			for (const TWeakObjectPtr<AActor>& Actor : Actors)
			{
				if (Actor.IsValid())
				{
					Actor->Destroy();
				}
			}
			Actors.Reset();
			if (GameInstance)
			{
				GameInstance->Shutdown();
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
				GameInstance->RemoveFromRoot();
			}
			if (Receiver)
			{
				Receiver->RemoveFromRoot();
			}
		}
	};

	class FWaitUntil : public IAutomationLatentCommand
	{
	public:
		FWaitUntil(FAutomationTestBase* InTest, const FString& InLabel, TFunction<bool()> InCondition, double InTimeout = Timeout)
			: Test(InTest), Label(InLabel), Condition(MoveTemp(InCondition)), TimeoutSeconds(InTimeout) {}

		virtual bool Update() override
		{
			if (StartTime < 0.0)
			{
				StartTime = FPlatformTime::Seconds();
			}
			if (Condition())
			{
				return true;
			}
			if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
			{
				Test->AddError(FString::Printf(TEXT("Timed out waiting for %s"), *Label));
				return true;
			}
			return false;
		}

	private:
		FAutomationTestBase* Test;
		FString Label;
		TFunction<bool()> Condition;
		double TimeoutSeconds;
		double StartTime = -1.0;
	};

	void Then(TFunction<void()> Step)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Step]() { Step(); return true; }));
	}

	void WaitUntil(FAutomationTestBase* Test, const FString& Label, TFunction<bool()> Condition, double InTimeout = Timeout)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FWaitUntil(Test, Label, MoveTemp(Condition), InTimeout));
	}

	void Delay(float Seconds)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FEngineWaitLatentCommand(Seconds));
	}

	FNodeJsScriptParams ScriptParams(const FString& Script, const FString& Folder, ENodeScriptLaunchMode Mode)
	{
		FNodeJsScriptParams Params;
		Params.Script = Script;
		Params.ScriptPathRoot = Folder;
		Params.LaunchMode = Mode;
		return Params;
	}

	double FirstArgNumberField(const FNodeEventData& Data, const FString& Field)
	{
		double Value = 0.0;
		if (Data.Args.Num() > 0 && Data.Args[0].IsValid() && Data.Args[0]->Type == EJson::Object)
		{
			Data.Args[0]->AsObject()->TryGetNumberField(Field, Value);
		}
		return Value;
	}
}

using namespace NodeJsTests;

// Queued emit before node is up, native/delegate/function bindings, OnEvent, running-state tracking.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsRoundTripTest, "NodeJs.Component.RoundTrip", NodeJsTests::Flags)
bool FNodeJsRoundTripTest::RunTest(const FString& Parameters)
{
	TSharedRef<FEnv> Env = MakeShared<FEnv>();
	Env->Create();
	TSharedRef<FString> NativeResult = MakeShared<FString>();

	UNodeComponent* Component = Env->AddComponent([](UNodeComponent* C)
	{
		C->DefaultScriptParams = ScriptParams(TEXT("adder.js"), TEXT("Content/Scripts/examples/"), ENodeScriptLaunchMode::Inline);
	});
	TWeakObjectPtr<UNodeComponent> Weak(Component);
	UNodeJsTestReceiver* Receiver = Env->Receiver;

	Component->BindEventNative(TEXT("result"), [NativeResult](const FNodeEventData& Data) { *NativeResult = Data.JsonArgs; });
	FNodeEventCallback Callback;
	Callback.BindUFunction(Receiver, GET_FUNCTION_NAME_CHECKED(UNodeJsTestReceiver, OnData));
	Component->BindEvent(TEXT("result"), Callback);
	TestTrue(TEXT("BindEventToFunction accepts (FString)"), Component->BindEventToFunction(TEXT("result"), TEXT("OnJson"), Receiver));
	TestTrue(TEXT("BindEventToFunction accepts (FString, TArray<uint8>)"), Component->BindEventToFunction(TEXT("result"), TEXT("OnJsonAndBinary"), Receiver));
	AddExpectedMessage(TEXT("must take"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("BindEventToFunction rejects a mismatched signature"), Component->BindEventToFunction(TEXT("result"), TEXT("OnBadSignature"), Receiver));
	Component->OnEvent.AddDynamic(Receiver, &UNodeJsTestReceiver::OnAnyEvent);

	//Node isn't up yet: this must be queued and delivered after the default script launches.
	Component->EmitEvent(TEXT("myevent"), TEXT("{\"a\":3,\"b\":4}"));

	WaitUntil(this, TEXT("adder result"), [NativeResult] { return !NativeResult->IsEmpty(); });
	Then([this, Weak, NativeResult, Receiver]
	{
		TestEqual(TEXT("native handler got [5]"), *NativeResult, FString(TEXT("[5]")));
		TestEqual(TEXT("BindEvent delegate fired once"), Receiver->DataCalls.Num(), 1);
		TestTrue(TEXT("BindEventToFunction (FString) got [5]"), Receiver->JsonCalls.Num() == 1 && Receiver->JsonCalls[0] == TEXT("[5]"));
		TestEqual(TEXT("BindEventToFunction (FString, Binary) fired"), Receiver->BinaryCalls.Num(), 1);
		TestTrue(TEXT("OnEvent fired for result"), Receiver->AnyEvents.Contains(TEXT("result")));
		if (UNodeComponent* C = Weak.Get())
		{
			TestTrue(TEXT("IsScriptRunning"), C->IsScriptRunning());
			TestTrue(TEXT("GetRunningScripts lists adder.js"), C->GetRunningScripts().Contains(TEXT("adder.js")));
			C->UnbindEvent(TEXT("result"));
			C->StopScript(C->DefaultScriptParams);
		}
	});
	WaitUntil(this, TEXT("script end"), [Weak] { return Weak.IsValid() && !Weak->IsScriptRunning(); });
	Then([Env] { Env->Destroy(); });
	return true;
}

// Emit Event With Callback (native + dynamic) against a worker-mode script, reply carries binary.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsCallbackTest, "NodeJs.Component.CallbackWorker", NodeJsTests::Flags)
bool FNodeJsCallbackTest::RunTest(const FString& Parameters)
{
	TSharedRef<FEnv> Env = MakeShared<FEnv>();
	Env->Create();
	TSharedRef<TOptional<FNodeEventData>> NativeReply = MakeShared<TOptional<FNodeEventData>>();

	UNodeComponent* Component = Env->AddComponent([](UNodeComponent* C)
	{
		C->DefaultScriptParams = ScriptParams(TEXT("acker.js"), TEXT("Content/Scripts/test/"), ENodeScriptLaunchMode::Worker);
	});
	UNodeJsTestReceiver* Receiver = Env->Receiver;

	TSharedRef<FJsonObject> Question = MakeShared<FJsonObject>();
	Question->SetNumberField(TEXT("n"), 21);
	TArray<TSharedPtr<FJsonValue>> Args;
	Args.Add(MakeShared<FJsonValueObject>(Question));
	Component->EmitEventNative(TEXT("ask"), Args, {}, TEXT(""), [NativeReply](const FNodeEventData& Data) { *NativeReply = Data; });

	FNodeEventCallback Callback;
	Callback.BindUFunction(Receiver, GET_FUNCTION_NAME_CHECKED(UNodeJsTestReceiver, OnData));
	Component->EmitEventWithCallback(TEXT("ask"), TEXT("{\"n\":5}"), Callback);

	WaitUntil(this, TEXT("callback replies"), [NativeReply, Receiver] { return NativeReply->IsSet() && Receiver->DataCalls.Num() > 0; });
	Then([this, NativeReply, Receiver, Env]
	{
		if (NativeReply->IsSet())
		{
			const FNodeEventData& Reply = NativeReply->GetValue();
			TestEqual(TEXT("native reply doubled 21"), FirstArgNumberField(Reply, TEXT("doubled")), 42.0);
			TestTrue(TEXT("native reply carries the [7,8,9] buffer"), Reply.Buffers.Num() == 1 && Reply.Buffers[0].Data == TArray<uint8>({ 7, 8, 9 }));
		}
		if (Receiver->DataCalls.Num() > 0)
		{
			TestEqual(TEXT("dynamic reply doubled 5"), FirstArgNumberField(Receiver->DataCalls[0], TEXT("doubled")), 10.0);
		}
		Env->Destroy();
	});
	return true;
}

// Several buffers each way through a subprocess-mode script.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsMultiBufferTest, "NodeJs.Component.MultiBufferSubprocess", NodeJsTests::Flags)
bool FNodeJsMultiBufferTest::RunTest(const FString& Parameters)
{
	TSharedRef<FEnv> Env = MakeShared<FEnv>();
	Env->Create();
	TSharedRef<TOptional<FNodeEventData>> Echo = MakeShared<TOptional<FNodeEventData>>();

	UNodeComponent* Component = Env->AddComponent([](UNodeComponent* C)
	{
		C->DefaultScriptParams = ScriptParams(TEXT("multiEcho.js"), TEXT("Content/Scripts/test/"), ENodeScriptLaunchMode::Subprocess);
	});
	Component->BindEventNative(TEXT("echoed"), [Echo](const FNodeEventData& Data) { *Echo = Data; });

	const TArray<uint8> B0 = { 1, 2, 3 };
	TArray<uint8> B1;
	B1.Init(5, 1000);
	Component->EmitEventWithBuffers(TEXT("echo"), TEXT("{\"tag\":\"x\"}"), { FNodeBuffer(B0), FNodeBuffer(B1) });

	WaitUntil(this, TEXT("echoed"), [Echo] { return Echo->IsSet(); });
	Then([this, Echo, Env, B0, B1]
	{
		if (Echo->IsSet())
		{
			const FNodeEventData& Data = Echo->GetValue();
			TestTrue(TEXT("both buffers came back in order"), Data.Buffers.Num() == 2 && Data.Buffers[0].Data == B0 && Data.Buffers[1].Data == B1);
			TestTrue(TEXT("json arg kept"), Data.JsonArgs.Contains(TEXT("\"tag\":\"x\"")));
			TestEqual(TEXT("script name reported"), Data.ScriptName, FString(TEXT("multiEcho.js")));
		}
		Env->Destroy();
	});
	return true;
}

// Inline stop really stops: shouldExit runs and interval timers are cleared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsInlineStopTest, "NodeJs.Component.InlineStopClearsTimers", NodeJsTests::Flags)
bool FNodeJsInlineStopTest::RunTest(const FString& Parameters)
{
	TSharedRef<FEnv> Env = MakeShared<FEnv>();
	Env->Create();
	TSharedRef<int32> Ticks = MakeShared<int32>(0);
	TSharedRef<int32> TicksAtStop = MakeShared<int32>(0);
	TSharedRef<bool> Bye = MakeShared<bool>(false);

	UNodeComponent* Component = Env->AddComponent([](UNodeComponent* C)
	{
		C->DefaultScriptParams = ScriptParams(TEXT("ticker.js"), TEXT("Content/Scripts/test/"), ENodeScriptLaunchMode::Inline);
	});
	TWeakObjectPtr<UNodeComponent> Weak(Component);
	Component->BindEventNative(TEXT("tick"), [Ticks](const FNodeEventData&) { ++(*Ticks); });
	Component->BindEventNative(TEXT("bye"), [Bye](const FNodeEventData&) { *Bye = true; });

	WaitUntil(this, TEXT("ticks"), [Ticks] { return *Ticks >= 3; });
	Then([Weak] { if (Weak.IsValid()) { Weak->StopScript(Weak->DefaultScriptParams); } });
	WaitUntil(this, TEXT("script end"), [Weak] { return Weak.IsValid() && !Weak->IsScriptRunning(); });
	Then([Ticks, TicksAtStop] { *TicksAtStop = *Ticks; });
	Delay(0.5f);
	Then([this, Ticks, TicksAtStop, Bye, Env]
	{
		TestTrue(TEXT("shouldExit handler ran"), *Bye);
		TestEqual(TEXT("no ticks after stop"), *Ticks, *TicksAtStop);
		Env->Destroy();
	});
	return true;
}

// Two components share one node process; same script, events stay with their owner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsSharedProcessTest, "NodeJs.Component.SharedProcess", NodeJsTests::Flags)
bool FNodeJsSharedProcessTest::RunTest(const FString& Parameters)
{
	TSharedRef<FEnv> Env = MakeShared<FEnv>();
	Env->Create();
	TSharedRef<int32> EchoA = MakeShared<int32>(0);
	TSharedRef<int32> EchoB = MakeShared<int32>(0);

	auto Configure = [](UNodeComponent* C)
	{
		C->NodeJsProcessParams.bShareMainProcess = true;
		C->DefaultScriptParams = ScriptParams(TEXT("multiEcho.js"), TEXT("Content/Scripts/test/"), ENodeScriptLaunchMode::Inline);
	};
	UNodeComponent* A = Env->AddComponent(Configure);
	UNodeComponent* B = Env->AddComponent(Configure);
	TWeakObjectPtr<UNodeComponent> WeakA(A), WeakB(B);
	A->BindEventNative(TEXT("echoed"), [EchoA](const FNodeEventData&) { ++(*EchoA); });
	B->BindEventNative(TEXT("echoed"), [EchoB](const FNodeEventData&) { ++(*EchoB); });

	UNodeJsSubsystem* Subsystem = Env->GameInstance->GetSubsystem<UNodeJsSubsystem>();
	TestNotNull(TEXT("subsystem exists"), Subsystem);
	TestEqual(TEXT("both components registered"), Subsystem ? Subsystem->NumRegisteredComponents() : 0, 2);

	A->EmitEvent(TEXT("echo"), TEXT("1"));

	WaitUntil(this, TEXT("A echoed"), [EchoA] { return *EchoA > 0; });
	Delay(0.3f);
	Then([this, WeakA, WeakB, EchoB, Subsystem, Env]
	{
		TestEqual(TEXT("B did not see A's event"), *EchoB, 0);
		TestTrue(TEXT("shared process running"), Subsystem && Subsystem->IsProcessRunning());
		TestTrue(TEXT("components did not start processes of their own"), WeakA.IsValid() && WeakB.IsValid() && !WeakA->bProcessIsRunning && !WeakB->bProcessIsRunning);
		if (!WeakA.IsValid() || !WeakB.IsValid())
		{
			return;
		}
		TestTrue(TEXT("both run the same script independently"), WeakA->IsScriptRunning() && WeakB->IsScriptRunning());

		//Removing A must leave B working.
		Env->DestroyActor(WeakA.Get());
		WeakB->EmitEvent(TEXT("echo"), TEXT("2"));
	});
	WaitUntil(this, TEXT("B echoed after A was destroyed"), [EchoB] { return *EchoB > 0; });
	Then([this, Subsystem, Env]
	{
		TestEqual(TEXT("A unregistered"), Subsystem ? Subsystem->NumRegisteredComponents() : -1, 1);
		Env->Destroy();
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsPackageDependenciesTest, "NodeJs.Component.PackageDependencies", NodeJsTests::Flags)
bool FNodeJsPackageDependenciesTest::RunTest(const FString& Parameters)
{
	const FString Folder = TEXT("Saved/NodeJsTests/pkg/");
	const FString Dir = FPaths::Combine(FPaths::ProjectDir(), Folder);
	FFileHelper::SaveStringToFile(TEXT("{\"name\":\"t\",\"dependencies\":{\"foo\":\"1.0.0\",\"bar\":\"^2\"}}"), *FPaths::Combine(Dir, TEXT("package.json")));
	FFileHelper::SaveStringToFile(TEXT("console.log('x');"), *FPaths::Combine(Dir, TEXT("sub/script.js")));

	UNodeComponent* Component = NewObject<UNodeComponent>();
	TArray<FString> Deps = Component->PackageDependencies(ScriptParams(TEXT("script.js"), Folder + TEXT("sub/"), ENodeScriptLaunchMode::Inline));
	Deps.Sort();
	TestEqual(TEXT("dependencies from the nearest package.json"), Deps, TArray<FString>({ TEXT("bar"), TEXT("foo") }));

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsLegacyLaunchModeTest, "NodeJs.Params.LegacyInlineFlag", NodeJsTests::Flags)
bool FNodeJsLegacyLaunchModeTest::RunTest(const FString& Parameters)
{
	FNodeJsScriptParams Legacy;
	Legacy.bInlineLaunchScript = false;
	TestEqual(TEXT("v2.0 bInlineLaunchScript=false still means Subprocess"), Legacy.GetEffectiveLaunchMode(), ENodeScriptLaunchMode::Subprocess);

	FNodeJsScriptParams Worker;
	Worker.LaunchMode = ENodeScriptLaunchMode::Worker;
	TestEqual(TEXT("LaunchMode applies while bInlineLaunchScript is true"), Worker.GetEffectiveLaunchMode(), ENodeScriptLaunchMode::Worker);

	FNodeJsScriptParams Default;
	TestEqual(TEXT("default is Inline"), Default.GetEffectiveLaunchMode(), ENodeScriptLaunchMode::Inline);
	return true;
}

// StopProcess then StartProcess in the same frame restarts cleanly and relaunches the default script.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNodeJsRestartTest, "NodeJs.Component.StopStartSameFrame", NodeJsTests::Flags)
bool FNodeJsRestartTest::RunTest(const FString& Parameters)
{
	TSharedRef<FEnv> Env = MakeShared<FEnv>();
	Env->Create();
	TSharedRef<int32> Echoes = MakeShared<int32>(0);

	UNodeComponent* Component = Env->AddComponent([](UNodeComponent* C)
	{
		C->DefaultScriptParams = ScriptParams(TEXT("multiEcho.js"), TEXT("Content/Scripts/test/"), ENodeScriptLaunchMode::Inline);
	});
	TWeakObjectPtr<UNodeComponent> Weak(Component);
	Component->BindEventNative(TEXT("echoed"), [Echoes](const FNodeEventData&) { ++(*Echoes); });

	WaitUntil(this, TEXT("first script running"), [Weak] { return Weak.IsValid() && Weak->IsScriptRunning(); });
	Then([this, Weak]
	{
		Weak->StopProcess();
		TestFalse(TEXT("stopped synchronously"), Weak->bProcessIsRunning || Weak->IsScriptRunning());
		Weak->StartProcess();
		Weak->EmitEvent(TEXT("echo"), TEXT("1"));
	});
	WaitUntil(this, TEXT("echo from the restarted process"), [Echoes] { return *Echoes > 0; });
	Delay(0.5f);
	Then([this, Weak, Env]
	{
		TestTrue(TEXT("still running after the old process's late end"), Weak.IsValid() && Weak->bProcessIsRunning && Weak->IsScriptRunning());
		Env->Destroy();
	});
	return true;
}

#endif //WITH_DEV_AUTOMATION_TESTS
