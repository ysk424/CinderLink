// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Executes the small, validated set of Unreal Editor actions exposed to Codex.
 * This layer never opens a listener and never executes arbitrary script or
 * console text. All calls arrive through the existing App Server stdio pipe.
 */
class FCinderLinkEditorTools
{
public:
    static TArray<TSharedPtr<FJsonValue>> BuildToolSpecs();
    static bool IsKnownTool(const FString& ToolName);
    static bool IsMutationTool(const FString& ToolName);

    /** Builds a DynamicToolCallResponse-compatible result object. */
    static TSharedRef<FJsonObject> Execute(
        const FString& ToolName,
        const TSharedPtr<FJsonObject>& Arguments,
        const FString& ProjectRoot,
        bool bAllowEditorActions,
        FString& OutSummary);
};
