// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkProcess.h"
#include "CinderLinkProtocol.h"
#include "CinderLinkEditorTools.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    constexpr const TCHAR* CanaryName = TEXT("CINDERLINK_TEST_SECRET_CANARY");

    bool WriteObject(FCinderLinkProcess& Process, const TSharedRef<FJsonObject>& Object, FString& OutError)
    {
        FString Serialized;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
        return FJsonSerializer::Serialize(Object, Writer) && Process.WriteJsonLine(Serialized, OutError);
    }

    TSharedRef<FJsonObject> MakeRequest(int64 Id, const FString& Method, const TSharedRef<FJsonObject>& Params)
    {
        TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
        Request->SetNumberField(TEXT("id"), static_cast<double>(Id));
        Request->SetStringField(TEXT("method"), Method);
        Request->SetObjectField(TEXT("params"), Params);
        return Request;
    }

    struct FHandshakeState
    {
        ~FHandshakeState()
        {
            if (Process)
            {
                Process->Stop();
            }
            FPlatformMisc::SetEnvironmentVar(CanaryName, *PreviousCanaryValue);
            if (!OutsideCanaryPath.IsEmpty())
            {
                IFileManager::Get().Delete(*OutsideCanaryPath, false, true);
            }
        }

        TUniquePtr<FCinderLinkProcess> Process;
        FString PreviousCanaryValue;
        FString OutsideCanaryPath;
        FString Failure;
        TArray<FString> McpServerNames;
        FString ThreadId;
        bool bInitializeReceived = false;
        bool bMcpIsolationVerified = false;
        bool bPermissionProfileFound = false;
        bool bThreadReceived = false;
        bool bProjectRootBound = false;
        bool bCanaryWasAbsent = false;
        bool bOutsideReadBlocked = false;
        bool bFollowUpSent = false;
        double Deadline = 0.0;
    };

    class FCinderLinkHandshakeLatentCommand final : public IAutomationLatentCommand
    {
    public:
        FCinderLinkHandshakeLatentCommand(
            FAutomationTestBase* InTest,
            const TSharedRef<FHandshakeState>& InState)
            : Test(InTest)
            , State(InState)
        {
        }

        virtual bool Update() override
        {
            State->Process->PumpOutput(
                [this](const FString& Line) { HandleLine(Line); },
                [this](const FString& Line)
                {
                    if (State->Failure.IsEmpty() && !Line.IsEmpty())
                    {
                        State->Failure = TEXT("Codex App Server stderr: ") + Line.Left(2048);
                    }
                });

            if (!State->Failure.IsEmpty())
            {
                Test->AddError(State->Failure);
                State->Process->Stop();
                return true;
            }

            if (State->bInitializeReceived && State->bPermissionProfileFound &&
                State->bThreadReceived && State->bProjectRootBound && State->bCanaryWasAbsent &&
                State->bOutsideReadBlocked && State->bMcpIsolationVerified)
            {
                Test->TestTrue(TEXT("Initialize response received"), State->bInitializeReceived);
                Test->TestTrue(TEXT("Project permission profile was discovered"), State->bPermissionProfileFound);
                Test->TestTrue(TEXT("Project-only thread was accepted"), State->bThreadReceived);
                Test->TestTrue(TEXT("Runtime workspace root is the project"), State->bProjectRootBound);
                Test->TestTrue(TEXT("Unlisted parent environment value was removed"), State->bCanaryWasAbsent);
                Test->TestTrue(TEXT("A file outside the project was unreadable"), State->bOutsideReadBlocked);
                Test->TestTrue(TEXT("Every discovered MCP server was disabled"), State->bMcpIsolationVerified);
                State->Process->Stop();
                return true;
            }

            if (!State->Process->IsRunning())
            {
                Test->AddError(TEXT("Codex App Server exited during the handshake test."));
                return true;
            }

            if (FPlatformTime::Seconds() >= State->Deadline)
            {
                Test->AddError(TEXT("Timed out waiting for Codex App Server handshake responses."));
                State->Process->Stop();
                return true;
            }
            return false;
        }

    private:
        void HandleLine(const FString& Line)
        {
            TSharedPtr<FJsonObject> Message;
            if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Message) || !Message.IsValid())
            {
                return;
            }

            double IdValue = 0.0;
            if (!Message->TryGetNumberField(TEXT("id"), IdValue))
            {
                return;
            }
            const int64 Id = static_cast<int64>(IdValue);

            const TSharedPtr<FJsonObject>* Error = nullptr;
            if (Message->TryGetObjectField(TEXT("error"), Error) && Error != nullptr && Error->IsValid())
            {
                FString ErrorMessage;
                (*Error)->TryGetStringField(TEXT("message"), ErrorMessage);
                State->Failure = FString::Printf(TEXT("App Server request %lld failed: %s"), Id, *ErrorMessage);
                return;
            }

            if (Id == 1)
            {
                State->bInitializeReceived = true;
                if (!State->bFollowUpSent)
                {
                    State->bFollowUpSent = true;
                    SendFollowUpRequests();
                }
                return;
            }

            const TSharedPtr<FJsonObject>* Result = nullptr;
            if (!Message->TryGetObjectField(TEXT("result"), Result) || Result == nullptr || !Result->IsValid())
            {
                return;
            }

            if (Id == 2)
            {
                const TArray<TSharedPtr<FJsonValue>>* Servers = nullptr;
                if (!(*Result)->TryGetArrayField(TEXT("data"), Servers) || Servers == nullptr)
                {
                    State->Failure = TEXT("Could not enumerate MCP servers during the integration test.");
                    return;
                }

                for (const TSharedPtr<FJsonValue>& ServerValue : *Servers)
                {
                    const TSharedPtr<FJsonObject> Server = ServerValue.IsValid() ? ServerValue->AsObject() : nullptr;
                    FString Name;
                    if (!Server.IsValid() || !Server->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
                    {
                        State->Failure = TEXT("App Server returned an invalid MCP server entry.");
                        return;
                    }
                    State->McpServerNames.AddUnique(Name);
                }
                SendPermissionProfileRequest();
            }
            else if (Id == 3)
            {
                const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
                if ((*Result)->TryGetArrayField(TEXT("data"), Profiles) && Profiles != nullptr)
                {
                    for (const TSharedPtr<FJsonValue>& ProfileValue : *Profiles)
                    {
                        const TSharedPtr<FJsonObject> Profile = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
                        FString ProfileId;
                        bool bAllowed = false;
                        if (Profile.IsValid() && Profile->TryGetStringField(TEXT("id"), ProfileId) &&
                            Profile->TryGetBoolField(TEXT("allowed"), bAllowed) &&
                            ProfileId == TEXT("cinderlink-project-read") && bAllowed)
                        {
                            State->bPermissionProfileFound = true;
                            SendThreadAndCanaryRequests();
                            break;
                        }
                    }
                }
                if (!State->bPermissionProfileFound)
                {
                    State->Failure = TEXT("The project-only permission profile was not available.");
                }
            }
            else if (Id == 4)
            {
                const TSharedPtr<FJsonObject>* Thread = nullptr;
                FString ThreadId;
                if ((*Result)->TryGetObjectField(TEXT("thread"), Thread) && Thread != nullptr && Thread->IsValid())
                {
                    (*Thread)->TryGetStringField(TEXT("id"), ThreadId);
                }
                State->bThreadReceived = !ThreadId.IsEmpty();

                const TArray<TSharedPtr<FJsonValue>>* RuntimeRoots = nullptr;
                if ((*Result)->TryGetArrayField(TEXT("runtimeWorkspaceRoots"), RuntimeRoots) &&
                    RuntimeRoots != nullptr && RuntimeRoots->Num() == 1)
                {
                    FString ExpectedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
                    FString ActualRoot = (*RuntimeRoots)[0]->AsString();
                    FPaths::NormalizeDirectoryName(ExpectedRoot);
                    FPaths::NormalizeDirectoryName(ActualRoot);
                    State->bProjectRootBound = ExpectedRoot.Equals(ActualRoot, ESearchCase::IgnoreCase);
                }

                const TSharedPtr<FJsonObject>* ActiveProfile = nullptr;
                FString ActiveProfileId;
                if ((*Result)->TryGetObjectField(TEXT("activePermissionProfile"), ActiveProfile) &&
                    ActiveProfile != nullptr && ActiveProfile->IsValid())
                {
                    (*ActiveProfile)->TryGetStringField(TEXT("id"), ActiveProfileId);
                }
                State->bThreadReceived = State->bThreadReceived &&
                    ActiveProfileId == TEXT("cinderlink-project-read");
                if (!State->bThreadReceived || !State->bProjectRootBound)
                {
                    State->Failure = TEXT("App Server did not activate the exact project boundary.");
                    return;
                }
                State->ThreadId = ThreadId;
                SendMcpVerificationRequest();
            }
            else if (Id == 5)
            {
                const TArray<TSharedPtr<FJsonValue>>* Servers = nullptr;
                if (!(*Result)->TryGetArrayField(TEXT("data"), Servers) || Servers == nullptr)
                {
                    State->Failure = TEXT("Could not verify MCP isolation for the active thread.");
                    return;
                }

                for (const TSharedPtr<FJsonValue>& ServerValue : *Servers)
                {
                    const TSharedPtr<FJsonObject> Server = ServerValue.IsValid() ? ServerValue->AsObject() : nullptr;
                    FString RuntimeStatus;
                    const TSharedPtr<FJsonObject>* Tools = nullptr;
                    const bool bHasTools = Server.IsValid() &&
                        Server->TryGetObjectField(TEXT("tools"), Tools) && Tools != nullptr &&
                        Tools->IsValid() && (*Tools)->Values.Num() > 0;
                    if (!Server.IsValid() || !Server->TryGetStringField(TEXT("runtimeStatus"), RuntimeStatus) ||
                        RuntimeStatus != TEXT("disabled") || bHasTools)
                    {
                        State->Failure = TEXT("An MCP server or external MCP tool remained active.");
                        return;
                    }
                }
                State->bMcpIsolationVerified = true;
                SendCanaryRequests();
            }
            else if (Id == 6)
            {
                double ExitCode = -1.0;
                (*Result)->TryGetNumberField(TEXT("exitCode"), ExitCode);
                State->bCanaryWasAbsent = static_cast<int32>(ExitCode) == 0;
                if (!State->bCanaryWasAbsent)
                {
                    State->Failure = TEXT("The Codex child inherited an environment variable outside the allowlist.");
                }
            }
            else if (Id == 7)
            {
                double ExitCode = 0.0;
                (*Result)->TryGetNumberField(TEXT("exitCode"), ExitCode);
                State->bOutsideReadBlocked = static_cast<int32>(ExitCode) != 0;
                if (!State->bOutsideReadBlocked)
                {
                    State->Failure = TEXT("The Codex command sandbox read a file outside the project boundary.");
                }
            }
        }

        void SendFollowUpRequests()
        {
            FString Error;
            TSharedRef<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
            TSharedRef<FJsonObject> Initialized = MakeShared<FJsonObject>();
            Initialized->SetStringField(TEXT("method"), TEXT("initialized"));
            Initialized->SetObjectField(TEXT("params"), EmptyParams);
            if (!WriteObject(*State->Process, Initialized, Error))
            {
                State->Failure = Error;
                return;
            }

            TSharedRef<FJsonObject> McpParams = MakeShared<FJsonObject>();
            McpParams->SetStringField(TEXT("detail"), TEXT("toolsAndAuthOnly"));
            if (!WriteObject(*State->Process, MakeRequest(2, TEXT("mcpServerStatus/list"), McpParams), Error))
            {
                State->Failure = Error;
            }
        }

        void SendPermissionProfileRequest()
        {
            FString Error;
            const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
            TSharedRef<FJsonObject> ProfileParams = MakeShared<FJsonObject>();
            ProfileParams->SetStringField(TEXT("cwd"), Root);
            if (!WriteObject(*State->Process, MakeRequest(3, TEXT("permissionProfile/list"), ProfileParams), Error))
            {
                State->Failure = Error;
            }
        }

        void SendThreadAndCanaryRequests()
        {
            FString Error;
            const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
            TSharedRef<FJsonObject> ThreadParams = MakeShared<FJsonObject>();
            ThreadParams->SetStringField(TEXT("cwd"), Root);
            ThreadParams->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
            ThreadParams->SetStringField(TEXT("approvalsReviewer"), TEXT("user"));
            ThreadParams->SetStringField(TEXT("permissions"), TEXT("cinderlink-project-read"));
            ThreadParams->SetBoolField(TEXT("ephemeral"), true);
            ThreadParams->SetObjectField(
                TEXT("config"),
                FCinderLinkAppServerClient::BuildIsolationConfig(State->McpServerNames));
            ThreadParams->SetArrayField(TEXT("dynamicTools"), FCinderLinkEditorTools::BuildToolSpecs());
            TArray<TSharedPtr<FJsonValue>> RuntimeRoots;
            RuntimeRoots.Add(MakeShared<FJsonValueString>(Root));
            ThreadParams->SetArrayField(TEXT("runtimeWorkspaceRoots"), RuntimeRoots);
            ThreadParams->SetStringField(TEXT("serviceName"), TEXT("cinderlink_test"));
            if (!WriteObject(*State->Process, MakeRequest(4, TEXT("thread/start"), ThreadParams), Error))
            {
                State->Failure = Error;
            }
        }

        void SendMcpVerificationRequest()
        {
            FString Error;
            TSharedRef<FJsonObject> McpParams = MakeShared<FJsonObject>();
            McpParams->SetStringField(TEXT("detail"), TEXT("toolsAndAuthOnly"));
            McpParams->SetStringField(TEXT("threadId"), State->ThreadId);
            if (!WriteObject(*State->Process, MakeRequest(5, TEXT("mcpServerStatus/list"), McpParams), Error))
            {
                State->Failure = Error;
            }
        }

        void SendCanaryRequests()
        {
            FString Error;
            const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
            TArray<TSharedPtr<FJsonValue>> Command;
            Command.Add(MakeShared<FJsonValueString>(TEXT("cmd.exe")));
            Command.Add(MakeShared<FJsonValueString>(TEXT("/D")));
            Command.Add(MakeShared<FJsonValueString>(TEXT("/C")));
            Command.Add(MakeShared<FJsonValueString>(
                TEXT("if defined CINDERLINK_TEST_SECRET_CANARY (exit /b 9) else (exit /b 0)")));

            TSharedRef<FJsonObject> CommandParams = MakeShared<FJsonObject>();
            CommandParams->SetArrayField(TEXT("command"), Command);
            CommandParams->SetStringField(TEXT("cwd"), Root);
            CommandParams->SetStringField(TEXT("permissionProfile"), TEXT("cinderlink-project-read"));
            CommandParams->SetNumberField(TEXT("timeoutMs"), 5000.0);
            if (!WriteObject(*State->Process, MakeRequest(6, TEXT("command/exec"), CommandParams), Error))
            {
                State->Failure = Error;
                return;
            }

            TArray<TSharedPtr<FJsonValue>> OutsideCommand;
            OutsideCommand.Add(MakeShared<FJsonValueString>(TEXT("cmd.exe")));
            OutsideCommand.Add(MakeShared<FJsonValueString>(TEXT("/D")));
            OutsideCommand.Add(MakeShared<FJsonValueString>(TEXT("/C")));
            OutsideCommand.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("type \"%s\" >nul 2>nul"), *State->OutsideCanaryPath)));

            TSharedRef<FJsonObject> OutsideParams = MakeShared<FJsonObject>();
            OutsideParams->SetArrayField(TEXT("command"), OutsideCommand);
            OutsideParams->SetStringField(TEXT("cwd"), Root);
            OutsideParams->SetStringField(TEXT("permissionProfile"), TEXT("cinderlink-project-read"));
            OutsideParams->SetNumberField(TEXT("timeoutMs"), 5000.0);
            if (!WriteObject(*State->Process, MakeRequest(7, TEXT("command/exec"), OutsideParams), Error))
            {
                State->Failure = Error;
            }
        }

        FAutomationTestBase* Test;
        TSharedRef<FHandshakeState> State;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCinderLinkAppServerHandshakeTest,
    "CinderLink.Integration.AppServerHandshake",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkAppServerHandshakeTest::RunTest(const FString& Parameters)
{
    FString ResolveError;
    const FString Executable = FCinderLinkProcess::ResolveCodexExecutable(ResolveError);
    if (Executable.IsEmpty())
    {
        AddWarning(TEXT("Skipped App Server handshake: ") + ResolveError);
        return true;
    }

    TSharedRef<FHandshakeState> State = MakeShared<FHandshakeState>();
    State->PreviousCanaryValue = FPlatformMisc::GetEnvironmentVariable(CanaryName);
    FPlatformMisc::SetEnvironmentVar(CanaryName, TEXT("must-not-reach-child"));
    State->OutsideCanaryPath = FPaths::Combine(
        FPlatformProcess::UserTempDir(),
        TEXT("CinderLinkOutsideBoundaryCanary.txt"));
    if (!FFileHelper::SaveStringToFile(TEXT("CINDERLINK_BOUNDARY_CANARY"), *State->OutsideCanaryPath))
    {
        AddError(TEXT("Could not create the harmless outside-boundary test file."));
        return false;
    }
    State->Process = MakeUnique<FCinderLinkProcess>();

    FString StartError;
    if (!State->Process->Start(Executable, FPaths::ProjectDir(), StartError))
    {
        AddError(StartError);
        return false;
    }

    TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
    ClientInfo->SetStringField(TEXT("name"), TEXT("cinderlink_test"));
    ClientInfo->SetStringField(TEXT("title"), TEXT("CinderLink Test"));
    ClientInfo->SetStringField(TEXT("version"), TEXT("0.2.1"));
    TSharedRef<FJsonObject> InitializeParams = MakeShared<FJsonObject>();
    InitializeParams->SetObjectField(TEXT("clientInfo"), ClientInfo);
    TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    Capabilities->SetBoolField(TEXT("experimentalApi"), true);
    InitializeParams->SetObjectField(TEXT("capabilities"), Capabilities);

    FString WriteError;
    if (!WriteObject(*State->Process, MakeRequest(1, TEXT("initialize"), InitializeParams), WriteError))
    {
        AddError(WriteError);
        return false;
    }

    State->Deadline = FPlatformTime::Seconds() + 15.0;
    ADD_LATENT_AUTOMATION_COMMAND(FCinderLinkHandshakeLatentCommand(this, State));
    return true;
}

#endif
