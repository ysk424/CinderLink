// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "Modules/ModuleManager.h"

class FSpawnTabArgs;
class SDockTab;

class FCinderLinkModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    void OpenCinderLinkTab();
    TSharedRef<SDockTab> SpawnCinderLinkTab(const FSpawnTabArgs& SpawnTabArgs);
};
