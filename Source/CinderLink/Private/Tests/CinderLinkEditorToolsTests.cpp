// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkEditorTools.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCinderLinkEditorToolPolicyTest,
    "CinderLink.Security.EditorToolPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCinderLinkEditorToolPolicyTest::RunTest(const FString& Parameters)
{
    const TArray<TSharedPtr<FJsonValue>> Specs = FCinderLinkEditorTools::BuildToolSpecs();
    TestEqual(TEXT("The bounded Editor tool set is present"), Specs.Num(), 16);

    TSet<FString> Names;
    for (const TSharedPtr<FJsonValue>& Value : Specs)
    {
        const TSharedPtr<FJsonObject> Spec = Value.IsValid() ? Value->AsObject() : nullptr;
        TestTrue(TEXT("Every tool spec is an object"), Spec.IsValid());
        if (!Spec.IsValid())
        {
            continue;
        }
        FString Name;
        TestTrue(TEXT("Every tool has a name"), Spec->TryGetStringField(TEXT("name"), Name));
        TestFalse(TEXT("Tool names are unique"), Names.Contains(Name));
        Names.Add(Name);
        TestEqual(TEXT("Every tool is a function"), Spec->GetStringField(TEXT("type")), FString(TEXT("function")));
        TestTrue(TEXT("Every tool has an input schema"), Spec->HasTypedField<EJson::Object>(TEXT("inputSchema")));
    }

    TestTrue(TEXT("Read-only state tool is known"), FCinderLinkEditorTools::IsKnownTool(TEXT("ue_editor_get_state")));
    TestFalse(TEXT("Read-only state tool is not a mutation"), FCinderLinkEditorTools::IsMutationTool(TEXT("ue_editor_get_state")));
    TestTrue(TEXT("Level save is a gated mutation"), FCinderLinkEditorTools::IsMutationTool(TEXT("ue_level_save")));
    TestFalse(TEXT("Arbitrary Python is not exposed"), FCinderLinkEditorTools::IsKnownTool(TEXT("execute_python")));
    TestFalse(TEXT("Actor deletion is not exposed"), FCinderLinkEditorTools::IsKnownTool(TEXT("ue_level_delete_actor")));

    FString Summary;
    TSharedRef<FJsonObject> Denied = FCinderLinkEditorTools::Execute(
        TEXT("ue_level_save"),
        MakeShared<FJsonObject>(),
        FPaths::ProjectDir(),
        false,
        Summary);
    TestFalse(TEXT("Mutation is denied without turn consent"), Denied->GetBoolField(TEXT("success")));

    TSharedRef<FJsonObject> InvalidCreateArguments = MakeShared<FJsonObject>();
    InvalidCreateArguments->SetStringField(TEXT("asset_path"), TEXT("/Game/CinderLinkInvalidTypeTest"));
    InvalidCreateArguments->SetStringField(TEXT("partitioned"), TEXT("not-a-boolean"));
    TSharedRef<FJsonObject> InvalidCreate = FCinderLinkEditorTools::Execute(
        TEXT("ue_level_create"),
        InvalidCreateArguments,
        FPaths::ProjectDir(),
        true,
        Summary);
    TestFalse(TEXT("Malformed mutation arguments fail closed before changing the level"),
        InvalidCreate->GetBoolField(TEXT("success")));

    TSharedRef<FJsonObject> State = FCinderLinkEditorTools::Execute(
        TEXT("ue_editor_get_state"),
        MakeShared<FJsonObject>(),
        FPaths::ProjectDir(),
        false,
        Summary);
    TestTrue(TEXT("Read-only Editor state is available without mutation consent"), State->GetBoolField(TEXT("success")));

    if (FModuleManager::Get().IsModuleLoaded(TEXT("CesiumRuntime")))
    {
        UClass* GeoreferenceClass = FindObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.CesiumGeoreference"));
        UClass* TilesetClass = FindObject<UClass>(nullptr, TEXT("/Script/CesiumRuntime.Cesium3DTileset"));
        TestNotNull(TEXT("Loaded Cesium Georeference class is discoverable"), GeoreferenceClass);
        TestNotNull(TEXT("Loaded Cesium 3D Tileset class is discoverable"), TilesetClass);
        if (GeoreferenceClass != nullptr)
        {
            FNumericProperty* Latitude = FindFProperty<FNumericProperty>(GeoreferenceClass, TEXT("OriginLatitude"));
            FNumericProperty* Longitude = FindFProperty<FNumericProperty>(GeoreferenceClass, TEXT("OriginLongitude"));
            FNumericProperty* Height = FindFProperty<FNumericProperty>(GeoreferenceClass, TEXT("OriginHeight"));
            TestTrue(TEXT("Cesium origin latitude is in the safe numeric reflection subset"),
                Latitude != nullptr && Latitude->HasAnyPropertyFlags(CPF_Edit));
            TestTrue(TEXT("Cesium origin longitude is in the safe numeric reflection subset"),
                Longitude != nullptr && Longitude->HasAnyPropertyFlags(CPF_Edit));
            TestTrue(TEXT("Cesium origin height is in the safe numeric reflection subset"),
                Height != nullptr && Height->HasAnyPropertyFlags(CPF_Edit));
        }
        if (TilesetClass != nullptr)
        {
            FNumericProperty* IonAssetId = FindFProperty<FNumericProperty>(TilesetClass, TEXT("IonAssetID"));
            FNumericProperty* ScreenSpaceError = FindFProperty<FNumericProperty>(TilesetClass, TEXT("MaximumScreenSpaceError"));
            FObjectPropertyBase* Georeference = FindFProperty<FObjectPropertyBase>(TilesetClass, TEXT("Georeference"));
            TestTrue(TEXT("Cesium ion asset id is in the safe numeric reflection subset"),
                IonAssetId != nullptr && IonAssetId->HasAnyPropertyFlags(CPF_Edit));
            TestTrue(TEXT("Cesium screen-space error is in the safe numeric reflection subset"),
                ScreenSpaceError != nullptr && ScreenSpaceError->HasAnyPropertyFlags(CPF_Edit));
            TestTrue(TEXT("Cesium Georeference actor link is in the safe object reflection subset"),
                Georeference != nullptr && Georeference->HasAnyPropertyFlags(CPF_Edit));
        }
    }
    return true;
}

#endif
