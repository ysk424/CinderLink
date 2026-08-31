// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkProtocol.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

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

#endif
