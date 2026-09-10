// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkProtocol.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SCinderLinkPanel.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCinderLinkReadOnlyProfileTest,
    "CinderLink.Security.ReadOnlyProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkReadOnlyProfileTest::RunTest(const FString& Parameters)
{
    TestEqual(
        TEXT("Read-only turns select the restricted read profile"),
        FCinderLinkAppServerClient::GetPermissionProfileName(false),
        FString(TEXT("cinderlink-project-read")));

    const TArray<FString> Servers = {TEXT("example-local"), TEXT("example-remote")};
    const TSharedRef<FJsonObject> Config = FCinderLinkAppServerClient::BuildIsolationConfig(Servers);
    const TSharedPtr<FJsonObject> McpServers = Config->GetObjectField(TEXT("mcp_servers"));
    TestEqual(TEXT("Every discovered MCP server is represented"), McpServers->Values.Num(), Servers.Num());
    for (const FString& Name : Servers)
    {
        TestFalse(TEXT("MCP server is disabled"), McpServers->GetObjectField(Name)->GetBoolField(TEXT("enabled")));
    }

    const TSharedPtr<FJsonObject> Tools = Config->GetObjectField(TEXT("tools"));
    TestFalse(TEXT("Hosted web search is disabled"), Tools->GetBoolField(TEXT("web_search")));
    TestFalse(TEXT("Image file attachment is disabled"), Tools->GetBoolField(TEXT("view_image")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCinderLinkEditProfileTest,
    "CinderLink.Security.EditProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkEditProfileTest::RunTest(const FString& Parameters)
{
    TestEqual(
        TEXT("Edit turns select the project-only write profile"),
        FCinderLinkAppServerClient::GetPermissionProfileName(true),
        FString(TEXT("cinderlink-project-edit")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkSteeringTest, "CinderLink.Protocol.Steering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkSteeringTest::RunTest(const FString& Parameters)
{
    FCinderLinkAppServerClient Client;
    FString Error;
    TestFalse(TEXT("Disconnected updates cannot be sent"), Client.SteerTurn(TEXT("saved"), Error));
    TestFalse(TEXT("Unsent updates do not create a pending delivery"), Client.HasPendingUpdate());

    TArray<FCinderLinkMessage> Messages;
    Client.OnMessage.AddLambda([&Messages](const FCinderLinkMessage& Message) { Messages.Add(Message); });
    Client.bTurnInProgress = true;
    Client.bIsolationReady = true;
    Client.ActiveTurnId = TEXT("turn-current");
    Client.bActiveTurnAllowsPythonAuthoring = false;
    Client.PendingSteerText = TEXT("saved");
    Client.PendingSteerTurnId = TEXT("turn-current");
    Client.PendingRequests.Add(10, FCinderLinkAppServerClient::EPendingRequest::TurnSteer);
    auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("turnId"), TEXT("turn-current"));
    auto Response = MakeShared<FJsonObject>(); Response->SetObjectField(TEXT("result"), Result);
    Client.HandleResponse(Response, 10);
    TestTrue(TEXT("Matching acknowledgement confirms delivery"), Messages.Last().Kind == ECinderLinkMessageKind::UpdateAccepted);
    TestEqual(TEXT("Confirmed text is delivered to the panel"), Messages.Last().Text, FString(TEXT("saved")));
    TestFalse(TEXT("Acceptance clears pending input"), Client.HasPendingUpdate());
    TestFalse(TEXT("A follow-up cannot grant Python authorization"), Client.bActiveTurnAllowsPythonAuthoring);

    Client.PendingSteerText = TEXT("keep this draft");
    Client.PendingSteerTurnId = TEXT("turn-current");
    Client.PendingRequests.Add(11, FCinderLinkAppServerClient::EPendingRequest::TurnSteer);
    Result->SetStringField(TEXT("turnId"), TEXT("turn-wrong"));
    Client.HandleResponse(Response, 11);
    TestTrue(TEXT("Wrong-turn acknowledgement is rejected"), Messages.Last().Kind == ECinderLinkMessageKind::UpdateRejected);
    TestTrue(TEXT("Failed delivery does not disconnect the thread"), Client.bIsolationReady);
    TestTrue(TEXT("Failed delivery does not end the active turn"), Client.bTurnInProgress);

    Client.PendingSteerText = TEXT("keep this draft");
    Client.PendingSteerTurnId = TEXT("turn-current");
    Client.PendingRequests.Add(12, FCinderLinkAppServerClient::EPendingRequest::TurnSteer);
    Response->RemoveField(TEXT("result"));
    Response->SetObjectField(TEXT("error"), MakeShared<FJsonObject>());
    Client.HandleResponse(Response, 12);
    TestTrue(TEXT("Server rejection is reported for retry"), Messages.Last().Kind == ECinderLinkMessageKind::UpdateRejected);
    TestTrue(TEXT("Server rejection preserves isolation readiness"), Client.bIsolationReady);

    TSharedRef<SCinderLinkPanel> Panel = SNew(SCinderLinkPanel).AutoConnect(false);
    Panel->InputBox->SetText(FText::FromString(TEXT("keep this draft")));
    Panel->HandleMessage(Messages.Last());
    TestEqual(TEXT("Rejected updates stay in the editable input"), Panel->InputBox->GetText().ToString(), FString(TEXT("keep this draft")));
    Panel->HandleMessage({ECinderLinkMessageKind::AssistantDelta, TEXT("partial")});
    Panel->HandleMessage({ECinderLinkMessageKind::UpdateAccepted, TEXT("saved")});
    TestTrue(TEXT("Accepted updates clear the input"), Panel->InputBox->GetText().IsEmpty());
    Panel->HandleMessage({ECinderLinkMessageKind::AssistantDelta, TEXT(" continuation")});
    Panel->HandleMessage({ECinderLinkMessageKind::AssistantFinal, TEXT("complete response")});
    TestTrue(TEXT("Final response does not erase a user update during streaming"), Panel->Transcript.Contains(TEXT("You (update): saved")));
    TestTrue(TEXT("Final response replaces its streamed fragment"), Panel->Transcript.Contains(TEXT("Assistant: complete response")));
    TestFalse(TEXT("Partial assistant text is removed"), Panel->Transcript.Contains(TEXT("partial")));
    return true;
}

#endif
