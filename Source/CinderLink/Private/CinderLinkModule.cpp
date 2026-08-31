// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkModule.h"

#include "SCinderLinkPanel.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FCinderLinkModule"

namespace
{
    const FName CinderLinkTabName(TEXT("CinderLink"));
}

void FCinderLinkModule::StartupModule()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
            CinderLinkTabName,
            FOnSpawnTab::CreateRaw(this, &FCinderLinkModule::SpawnCinderLinkTab))
        .SetDisplayName(LOCTEXT("CinderLinkTabTitle", "CinderLink"))
        .SetTooltipText(LOCTEXT("CinderLinkTabTooltip", "Open the local-first CinderLink agent panel."))
        .SetMenuType(ETabSpawnerMenuType::Hidden);

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCinderLinkModule::RegisterMenus));
}

void FCinderLinkModule::ShutdownModule()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CinderLinkTabName);
}

void FCinderLinkModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
    FToolMenuSection& Section = WindowMenu->FindOrAddSection(TEXT("WindowLayout"));
    Section.AddMenuEntry(
        TEXT("OpenCinderLink"),
        LOCTEXT("OpenCinderLinkLabel", "CinderLink"),
        LOCTEXT("OpenCinderLinkTooltip", "Open the CinderLink agent panel."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(this, &FCinderLinkModule::OpenCinderLinkTab)));
}

void FCinderLinkModule::OpenCinderLinkTab()
{
    FGlobalTabmanager::Get()->TryInvokeTab(CinderLinkTabName);
}

TSharedRef<SDockTab> FCinderLinkModule::SpawnCinderLinkTab(const FSpawnTabArgs& SpawnTabArgs)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SCinderLinkPanel)
        ];
}

IMPLEMENT_MODULE(FCinderLinkModule, CinderLink)

#undef LOCTEXT_NAMESPACE
