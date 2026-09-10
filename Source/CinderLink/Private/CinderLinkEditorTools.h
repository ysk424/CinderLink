// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Dispatches the bounded Editor tools and the separately enabled Python
 * authoring tools. All calls arrive through the App Server stdio pipe.
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
        FString& OutSummary,
        bool bAllowPythonAuthoring = false);
};
