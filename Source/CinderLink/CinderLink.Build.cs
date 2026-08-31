// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

using UnrealBuildTool;

public class CinderLink : ModuleRules
{
    public CinderLink(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "CoreUObject",
                "Engine",
                "InputCore",
                "Json",
                "Projects",
                "Slate",
                "SlateCore",
                "ToolMenus",
                "UnrealEd"
            }
        );
    }
}
