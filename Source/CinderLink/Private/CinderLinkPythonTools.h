// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Explicitly enabled authoring tools. Python runs with Unreal Editor's host permissions. */
class FCinderLinkPythonTools
{
public:
    static TArray<TSharedPtr<FJsonValue>> BuildToolSpecs();
    static bool IsKnownTool(const FString& Name);
    static bool IsMutationTool(const FString& Name);
    static TSharedRef<FJsonObject> Execute(
        const FString& Name, const TSharedPtr<FJsonObject>& Arguments,
        const FString& ProjectRoot, bool bAuthoringEnabled, FString& OutSummary);
};
