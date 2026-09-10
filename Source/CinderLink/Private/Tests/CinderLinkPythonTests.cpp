// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkEditorTools.h"
#include "CinderLinkProtocol.h"
#include "SCinderLinkPanel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Widgets/Input/SCheckBox.h"

namespace
{
    TSharedRef<FJsonObject> Arguments(const FString& Code, const FString& Folder = FString())
    {
        auto Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("label"), TEXT("automation"));
        Args->SetStringField(TEXT("code"), Code);
        TArray<TSharedPtr<FJsonValue>> Paths;
        if (!Folder.IsEmpty()) Paths.Add(MakeShared<FJsonValueString>(Folder));
        Args->SetArrayField(TEXT("backup_paths"), Paths);
        return Args;
    }

    TSharedPtr<FJsonObject> Call(const FString& Tool, const TSharedPtr<FJsonObject>& Args,
                               bool bEditor = true, bool bPython = true)
    {
        FString Summary;
        auto Response = FCinderLinkEditorTools::Execute(Tool, Args, FPaths::ProjectDir(), bEditor, Summary, bPython);
        TSharedPtr<FJsonObject> Payload;
        const auto& Items = Response->GetArrayField(TEXT("contentItems"));
        if (!Items.IsEmpty())
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Items[0]->AsObject()->GetStringField(TEXT("text"))), Payload);
        // Bounded-tool errors do not carry payload.success; normalize it for assertions.
        if (Payload.IsValid()) Payload->SetBoolField(TEXT("success"), Response->GetBoolField(TEXT("success")));
        return Payload;
    }

    FString UniqueFolder()
    {
        return TEXT("/Game/CinderLinkPythonTests/T") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkPythonPolicyTest, "CinderLink.Security.PythonAuthoringPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkPythonPolicyTest::RunTest(const FString& Parameters)
{
    auto Args = Arguments(TEXT("raise AssertionError('must never execute')"));
    TestFalse(TEXT("Normal Editor consent cannot authorize Python"), Call(TEXT("ue_python_execute"), Args, true, false)->GetBoolField(TEXT("success")));
    TestFalse(TEXT("Python also requires Editor consent"), Call(TEXT("ue_python_execute"), Args, false, true)->GetBoolField(TEXT("success")));
    auto Status = Call(TEXT("ue_python_status"), MakeShared<FJsonObject>(), false, false);
    TestTrue(TEXT("Status is readable with no write consent"), Status->GetBoolField(TEXT("success")));
    TestFalse(TEXT("Normal mode reports authoring disabled"), Status->GetBoolField(TEXT("authoring_enabled")));
    TestTrue(TEXT("UE Python dependency is initialized"), Status->GetBoolField(TEXT("python_ready")));
    TestFalse(TEXT("No tool enables Python authoring"), FCinderLinkEditorTools::IsKnownTool(TEXT("ue_python_enable")));
    for (const FString& Path : {TEXT("/Game/../Outside"), TEXT("/Game/Folder/"), TEXT("/Engine"), TEXT("/Game/CON")})
    {
        auto BadPath = Arguments(TEXT("raise AssertionError('must never execute')"), Path);
        TestFalse(TEXT("Malformed backup path is rejected before execution: ") + Path,
            Call(TEXT("ue_python_execute"), BadPath)->GetBoolField(TEXT("success")));
    }
    auto TooLong = Arguments(FString::ChrN(131073, 'x'));
    TestFalse(TEXT("Oversized scripts are rejected"), Call(TEXT("ue_python_execute"), TooLong)->GetBoolField(TEXT("success")));
    auto MissingPaths = MakeShared<FJsonObject>();
    MissingPaths->SetStringField(TEXT("code"), TEXT("print('never')"));
    MissingPaths->SetStringField(TEXT("label"), TEXT("test"));
    TestFalse(TEXT("Backup declaration is required"), Call(TEXT("ue_python_execute"), MissingPaths)->GetBoolField(TEXT("success")));
    auto InvalidRun = MakeShared<FJsonObject>(); InvalidRun->SetStringField(TEXT("run_id"), TEXT("../../outside"));
    TestFalse(TEXT("Run archive traversal is rejected"), Call(TEXT("ue_python_get_run"), InvalidRun, false, false)->GetBoolField(TEXT("success")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkPythonExecutionTest, "CinderLink.Python.ExecuteAndArchive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkPythonExecutionTest::RunTest(const FString& Parameters)
{
    auto First = Call(TEXT("ue_python_execute"), Arguments(TEXT("import unreal\ncinderlink_private_marker = 42\nprint('CINDERLINK_PYTHON_OK')\n")));
    TestTrue(TEXT("UE Python executes successfully"), First->GetBoolField(TEXT("success")));
    if (!First->HasField(TEXT("run_id"))) return false;
    TestTrue(TEXT("Printed output returns to the model"), First->GetStringField(TEXT("output")).Contains(TEXT("CINDERLINK_PYTHON_OK")));
    TestTrue(TEXT("Exact script is archived"), IFileManager::Get().FileExists(*(FPaths::ProjectDir() / First->GetStringField(TEXT("script_path")))));
    auto Query = MakeShared<FJsonObject>(); Query->SetStringField(TEXT("run_id"), First->GetStringField(TEXT("run_id")));
    auto Archived = Call(TEXT("ue_python_get_run"), Query, false, false);
    TestEqual(TEXT("Result can be read after authoring is off"), Archived->GetStringField(TEXT("output")), First->GetStringField(TEXT("output")));
    auto Second = Call(TEXT("ue_python_execute"), Arguments(TEXT("assert 'cinderlink_private_marker' not in globals()\nprint('PRIVATE_SCOPE_OK')\n")));
    TestTrue(TEXT("Runs have fresh private globals"), Second->GetBoolField(TEXT("success")));
    TestNotEqual(TEXT("Every execution receives a unique archive"), First->GetStringField(TEXT("run_id")), Second->GetStringField(TEXT("run_id")));
    const FString RecipeName = TEXT("test_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Query->SetStringField(TEXT("name"), RecipeName);
    TestFalse(TEXT("Recipe export requires authoring mode"), Call(TEXT("ue_python_save_recipe"), Query, true, false)->GetBoolField(TEXT("success")));
    auto Export = Call(TEXT("ue_python_save_recipe"), Query);
    TestTrue(TEXT("Recipe is saved outside Saved for Git tracking"), Export->GetBoolField(TEXT("success")));
    TestFalse(TEXT("Existing recipe cannot be overwritten"), Call(TEXT("ue_python_save_recipe"), Query)->GetBoolField(TEXT("success")));
    FString Recipe;
    FFileHelper::LoadFileToString(Recipe, *(FPaths::ProjectDir() / TEXT("Scripts/CinderLink") / (RecipeName + TEXT(".py"))));
    TestTrue(TEXT("Recipe contains the executed code"), Recipe.Contains(TEXT("CINDERLINK_PYTHON_OK")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkPythonBackupTest, "CinderLink.Python.BackupAndDiskChanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkPythonBackupTest::RunTest(const FString& Parameters)
{
    const FString Folder = UniqueFolder();
    const FString Relative = TEXT("Content") + Folder.RightChop(5);
    const FString Absolute = FPaths::ProjectDir() / Relative;
    IFileManager::Get().MakeDirectory(*Absolute, true);
    FFileHelper::SaveStringToFile(TEXT("original"), *(Absolute / TEXT("canary.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const FString Code = FString::Printf(TEXT("import unreal\nfrom pathlib import Path\np = Path(unreal.Paths.project_dir()) / '%s'\n(p / 'canary.txt').write_text('changed', encoding='utf-8')\n(p / 'new.txt').write_text('new', encoding='utf-8')\nprint('BACKUP_TEST_DONE')\n"), *Relative);
    auto Run = Call(TEXT("ue_python_execute"), Arguments(Code, Folder));
    TestTrue(TEXT("Authoring script runs after backup"), Run->GetBoolField(TEXT("success")));
    if (!Run->HasField(TEXT("run_id"))) return false;
    TestEqual(TEXT("Existing file backed up"), Run->GetArrayField(TEXT("backups")).Num(), 1);
    TestEqual(TEXT("Created and modified files reported"), Run->GetArrayField(TEXT("disk_changes")).Num(), 2);
    FString Backup;
    const FString BackupPath = FPaths::ProjectDir() / TEXT("Saved/CinderLink/Python") / Run->GetStringField(TEXT("run_id")) / TEXT("backup") / Relative / TEXT("canary.txt");
    FFileHelper::LoadFileToString(Backup, *BackupPath);
    TestEqual(TEXT("Backup preserves bytes from before Python"), Backup, FString(TEXT("original")));
    AddInfo(TEXT("Recovery fixture run: ") + Run->GetStringField(TEXT("run_id")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkPythonMaterialTest, "CinderLink.Python.MaterialAuthoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkPythonMaterialTest::RunTest(const FString& Parameters)
{
    const FString Folder = UniqueFolder();
    const FString Code = FString::Printf(TEXT(
        "import unreal as u\n"
        "folder = '%s'\n"
        "mat = u.AssetToolsHelpers.get_asset_tools().create_asset('M_CinderLinkTest', folder, u.Material, u.MaterialFactoryNew())\n"
        "assert mat\n"
        "node = u.MaterialEditingLibrary.create_material_expression(mat, u.MaterialExpressionConstant3Vector)\n"
        "node.set_editor_property('constant', u.LinearColor(0.12, 0.35, 0.08, 1))\n"
        "assert u.MaterialEditingLibrary.connect_material_property(node, '', u.MaterialProperty.MP_BASE_COLOR)\n"
        "u.MaterialEditingLibrary.recompile_material(mat)\n"
        "assert u.EditorAssetLibrary.save_loaded_asset(mat)\n"
        "print('MATERIAL_CREATED ' + mat.get_path_name())\n"), *Folder);
    auto Created = Call(TEXT("ue_python_execute"), Arguments(Code, Folder));
    TestTrue(TEXT("Python creates, compiles and saves a real UE material"), Created->GetBoolField(TEXT("success")));
    if (!Created->HasField(TEXT("run_id"))) return false;
    TestTrue(TEXT("Material asset creation is recorded"), Created->GetArrayField(TEXT("disk_changes")).Num() > 0);
    const FString ModifyCode = FString::Printf(TEXT(
        "import unreal as u\n"
        "mat = u.load_asset('%s/M_CinderLinkTest')\n"
        "assert mat\n"
        "node = u.MaterialEditingLibrary.get_material_property_input_node(mat, u.MaterialProperty.MP_BASE_COLOR)\n"
        "assert abs(node.get_editor_property('constant').g - 0.35) < 0.001\n"
        "node.set_editor_property('constant', u.LinearColor(0.4, 0.2, 0.1, 1))\n"
        "u.MaterialEditingLibrary.recompile_material(mat)\n"
        "assert u.EditorAssetLibrary.save_loaded_asset(mat)\n"
        "print('MATERIAL_UPDATED')\n"), *Folder);
    auto Updated = Call(TEXT("ue_python_execute"), Arguments(ModifyCode, Folder));
    TestTrue(TEXT("Follow-up script reads and changes the existing material graph"), Updated->GetBoolField(TEXT("success")));
    TestTrue(TEXT("Original material is backed up before modification"), Updated->GetArrayField(TEXT("backups")).Num() > 0);
    UPackage* Package = FindPackage(nullptr, *(Folder / TEXT("M_CinderLinkTest")));
    if (TestNotNull(TEXT("Authored material package is loaded"), Package))
    {
        Package->SetDirtyFlag(true);
        auto Dirty = Call(TEXT("ue_python_execute"), Arguments(TEXT("raise AssertionError('must never run')"), Folder));
        TestFalse(TEXT("Unsaved source assets cannot silently receive a stale disk backup"), Dirty->GetBoolField(TEXT("success")));
        Package->SetDirtyFlag(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkPythonExceptionTest, "CinderLink.Python.ExceptionEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkPythonExceptionTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("CinderLinkExpectedException"), EAutomationExpectedErrorFlags::Contains, 0);
    AddExpectedErrorPlain(TEXT("LogPython: Traceback (most recent call last):"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedError(TEXT("script.py\", line 2, in <module>"), EAutomationExpectedErrorFlags::Contains, 1);
    auto Failed = Call(TEXT("ue_python_execute"), Arguments(TEXT("print('BEFORE_FAILURE')\nraise RuntimeError('CinderLinkExpectedException')\n")));
    TestFalse(TEXT("Python exception is a failed tool result"), Failed->GetBoolField(TEXT("success")));
    if (!Failed->HasField(TEXT("run_id"))) return false;
    TestTrue(TEXT("Traceback returns to Codex"), Failed->GetStringField(TEXT("error")).Contains(TEXT("CinderLinkExpectedException")));
    TestTrue(TEXT("Output before failure survives"), Failed->GetStringField(TEXT("output")).Contains(TEXT("BEFORE_FAILURE")));
    auto Query = MakeShared<FJsonObject>(); Query->SetStringField(TEXT("run_id"), Failed->GetStringField(TEXT("run_id")));
    TestEqual(TEXT("Failure is archived, not reported as success"), Call(TEXT("ue_python_get_run"), Query, false, false)->GetStringField(TEXT("status")), FString(TEXT("failed")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkPythonTurnPolicyTest, "CinderLink.Security.PythonTurnRevocation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkPythonTurnPolicyTest::RunTest(const FString& Parameters)
{
    FCinderLinkAppServerClient Client;
    TestFalse(TEXT("New clients have no Python authorization"), Client.bActiveTurnAllowsPythonAuthoring);
    Client.bActiveTurnAllowsPythonAuthoring = true;
    Client.RevokePythonAuthoring();
    TestFalse(TEXT("Panel revocation is immediate for subsequent calls"), Client.bActiveTurnAllowsPythonAuthoring);
    Client.bActiveTurnAllowsPythonAuthoring = true;
    FString Error;
    Client.InterruptTurn(Error);
    TestFalse(TEXT("Stop revokes Python even if transport is no longer ready"), Client.bActiveTurnAllowsPythonAuthoring);
    Client.bActiveTurnAllowsPythonAuthoring = true;
    Client.bTurnInProgress = true;
    Client.ActiveTurnId = TEXT("active-test-turn");
    auto Turn = MakeShared<FJsonObject>(); Turn->SetStringField(TEXT("id"), TEXT("active-test-turn"));
    auto Params = MakeShared<FJsonObject>(); Params->SetObjectField(TEXT("turn"), Turn);
    auto Message = MakeShared<FJsonObject>(); Message->SetObjectField(TEXT("params"), Params);
    Client.HandleNotification(Message, TEXT("turn/completed"));
    TestFalse(TEXT("Completion clears the previous turn's authority"), Client.bActiveTurnAllowsPythonAuthoring);
    Client.bActiveTurnAllowsPythonAuthoring = true;
    Client.Disconnect();
    TestFalse(TEXT("Disconnect clears authoring authority"), Client.bActiveTurnAllowsPythonAuthoring);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCinderLinkPythonPanelPolicyTest, "CinderLink.Security.PythonPanelPreference",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkPythonPanelPolicyTest::RunTest(const FString& Parameters)
{
    // Exercise the real panel without starting a second App Server or sending a prompt.
    TSharedRef<SCinderLinkPanel> Panel = SNew(SCinderLinkPanel).AutoConnect(false);
    TestTrue(TEXT("Python is selected when a new panel opens"), Panel->PythonAuthoringCheckBox->IsChecked());
    TestFalse(TEXT("A selected default does not authorize tools outside a submitted turn"),
        Panel->Client->bActiveTurnAllowsPythonAuthoring);

    Panel->Client->bActiveTurnAllowsPythonAuthoring = true;
    Panel->OnInterruptClicked();
    TestTrue(TEXT("Stop preserves the preference for the next turn"), Panel->PythonAuthoringCheckBox->IsChecked());
    TestFalse(TEXT("Stop still revokes current-turn Python calls"), Panel->Client->bActiveTurnAllowsPythonAuthoring);

    Panel->OnNewThreadClicked();
    TestTrue(TEXT("New thread does not silently switch Python off"), Panel->PythonAuthoringCheckBox->IsChecked());
    Panel->Client->bActiveTurnAllowsPythonAuthoring = true;
    Panel->Client->Disconnect();
    Panel->HandleMessage({ECinderLinkMessageKind::Error, TEXT("Test disconnection")});
    TestTrue(TEXT("Disconnect and process-exit messages preserve selection"), Panel->PythonAuthoringCheckBox->IsChecked());
    TestFalse(TEXT("Disconnected turns have no Python authority"), Panel->Client->bActiveTurnAllowsPythonAuthoring);

    Panel->Client->bActiveTurnAllowsPythonAuthoring = true;
    Panel->PythonAuthoringCheckBox->SetIsChecked(ECheckBoxState::Unchecked);
    Panel->OnPythonAuthoringChanged(ECheckBoxState::Unchecked);
    TestFalse(TEXT("Manual OFF revokes active authority"), Panel->Client->bActiveTurnAllowsPythonAuthoring);
    Panel->OnNewThreadClicked();
    Panel->OnInterruptClicked();
    Panel->HandleMessage({ECinderLinkMessageKind::Status, TEXT("Test reconnection status")});
    TestFalse(TEXT("Manual OFF is not automatically re-enabled"), Panel->PythonAuthoringCheckBox->IsChecked());

    Panel->PythonAuthoringCheckBox->SetIsChecked(ECheckBoxState::Checked);
    Panel->Client->bActiveTurnAllowsPythonAuthoring = true;
    Panel->AllowEditsCheckBox->SetIsChecked(ECheckBoxState::Unchecked);
    Panel->OnEditPermissionChanged(ECheckBoxState::Unchecked);
    TestFalse(TEXT("Read-only selection disables Python"), Panel->PythonAuthoringCheckBox->IsChecked());
    TestFalse(TEXT("Read-only selection revokes the active turn"), Panel->Client->bActiveTurnAllowsPythonAuthoring);
    return true;
}

#endif
