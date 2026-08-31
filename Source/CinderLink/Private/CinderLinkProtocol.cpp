// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkProtocol.h"

#include "CinderLinkEditorTools.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    constexpr const TCHAR* ReadPermissionProfile = TEXT("cinderlink-project-read");
    constexpr const TCHAR* EditPermissionProfile = TEXT("cinderlink-project-edit");
    constexpr int32 MaximumMcpServerCount = 128;
    constexpr int32 MaximumMcpServerNameLength = 256;

    TSharedRef<FJsonObject> MakeParams()
    {
        return MakeShared<FJsonObject>();
    }

    bool GetObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TSharedPtr<FJsonObject>& OutObject)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* Found = nullptr;
        if (Object->TryGetObjectField(Name, Found) && Found != nullptr && Found->IsValid())
        {
            OutObject = *Found;
            return true;
        }
        return false;
    }

    FString TruncateForUi(const FString& Value, int32 MaximumCharacters = 4096)
    {
        if (Value.Len() <= MaximumCharacters)
        {
            return Value;
        }
        return Value.Left(MaximumCharacters) + TEXT("\n[output truncated by CinderLink]");
    }
}

FCinderLinkAppServerClient::FCinderLinkAppServerClient() = default;

FCinderLinkAppServerClient::~FCinderLinkAppServerClient()
{
    Disconnect();
}

bool FCinderLinkAppServerClient::Connect(const FString& ExecutablePath, const FString& InProjectRoot, FString& OutError)
{
    Disconnect();

    ProjectRoot = NormalizeRoot(InProjectRoot);
    if (!IFileManager::Get().DirectoryExists(*ProjectRoot))
    {
        OutError = TEXT("The current Unreal project directory does not exist.");
        return false;
    }

    if (!Process.Start(ExecutablePath, ProjectRoot, OutError))
    {
        return false;
    }

    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FCinderLinkAppServerClient::Tick),
        0.05f);
    bReportedProcessExit = false;
    Emit(ECinderLinkMessageKind::Status, TEXT("Codex App Server started with a sanitized environment."));
    SendInitialize();
    return true;
}

void FCinderLinkAppServerClient::Disconnect()
{
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }

    Process.Stop();
    PendingRequests.Reset();
    ThreadId.Reset();
    ActiveTurnId.Reset();
    ProjectRoot.Reset();
    NextRequestId = 1;
    bTurnInProgress = false;
    bReportedProcessExit = false;
    McpServerNames.Reset();
    bReadPermissionProfileReady = false;
    bEditPermissionProfileReady = false;
    bIsolationReady = false;
    bActiveTurnAllowsEditorActions = false;
}

bool FCinderLinkAppServerClient::StartNewThread(FString& OutError)
{
    if (!Process.IsRunning())
    {
        OutError = TEXT("Connect to Codex App Server first.");
        return false;
    }
    if (bTurnInProgress)
    {
        OutError = TEXT("Stop the active turn before starting a new thread.");
        return false;
    }

    ThreadId.Reset();
    ActiveTurnId.Reset();
    bIsolationReady = false;
    bActiveTurnAllowsEditorActions = false;
    SendThreadStart();
    return true;
}

bool FCinderLinkAppServerClient::SendTurn(
    const FString& Text,
    bool bAllowProjectEdits,
    bool bAllowEditorActions,
    FString& OutError)
{
    FString Trimmed = Text;
    Trimmed.TrimStartAndEndInline();
    if (Trimmed.IsEmpty())
    {
        OutError = TEXT("Enter a message first.");
        return false;
    }
    if (!IsReady())
    {
        OutError = TEXT("CinderLink is not ready yet.");
        return false;
    }
    if (bTurnInProgress)
    {
        OutError = TEXT("A turn is already running.");
        return false;
    }

    TSharedRef<FJsonObject> Params = MakeParams();
    Params->SetStringField(TEXT("threadId"), ThreadId);
    Params->SetStringField(TEXT("cwd"), ProjectRoot);
    Params->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
    Params->SetStringField(TEXT("approvalsReviewer"), TEXT("user"));
    Params->SetStringField(TEXT("permissions"), GetPermissionProfileName(bAllowProjectEdits));
    TArray<TSharedPtr<FJsonValue>> RuntimeRoots;
    RuntimeRoots.Add(MakeShared<FJsonValueString>(ProjectRoot));
    Params->SetArrayField(TEXT("runtimeWorkspaceRoots"), RuntimeRoots);

    TSharedRef<FJsonObject> InputItem = MakeShared<FJsonObject>();
    InputItem->SetStringField(TEXT("type"), TEXT("text"));
    InputItem->SetStringField(TEXT("text"), Trimmed);
    TArray<TSharedPtr<FJsonValue>> Inputs;
    Inputs.Add(MakeShared<FJsonValueObject>(InputItem));
    Params->SetArrayField(TEXT("input"), Inputs);

    const int64 RequestId = SendRequest(TEXT("turn/start"), Params, EPendingRequest::TurnStart, OutError);
    if (RequestId == 0)
    {
        return false;
    }

    bTurnInProgress = true;
    ActiveTurnId.Reset();
    bActiveTurnAllowsEditorActions = bAllowEditorActions;
    Emit(
        ECinderLinkMessageKind::Status,
        FString::Printf(
            TEXT("Turn started: project files %s; UE Editor actions %s; all escalation disabled."),
            bAllowProjectEdits ? TEXT("editable") : TEXT("read-only"),
            bAllowEditorActions ? TEXT("enabled") : TEXT("read-only")));
    return true;
}

bool FCinderLinkAppServerClient::InterruptTurn(FString& OutError)
{
    if (!IsReady() || !bTurnInProgress)
    {
        OutError = TEXT("There is no active turn to stop.");
        return false;
    }

    TSharedRef<FJsonObject> Params = MakeParams();
    Params->SetStringField(TEXT("threadId"), ThreadId);
    return SendRequest(TEXT("turn/interrupt"), Params, EPendingRequest::Interrupt, OutError) != 0;
}

FString FCinderLinkAppServerClient::GetPermissionProfileName(bool bAllowProjectEdits)
{
    return bAllowProjectEdits ? EditPermissionProfile : ReadPermissionProfile;
}

TSharedRef<FJsonObject> FCinderLinkAppServerClient::BuildIsolationConfig(
    const TArray<FString>& InMcpServerNames)
{
    TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> McpServers = MakeShared<FJsonObject>();
    for (const FString& Name : InMcpServerNames)
    {
        TSharedRef<FJsonObject> Disabled = MakeShared<FJsonObject>();
        Disabled->SetBoolField(TEXT("enabled"), false);
        McpServers->SetObjectField(Name, Disabled);
    }
    Config->SetObjectField(TEXT("mcp_servers"), McpServers);

    TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
    Tools->SetBoolField(TEXT("web_search"), false);
    Tools->SetBoolField(TEXT("view_image"), false);
    Config->SetObjectField(TEXT("tools"), Tools);
    return Config;
}

bool FCinderLinkAppServerClient::Tick(float DeltaTime)
{
    Process.PumpOutput(
        [this](const FString& Line) { HandleStdOutLine(Line); },
        [this](const FString& Line) { HandleStdErrLine(Line); });

    if (!Process.IsRunning() && !bReportedProcessExit)
    {
        bReportedProcessExit = true;
        bTurnInProgress = false;
        bActiveTurnAllowsEditorActions = false;
        ActiveTurnId.Reset();
        ThreadId.Reset();
        PendingRequests.Reset();
        Emit(ECinderLinkMessageKind::Error, TEXT("Codex App Server exited. Disconnect and reconnect to continue."));
    }
    return true;
}

bool FCinderLinkAppServerClient::SendObject(const TSharedRef<FJsonObject>& Object, FString& OutError)
{
    FString Serialized;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
    if (!FJsonSerializer::Serialize(Object, Writer))
    {
        OutError = TEXT("Could not serialize an App Server request.");
        return false;
    }
    return Process.WriteJsonLine(Serialized, OutError);
}

int64 FCinderLinkAppServerClient::SendRequest(
    const FString& Method,
    const TSharedRef<FJsonObject>& Params,
    EPendingRequest Kind,
    FString& OutError)
{
    const int64 Id = NextRequestId++;
    TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("method"), Method);
    Request->SetNumberField(TEXT("id"), static_cast<double>(Id));
    Request->SetObjectField(TEXT("params"), Params);
    if (!SendObject(Request, OutError))
    {
        return 0;
    }
    PendingRequests.Add(Id, Kind);
    return Id;
}

void FCinderLinkAppServerClient::SendNotification(const FString& Method, const TSharedRef<FJsonObject>& Params)
{
    TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
    Notification->SetStringField(TEXT("method"), Method);
    Notification->SetObjectField(TEXT("params"), Params);
    FString IgnoredError;
    SendObject(Notification, IgnoredError);
}

void FCinderLinkAppServerClient::SendInitialize()
{
    TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
    ClientInfo->SetStringField(TEXT("name"), TEXT("cinderlink"));
    ClientInfo->SetStringField(TEXT("title"), TEXT("CinderLink"));
    ClientInfo->SetStringField(TEXT("version"), TEXT("0.2.1"));

    TSharedRef<FJsonObject> Params = MakeParams();
    Params->SetObjectField(TEXT("clientInfo"), ClientInfo);
    TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    Capabilities->SetBoolField(TEXT("experimentalApi"), true);
    Params->SetObjectField(TEXT("capabilities"), Capabilities);

    FString Error;
    if (SendRequest(TEXT("initialize"), Params, EPendingRequest::Initialize, Error) == 0)
    {
        Emit(ECinderLinkMessageKind::Error, Error);
    }
}

void FCinderLinkAppServerClient::SendPermissionProfileList()
{
    TSharedRef<FJsonObject> Params = MakeParams();
    Params->SetStringField(TEXT("cwd"), ProjectRoot);

    FString Error;
    if (SendRequest(
            TEXT("permissionProfile/list"),
            Params,
            EPendingRequest::PermissionProfiles,
            Error) == 0)
    {
        Emit(ECinderLinkMessageKind::Error, Error);
    }
}

void FCinderLinkAppServerClient::SendMcpServerStatusList(
    EPendingRequest Kind,
    const FString& ForThreadId)
{
    TSharedRef<FJsonObject> Params = MakeParams();
    Params->SetStringField(TEXT("detail"), TEXT("toolsAndAuthOnly"));
    if (!ForThreadId.IsEmpty())
    {
        Params->SetStringField(TEXT("threadId"), ForThreadId);
    }

    FString Error;
    if (SendRequest(TEXT("mcpServerStatus/list"), Params, Kind, Error) == 0)
    {
        Emit(ECinderLinkMessageKind::Error, Error);
    }
}

void FCinderLinkAppServerClient::SendThreadStart()
{
    TSharedRef<FJsonObject> Params = MakeParams();
    Params->SetStringField(TEXT("cwd"), ProjectRoot);
    Params->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
    Params->SetStringField(TEXT("approvalsReviewer"), TEXT("user"));
    Params->SetStringField(TEXT("permissions"), ReadPermissionProfile);
    Params->SetBoolField(TEXT("ephemeral"), true);
    Params->SetObjectField(TEXT("config"), BuildIsolationConfig(McpServerNames));
    Params->SetArrayField(TEXT("dynamicTools"), FCinderLinkEditorTools::BuildToolSpecs());
    TArray<TSharedPtr<FJsonValue>> RuntimeRoots;
    RuntimeRoots.Add(MakeShared<FJsonValueString>(ProjectRoot));
    Params->SetArrayField(TEXT("runtimeWorkspaceRoots"), RuntimeRoots);
    Params->SetStringField(TEXT("serviceName"), TEXT("cinderlink"));

    FString Error;
    if (SendRequest(TEXT("thread/start"), Params, EPendingRequest::ThreadStart, Error) == 0)
    {
        Emit(ECinderLinkMessageKind::Error, Error);
    }
}

void FCinderLinkAppServerClient::HandleStdOutLine(const FString& Line)
{
    TSharedPtr<FJsonObject> Message;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
    if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
    {
        Emit(ECinderLinkMessageKind::Warning, TEXT("Ignored a malformed App Server message."));
        return;
    }

    double NumericId = 0.0;
    const bool bHasId = Message->TryGetNumberField(TEXT("id"), NumericId);
    FString Method;
    const bool bHasMethod = Message->TryGetStringField(TEXT("method"), Method);

    if (bHasId && bHasMethod)
    {
        HandleServerRequest(Message, Method, static_cast<int64>(NumericId));
    }
    else if (bHasId)
    {
        HandleResponse(Message, static_cast<int64>(NumericId));
    }
    else if (bHasMethod)
    {
        HandleNotification(Message, Method);
    }
}

void FCinderLinkAppServerClient::HandleStdErrLine(const FString& Line)
{
    if (!Line.IsEmpty())
    {
        Emit(ECinderLinkMessageKind::Warning, TruncateForUi(Line, 1024));
    }
}

void FCinderLinkAppServerClient::HandleResponse(const TSharedPtr<FJsonObject>& Message, int64 Id)
{
    const EPendingRequest* Pending = PendingRequests.Find(Id);
    if (Pending == nullptr)
    {
        return;
    }
    const EPendingRequest Kind = *Pending;
    PendingRequests.Remove(Id);

    TSharedPtr<FJsonObject> ErrorObject;
    if (GetObjectField(Message, TEXT("error"), ErrorObject))
    {
        bTurnInProgress = Kind == EPendingRequest::TurnStart ? false : bTurnInProgress;
        if (Kind == EPendingRequest::TurnStart)
        {
            bActiveTurnAllowsEditorActions = false;
            ActiveTurnId.Reset();
        }
        if (Kind != EPendingRequest::TurnStart && Kind != EPendingRequest::Interrupt)
        {
            bIsolationReady = false;
        }
        const FString ErrorText = ReadString(ErrorObject, TEXT("message"));
        Emit(
            ECinderLinkMessageKind::Error,
            ErrorText.IsEmpty() ? TEXT("Codex App Server rejected a request.") : TruncateForUi(ErrorText));
        return;
    }

    if (Kind == EPendingRequest::Initialize)
    {
        SendNotification(TEXT("initialized"), MakeParams());
        Emit(ECinderLinkMessageKind::Status, TEXT("Disabling external tools and validating isolation..."));
        SendMcpServerStatusList(EPendingRequest::InitialMcpInventory);
        return;
    }

    if (Kind == EPendingRequest::InitialMcpInventory)
    {
        TSharedPtr<FJsonObject> Result;
        const TArray<TSharedPtr<FJsonValue>>* Servers = nullptr;
        if (!GetObjectField(Message, TEXT("result"), Result) ||
            !Result->TryGetArrayField(TEXT("data"), Servers) || Servers == nullptr ||
            Servers->Num() > MaximumMcpServerCount)
        {
            Emit(ECinderLinkMessageKind::Error, TEXT("Could not enumerate external tool servers safely."));
            return;
        }

        McpServerNames.Reset();
        for (const TSharedPtr<FJsonValue>& ServerValue : *Servers)
        {
            const TSharedPtr<FJsonObject> Server = ServerValue.IsValid() ? ServerValue->AsObject() : nullptr;
            const FString Name = ReadString(Server, TEXT("name"));
            if (Name.IsEmpty() || Name.Len() > MaximumMcpServerNameLength)
            {
                Emit(ECinderLinkMessageKind::Error, TEXT("App Server returned an invalid external tool name."));
                return;
            }
            McpServerNames.AddUnique(Name);
        }

        SendPermissionProfileList();
        return;
    }

    if (Kind == EPendingRequest::PermissionProfiles)
    {
        TSharedPtr<FJsonObject> Result;
        const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
        if (GetObjectField(Message, TEXT("result"), Result) &&
            Result->TryGetArrayField(TEXT("data"), Profiles) && Profiles != nullptr)
        {
            for (const TSharedPtr<FJsonValue>& ProfileValue : *Profiles)
            {
                const TSharedPtr<FJsonObject> Profile = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
                bool bAllowed = false;
                if (!Profile.IsValid() || !Profile->TryGetBoolField(TEXT("allowed"), bAllowed) || !bAllowed)
                {
                    continue;
                }
                const FString ProfileId = ReadString(Profile, TEXT("id"));
                if (ProfileId == ReadPermissionProfile)
                {
                    bReadPermissionProfileReady = true;
                }
                else if (ProfileId == EditPermissionProfile)
                {
                    bEditPermissionProfileReady = true;
                }
            }
        }

        if (!bReadPermissionProfileReady || !bEditPermissionProfileReady)
        {
            Emit(
                ECinderLinkMessageKind::Error,
                TEXT("Codex did not accept CinderLink's project-only permission profiles. Update Codex and provision the elevated Windows sandbox."));
            return;
        }
        SendThreadStart();
        return;
    }

    if (Kind == EPendingRequest::ThreadStart)
    {
        TSharedPtr<FJsonObject> Result;
        TSharedPtr<FJsonObject> Thread;
        FString CandidateThreadId;
        if (GetObjectField(Message, TEXT("result"), Result) &&
            GetObjectField(Result, TEXT("thread"), Thread))
        {
            CandidateThreadId = ReadString(Thread, TEXT("id"));
        }

        TSharedPtr<FJsonObject> ActiveProfile;
        const bool bCorrectProfile = GetObjectField(Result, TEXT("activePermissionProfile"), ActiveProfile) &&
            ReadString(ActiveProfile, TEXT("id")) == ReadPermissionProfile;

        bool bCorrectRoot = false;
        const TArray<TSharedPtr<FJsonValue>>* RuntimeRoots = nullptr;
        if (Result.IsValid() && Result->TryGetArrayField(TEXT("runtimeWorkspaceRoots"), RuntimeRoots) &&
            RuntimeRoots != nullptr && RuntimeRoots->Num() == 1)
        {
            FString ActualRoot = NormalizeRoot((*RuntimeRoots)[0]->AsString());
            bCorrectRoot = ActualRoot.Equals(ProjectRoot, ESearchCase::IgnoreCase);
        }

        if (CandidateThreadId.IsEmpty() || !bCorrectProfile || !bCorrectRoot)
        {
            ThreadId.Reset();
            bIsolationReady = false;
            Emit(
                ECinderLinkMessageKind::Error,
                TEXT("App Server did not activate the exact CinderLink project boundary."));
        }
        else
        {
            ThreadId = CandidateThreadId;
            SendMcpServerStatusList(EPendingRequest::ThreadMcpVerification, ThreadId);
        }
        return;
    }

    if (Kind == EPendingRequest::ThreadMcpVerification)
    {
        TSharedPtr<FJsonObject> Result;
        const TArray<TSharedPtr<FJsonValue>>* Servers = nullptr;
        if (!GetObjectField(Message, TEXT("result"), Result) ||
            !Result->TryGetArrayField(TEXT("data"), Servers) || Servers == nullptr)
        {
            ThreadId.Reset();
            Emit(ECinderLinkMessageKind::Error, TEXT("Could not verify that external tools are disabled."));
            return;
        }

        for (const TSharedPtr<FJsonValue>& ServerValue : *Servers)
        {
            const TSharedPtr<FJsonObject> Server = ServerValue.IsValid() ? ServerValue->AsObject() : nullptr;
            TSharedPtr<FJsonObject> Tools;
            const FString RuntimeStatus = ReadString(Server, TEXT("runtimeStatus"));
            const bool bHasTools = GetObjectField(Server, TEXT("tools"), Tools) && Tools->Values.Num() > 0;
            if (!Server.IsValid() || RuntimeStatus != TEXT("disabled") || bHasTools)
            {
                ThreadId.Reset();
                bIsolationReady = false;
                Emit(ECinderLinkMessageKind::Error, TEXT("An external MCP, app, or plugin tool remained active. CinderLink stopped before sending a prompt."));
                return;
            }
        }

        bIsolationReady = true;
        Emit(ECinderLinkMessageKind::Status, TEXT("Connected. The project-only boundary is active; external tools are disabled."));
        return;
    }

    if (Kind == EPendingRequest::TurnStart)
    {
        TSharedPtr<FJsonObject> Result;
        TSharedPtr<FJsonObject> Turn;
        if (!GetObjectField(Message, TEXT("result"), Result) ||
            !GetObjectField(Result, TEXT("turn"), Turn))
        {
            bTurnInProgress = false;
            bActiveTurnAllowsEditorActions = false;
            ActiveTurnId.Reset();
            Emit(ECinderLinkMessageKind::Error, TEXT("App Server returned an invalid turn identity."));
            return;
        }
        ActiveTurnId = ReadString(Turn, TEXT("id"));
        if (ActiveTurnId.IsEmpty())
        {
            bTurnInProgress = false;
            bActiveTurnAllowsEditorActions = false;
            Emit(ECinderLinkMessageKind::Error, TEXT("App Server omitted the active turn identity."));
        }
        return;
    }
}

void FCinderLinkAppServerClient::HandleNotification(
    const TSharedPtr<FJsonObject>& Message,
    const FString& Method)
{
    TSharedPtr<FJsonObject> Params;
    GetObjectField(Message, TEXT("params"), Params);

    if (Method == TEXT("item/agentMessage/delta"))
    {
        const FString Delta = ReadString(Params, TEXT("delta"));
        if (!Delta.IsEmpty())
        {
            Emit(ECinderLinkMessageKind::AssistantDelta, Delta);
        }
        return;
    }

    if (Method == TEXT("item/completed"))
    {
        TSharedPtr<FJsonObject> Item;
        if (!GetObjectField(Params, TEXT("item"), Item))
        {
            return;
        }

        const FString Type = ReadString(Item, TEXT("type"));
        if (Type == TEXT("agentMessage"))
        {
            const FString Text = ReadString(Item, TEXT("text"));
            if (!Text.IsEmpty())
            {
                Emit(ECinderLinkMessageKind::AssistantFinal, Text);
            }
        }
        else if (Type == TEXT("commandExecution"))
        {
            const FString Command = ReadString(Item, TEXT("command"));
            const FString Status = ReadString(Item, TEXT("status"));
            Emit(
                ECinderLinkMessageKind::Command,
                FString::Printf(TEXT("Command %s: %s"), *Status, *TruncateForUi(Command, 1024)));
        }
        else if (Type == TEXT("fileChange"))
        {
            FString Summary = TEXT("File changes completed.");
            const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
            if (Item->TryGetArrayField(TEXT("changes"), Changes) && Changes != nullptr)
            {
                TArray<FString> Paths;
                for (const TSharedPtr<FJsonValue>& ChangeValue : *Changes)
                {
                    const TSharedPtr<FJsonObject> Change = ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
                    const FString Path = ReadString(Change, TEXT("path"));
                    if (!Path.IsEmpty())
                    {
                        Paths.Add(Path);
                    }
                }
                if (!Paths.IsEmpty())
                {
                    Summary = TEXT("Changed: ") + FString::Join(Paths, TEXT(", "));
                }
            }
            Emit(ECinderLinkMessageKind::FileChange, TruncateForUi(Summary));
        }
        return;
    }

    if (Method == TEXT("item/started"))
    {
        TSharedPtr<FJsonObject> Item;
        if (GetObjectField(Params, TEXT("item"), Item) && ReadString(Item, TEXT("type")) == TEXT("commandExecution"))
        {
            Emit(
                ECinderLinkMessageKind::Command,
                TEXT("Requested command: ") + TruncateForUi(ReadString(Item, TEXT("command")), 1024));
        }
        return;
    }

    if (Method == TEXT("turn/completed"))
    {
        TSharedPtr<FJsonObject> Turn;
        GetObjectField(Params, TEXT("turn"), Turn);
        const FString ReportedTurnId = ReadString(Turn, TEXT("id"));
        if (!ActiveTurnId.IsEmpty() && !ReportedTurnId.IsEmpty() && ReportedTurnId != ActiveTurnId)
        {
            Emit(ECinderLinkMessageKind::Warning, TEXT("Ignored completion for a stale Codex turn."));
            return;
        }
        bTurnInProgress = false;
        bActiveTurnAllowsEditorActions = false;
        ActiveTurnId.Reset();
        FString Status = TEXT("completed");
        if (Turn.IsValid())
        {
            const FString Reported = ReadString(Turn, TEXT("status"));
            if (!Reported.IsEmpty())
            {
                Status = Reported;
            }
        }
        Emit(ECinderLinkMessageKind::TurnCompleted, Status);
        return;
    }

    if (Method == TEXT("warning") || Method == TEXT("configWarning"))
    {
        FString Warning = ReadString(Params, TEXT("message"));
        if (Warning.IsEmpty())
        {
            Warning = ReadString(Params, TEXT("summary"));
        }
        if (!Warning.IsEmpty())
        {
            Emit(ECinderLinkMessageKind::Warning, TruncateForUi(Warning));
        }
        return;
    }

    if (Method == TEXT("error"))
    {
        TSharedPtr<FJsonObject> Error;
        GetObjectField(Params, TEXT("error"), Error);
        const FString ErrorText = ReadString(Error, TEXT("message"));
        Emit(
            ECinderLinkMessageKind::Error,
            ErrorText.IsEmpty() ? TEXT("The active Codex turn failed.") : TruncateForUi(ErrorText));
    }
}

void FCinderLinkAppServerClient::HandleServerRequest(
    const TSharedPtr<FJsonObject>& Message,
    const FString& Method,
    int64 Id)
{
    TSharedPtr<FJsonObject> Params;
    GetObjectField(Message, TEXT("params"), Params);

    if (Method == TEXT("item/commandExecution/requestApproval"))
    {
        SendApprovalDecision(Id, TEXT("decline"));
        Emit(ECinderLinkMessageKind::Warning, TEXT("Blocked a command that requested permission beyond the active project profile."));
        return;
    }

    if (Method == TEXT("item/fileChange/requestApproval"))
    {
        SendApprovalDecision(Id, TEXT("decline"));
        Emit(ECinderLinkMessageKind::Warning, TEXT("Blocked a file change outside the active project profile."));
        return;
    }

    if (Method == TEXT("item/permissions/requestApproval"))
    {
        SendEmptyPermissionGrant(Id);
        Emit(ECinderLinkMessageKind::Warning, TEXT("Declined a request for additional filesystem or network permissions."));
        return;
    }

    if (Method == TEXT("item/tool/call"))
    {
        const FString RequestedThreadId = ReadString(Params, TEXT("threadId"));
        const FString RequestedTurnId = ReadString(Params, TEXT("turnId"));
        const FString ToolName = ReadString(Params, TEXT("tool"));
        TSharedPtr<FJsonObject> Arguments;
        const bool bArgumentsObject = GetObjectField(Params, TEXT("arguments"), Arguments);
        if (!bTurnInProgress || ActiveTurnId.IsEmpty() || RequestedThreadId != ThreadId ||
            RequestedTurnId != ActiveTurnId || !bArgumentsObject ||
            !FCinderLinkEditorTools::IsKnownTool(ToolName))
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            TSharedRef<FJsonObject> TextItem = MakeShared<FJsonObject>();
            TextItem->SetStringField(TEXT("type"), TEXT("inputText"));
            TextItem->SetStringField(TEXT("text"), TEXT("{\"error\":\"CinderLink rejected the tool context.\"}"));
            TArray<TSharedPtr<FJsonValue>> ContentItems;
            ContentItems.Add(MakeShared<FJsonValueObject>(TextItem));
            Result->SetArrayField(TEXT("contentItems"), ContentItems);
            Result->SetBoolField(TEXT("success"), false);
            SendDynamicToolResponse(Id, Result);
            Emit(ECinderLinkMessageKind::Warning, TEXT("Rejected an Unreal Editor tool outside the active verified turn."));
            return;
        }

        FString Summary;
        TSharedRef<FJsonObject> Result = FCinderLinkEditorTools::Execute(
            ToolName,
            Arguments,
            ProjectRoot,
            bActiveTurnAllowsEditorActions,
            Summary);
        SendDynamicToolResponse(Id, Result);
        Emit(
            Result->GetBoolField(TEXT("success")) ? ECinderLinkMessageKind::EditorAction : ECinderLinkMessageKind::Warning,
            ToolName + TEXT(": ") + Summary);
        return;
    }

    SendMethodNotSupported(Id, Method);
    Emit(ECinderLinkMessageKind::Warning, TEXT("Declined an unsupported App Server interaction request."));
}

void FCinderLinkAppServerClient::SendApprovalDecision(int64 Id, const FString& Decision)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("decision"), Decision);
    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("id"), static_cast<double>(Id));
    Response->SetObjectField(TEXT("result"), Result);
    FString IgnoredError;
    SendObject(Response, IgnoredError);
}

void FCinderLinkAppServerClient::SendEmptyPermissionGrant(int64 Id)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetObjectField(TEXT("permissions"), MakeShared<FJsonObject>());
    Result->SetStringField(TEXT("scope"), TEXT("turn"));
    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("id"), static_cast<double>(Id));
    Response->SetObjectField(TEXT("result"), Result);
    FString IgnoredError;
    SendObject(Response, IgnoredError);
}

void FCinderLinkAppServerClient::SendDynamicToolResponse(
    int64 Id,
    const TSharedRef<FJsonObject>& Result)
{
    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("id"), static_cast<double>(Id));
    Response->SetObjectField(TEXT("result"), Result);
    FString IgnoredError;
    SendObject(Response, IgnoredError);
}

void FCinderLinkAppServerClient::SendMethodNotSupported(int64 Id, const FString& Method)
{
    TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
    Error->SetNumberField(TEXT("code"), -32601.0);
    Error->SetStringField(TEXT("message"), TEXT("CinderLink does not expose this interaction."));
    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("id"), static_cast<double>(Id));
    Response->SetObjectField(TEXT("error"), Error);
    FString IgnoredError;
    SendObject(Response, IgnoredError);
}

void FCinderLinkAppServerClient::Emit(ECinderLinkMessageKind Kind, const FString& Text)
{
    if (!Text.IsEmpty())
    {
        FCinderLinkMessage Message;
        Message.Kind = Kind;
        Message.Text = Text;
        OnMessage.Broadcast(Message);
    }
}

FString FCinderLinkAppServerClient::ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Result;
    if (Object.IsValid())
    {
        Object->TryGetStringField(Field, Result);
    }
    return Result;
}

int64 FCinderLinkAppServerClient::ReadRequestId(const TSharedPtr<FJsonObject>& Object)
{
    double Value = 0.0;
    return Object.IsValid() && Object->TryGetNumberField(TEXT("id"), Value)
        ? static_cast<int64>(Value)
        : 0;
}

FString FCinderLinkAppServerClient::NormalizeRoot(const FString& Path)
{
    FString Result = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeDirectoryName(Result);
    return Result;
}
