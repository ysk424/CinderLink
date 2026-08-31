// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"

#include "CinderLinkProcess.h"

enum class ECinderLinkMessageKind : uint8
{
    Status,
    AssistantDelta,
    AssistantFinal,
    Command,
    FileChange,
    EditorAction,
    Warning,
    Error,
    TurnCompleted
};

struct FCinderLinkMessage
{
    ECinderLinkMessageKind Kind = ECinderLinkMessageKind::Status;
    FString Text;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FCinderLinkMessageDelegate, const FCinderLinkMessage&);

/** Minimal, fail-closed client for the Codex App Server JSONL protocol. */
class FCinderLinkAppServerClient
{
public:
    FCinderLinkAppServerClient();
    ~FCinderLinkAppServerClient();

    FCinderLinkAppServerClient(const FCinderLinkAppServerClient&) = delete;
    FCinderLinkAppServerClient& operator=(const FCinderLinkAppServerClient&) = delete;

    bool Connect(const FString& ExecutablePath, const FString& ProjectRoot, FString& OutError);
    void Disconnect();
    bool StartNewThread(FString& OutError);
    bool SendTurn(
        const FString& Text,
        bool bAllowProjectEdits,
        bool bAllowEditorActions,
        FString& OutError);
    bool InterruptTurn(FString& OutError);

    bool IsProcessRunning() const { return Process.IsRunning(); }
    bool IsReady() const { return Process.IsRunning() && !ThreadId.IsEmpty() && bIsolationReady; }
    bool IsTurnInProgress() const { return bTurnInProgress; }
    const FString& GetProjectRoot() const { return ProjectRoot; }
    const FString& GetThreadId() const { return ThreadId; }

    FCinderLinkMessageDelegate OnMessage;

    static FString GetPermissionProfileName(bool bAllowProjectEdits);
    static TSharedRef<FJsonObject> BuildIsolationConfig(const TArray<FString>& McpServerNames);

private:
    enum class EPendingRequest : uint8
    {
        Initialize,
        InitialMcpInventory,
        PermissionProfiles,
        ThreadStart,
        ThreadMcpVerification,
        TurnStart,
        Interrupt
    };

    bool Tick(float DeltaTime);
    bool SendObject(const TSharedRef<FJsonObject>& Object, FString& OutError);
    int64 SendRequest(const FString& Method, const TSharedRef<FJsonObject>& Params, EPendingRequest Kind, FString& OutError);
    void SendNotification(const FString& Method, const TSharedRef<FJsonObject>& Params);
    void SendInitialize();
    void SendMcpServerStatusList(EPendingRequest Kind, const FString& ForThreadId = FString());
    void SendPermissionProfileList();
    void SendThreadStart();
    void HandleStdOutLine(const FString& Line);
    void HandleStdErrLine(const FString& Line);
    void HandleResponse(const TSharedPtr<FJsonObject>& Message, int64 Id);
    void HandleNotification(const TSharedPtr<FJsonObject>& Message, const FString& Method);
    void HandleServerRequest(const TSharedPtr<FJsonObject>& Message, const FString& Method, int64 Id);
    void SendApprovalDecision(int64 Id, const FString& Decision);
    void SendEmptyPermissionGrant(int64 Id);
    void SendDynamicToolResponse(int64 Id, const TSharedRef<FJsonObject>& Result);
    void SendMethodNotSupported(int64 Id, const FString& Method);
    void Emit(ECinderLinkMessageKind Kind, const FString& Text);

    static FString ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
    static int64 ReadRequestId(const TSharedPtr<FJsonObject>& Object);
    static FString NormalizeRoot(const FString& Path);

    FCinderLinkProcess Process;
    FTSTicker::FDelegateHandle TickerHandle;
    TMap<int64, EPendingRequest> PendingRequests;
    int64 NextRequestId = 1;
    FString ProjectRoot;
    FString ThreadId;
    FString ActiveTurnId;
    TArray<FString> McpServerNames;
    bool bTurnInProgress = false;
    bool bReportedProcessExit = false;
    bool bReadPermissionProfileReady = false;
    bool bEditPermissionProfileReady = false;
    bool bIsolationReady = false;
    bool bActiveTurnAllowsEditorActions = false;
};
