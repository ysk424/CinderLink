// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkEditorTools.h"

#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/Factory.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "LevelEditorSubsystem.h"
#include "UObject/Class.h"
#include "UObject/FieldIterator.h"
#include "UObject/Package.h"
#include "Windows/WindowsHWrapper.h"

namespace
{
    constexpr int32 MaximumActors = 512;
    constexpr int32 MaximumEditableProperties = 32;
    constexpr int32 MaximumReportedProperties = 128;
    constexpr int64 MaximumImportBytes = 32ll * 1024ll * 1024ll;
    constexpr int32 MaximumCaptureDimension = 1280;

    const TSet<FString>& MutationTools()
    {
        static const TSet<FString> Names = {
            TEXT("ue_level_create"),
            TEXT("ue_level_load"),
            TEXT("ue_level_save"),
            TEXT("ue_level_spawn_actor"),
            TEXT("ue_level_update_actor"),
            TEXT("ue_level_set_game_mode"),
            TEXT("ue_asset_import_image"),
            TEXT("ue_viewport_set_camera"),
            TEXT("ue_viewport_capture"),
            TEXT("ue_pie_start"),
            TEXT("ue_pie_stop")
        };
        return Names;
    }

    const TSet<FString>& ReadTools()
    {
        static const TSet<FString> Names = {
            TEXT("ue_editor_get_state"),
            TEXT("ue_level_list_actors"),
            TEXT("ue_level_get_actor"),
            TEXT("ue_asset_query"),
            TEXT("ue_level_map_check")
        };
        return Names;
    }

    TSharedRef<FJsonObject> StringSchema(const FString& Description)
    {
        TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("string"));
        Schema->SetStringField(TEXT("description"), Description);
        return Schema;
    }

    TSharedRef<FJsonObject> BooleanSchema(const FString& Description)
    {
        TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("boolean"));
        Schema->SetStringField(TEXT("description"), Description);
        return Schema;
    }

    TSharedRef<FJsonObject> NumberSchema(const FString& Description)
    {
        TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("number"));
        Schema->SetStringField(TEXT("description"), Description);
        return Schema;
    }

    TSharedRef<FJsonObject> VectorSchema(const FString& Description, bool bRotation = false)
    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(bRotation ? TEXT("pitch") : TEXT("x"), NumberSchema(TEXT("First component.")));
        Properties->SetObjectField(bRotation ? TEXT("yaw") : TEXT("y"), NumberSchema(TEXT("Second component.")));
        Properties->SetObjectField(bRotation ? TEXT("roll") : TEXT("z"), NumberSchema(TEXT("Third component.")));

        TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));
        Schema->SetStringField(TEXT("description"), Description);
        Schema->SetObjectField(TEXT("properties"), Properties);
        Schema->SetBoolField(TEXT("additionalProperties"), false);
        TArray<TSharedPtr<FJsonValue>> Required;
        Required.Add(MakeShared<FJsonValueString>(bRotation ? TEXT("pitch") : TEXT("x")));
        Required.Add(MakeShared<FJsonValueString>(bRotation ? TEXT("yaw") : TEXT("y")));
        Required.Add(MakeShared<FJsonValueString>(bRotation ? TEXT("roll") : TEXT("z")));
        Schema->SetArrayField(TEXT("required"), Required);
        return Schema;
    }

    TSharedRef<FJsonObject> ObjectSchema(
        const TSharedRef<FJsonObject>& Properties,
        const TArray<FString>& Required = {})
    {
        TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));
        Schema->SetObjectField(TEXT("properties"), Properties);
        Schema->SetBoolField(TEXT("additionalProperties"), false);
        if (!Required.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const FString& Name : Required)
            {
                Values.Add(MakeShared<FJsonValueString>(Name));
            }
            Schema->SetArrayField(TEXT("required"), Values);
        }
        return Schema;
    }

    TSharedPtr<FJsonValue> ToolSpec(
        const FString& Name,
        const FString& Description,
        const TSharedRef<FJsonObject>& InputSchema)
    {
        TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
        Tool->SetStringField(TEXT("type"), TEXT("function"));
        Tool->SetStringField(TEXT("name"), Name);
        Tool->SetStringField(TEXT("description"), Description);
        Tool->SetObjectField(TEXT("inputSchema"), InputSchema);
        return MakeShared<FJsonValueObject>(Tool);
    }

    FString SerializeJson(const TSharedRef<FJsonObject>& Object)
    {
        FString Result;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
        FJsonSerializer::Serialize(Object, Writer);
        return Result;
    }

    TSharedRef<FJsonObject> MakeResponse(
        bool bSuccess,
        const TSharedRef<FJsonObject>& Payload,
        const FString& ImageDataUrl = FString())
    {
        TArray<TSharedPtr<FJsonValue>> ContentItems;
        TSharedRef<FJsonObject> TextItem = MakeShared<FJsonObject>();
        TextItem->SetStringField(TEXT("type"), TEXT("inputText"));
        TextItem->SetStringField(TEXT("text"), SerializeJson(Payload));
        ContentItems.Add(MakeShared<FJsonValueObject>(TextItem));

        if (!ImageDataUrl.IsEmpty())
        {
            TSharedRef<FJsonObject> ImageItem = MakeShared<FJsonObject>();
            ImageItem->SetStringField(TEXT("type"), TEXT("inputImage"));
            ImageItem->SetStringField(TEXT("imageUrl"), ImageDataUrl);
            ContentItems.Add(MakeShared<FJsonValueObject>(ImageItem));
        }

        TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetArrayField(TEXT("contentItems"), ContentItems);
        Response->SetBoolField(TEXT("success"), bSuccess);
        return Response;
    }

    TSharedRef<FJsonObject> Failure(const FString& Error, FString& OutSummary)
    {
        OutSummary = Error;
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("error"), Error);
        return MakeResponse(false, Payload);
    }

    TSharedRef<FJsonObject> Success(
        const TSharedRef<FJsonObject>& Payload,
        const FString& Summary,
        FString& OutSummary,
        const FString& ImageDataUrl = FString())
    {
        OutSummary = Summary;
        return MakeResponse(true, Payload, ImageDataUrl);
    }

    UWorld* GetEditorWorld()
    {
        return GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    bool IsPlaySessionActive()
    {
        return GEditor != nullptr && GEditor->PlayWorld != nullptr;
    }

    bool IsSafeGamePath(const FString& Path, bool bAllowGameRoot = false)
    {
        if (Path.Len() > 256 || Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":")))
        {
            return false;
        }
        return bAllowGameRoot ? (Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"))) : Path.StartsWith(TEXT("/Game/"));
    }

    bool IsSafeClassPath(const FString& Path)
    {
        return Path.Len() <= 512 && !Path.Contains(TEXT("..")) &&
            (Path.StartsWith(TEXT("/Script/")) || Path.StartsWith(TEXT("/Game/")));
    }

    bool IsSafeLabelOrFolder(const FString& Value, int32 MaximumLength)
    {
        return Value.Len() <= MaximumLength && !Value.Contains(TEXT("\r")) &&
            !Value.Contains(TEXT("\n")) && !Value.Contains(TEXT("..")) &&
            !Value.Contains(TEXT("\\")) && !Value.Contains(TEXT(":"));
    }

    bool IsSensitivePropertyName(const FString& Name)
    {
        const FString Lower = Name.ToLower();
        return Lower.Contains(TEXT("secret")) || Lower.Contains(TEXT("token")) ||
            Lower.Contains(TEXT("password")) || Lower.Contains(TEXT("credential")) ||
            Lower.Contains(TEXT("authorization")) || Lower.Contains(TEXT("apikey")) ||
            Lower.Contains(TEXT("api_key")) || Lower.Contains(TEXT("privatekey")) ||
            Lower.Contains(TEXT("accesskey")) || Lower.Contains(TEXT("authkey")) ||
            Lower.Contains(TEXT("signingkey")) || Lower.Contains(TEXT("bearer"));
    }

    bool HasWrongJsonType(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* FieldName,
        EJson ExpectedType)
    {
        if (!Object.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonValue>* Value = Object->Values.Find(FieldName);
        return Value != nullptr && (!Value->IsValid() || (*Value)->Type != ExpectedType);
    }

    bool IsEditableSafeProperty(const FProperty* Property)
    {
        return Property != nullptr && Property->HasAnyPropertyFlags(CPF_Edit) &&
            !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnInstance) &&
            !IsSensitivePropertyName(Property->GetName());
    }

    TSharedRef<FJsonObject> VectorToJson(const FVector& Value)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("x"), Value.X);
        Result->SetNumberField(TEXT("y"), Value.Y);
        Result->SetNumberField(TEXT("z"), Value.Z);
        return Result;
    }

    TSharedRef<FJsonObject> RotatorToJson(const FRotator& Value)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("pitch"), Value.Pitch);
        Result->SetNumberField(TEXT("yaw"), Value.Yaw);
        Result->SetNumberField(TEXT("roll"), Value.Roll);
        return Result;
    }

    bool ReadFiniteNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, double& OutValue)
    {
        return Object.IsValid() && Object->TryGetNumberField(Name, OutValue) && FMath::IsFinite(OutValue);
    }

    bool ReadVector(const TSharedPtr<FJsonObject>& Object, FVector& OutValue)
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!ReadFiniteNumber(Object, TEXT("x"), X) || !ReadFiniteNumber(Object, TEXT("y"), Y) ||
            !ReadFiniteNumber(Object, TEXT("z"), Z) || FMath::Abs(X) > 1.0e9 ||
            FMath::Abs(Y) > 1.0e9 || FMath::Abs(Z) > 1.0e9)
        {
            return false;
        }
        OutValue = FVector(X, Y, Z);
        return true;
    }

    bool ReadRotator(const TSharedPtr<FJsonObject>& Object, FRotator& OutValue)
    {
        double Pitch = 0.0;
        double Yaw = 0.0;
        double Roll = 0.0;
        if (!ReadFiniteNumber(Object, TEXT("pitch"), Pitch) || !ReadFiniteNumber(Object, TEXT("yaw"), Yaw) ||
            !ReadFiniteNumber(Object, TEXT("roll"), Roll) || FMath::Abs(Pitch) > 360000.0 ||
            FMath::Abs(Yaw) > 360000.0 || FMath::Abs(Roll) > 360000.0)
        {
            return false;
        }
        OutValue = FRotator(Pitch, Yaw, Roll);
        return true;
    }

    FString ActorIdentifier(const AActor* Actor)
    {
        if (Actor == nullptr)
        {
            return FString();
        }
        const FGuid Guid = Actor->GetActorGuid();
        return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : Actor->GetPathName();
    }

    AActor* FindActor(const FString& Selector, FString& OutError)
    {
        UWorld* World = GetEditorWorld();
        if (World == nullptr || Selector.IsEmpty() || Selector.Len() > 512)
        {
            OutError = TEXT("A valid current editor world and actor selector are required.");
            return nullptr;
        }

        AActor* Match = nullptr;
        int32 MatchCount = 0;
        int32 Seen = 0;
        for (TActorIterator<AActor> It(World); It && Seen < MaximumActors; ++It, ++Seen)
        {
            AActor* Actor = *It;
            if (!IsValid(Actor) || Actor->HasAnyFlags(RF_Transient))
            {
                continue;
            }
            if (ActorIdentifier(Actor).Equals(Selector, ESearchCase::IgnoreCase) ||
                Actor->GetActorLabel().Equals(Selector, ESearchCase::CaseSensitive) ||
                Actor->GetName().Equals(Selector, ESearchCase::CaseSensitive) ||
                Actor->GetPathName().Equals(Selector, ESearchCase::CaseSensitive))
            {
                Match = Actor;
                ++MatchCount;
            }
        }

        if (MatchCount == 0)
        {
            OutError = TEXT("No actor matched the supplied id, label, name, or path.");
            return nullptr;
        }
        if (MatchCount > 1)
        {
            OutError = TEXT("The actor selector is ambiguous. Use the actor id returned by ue_level_list_actors.");
            return nullptr;
        }
        return Match;
    }

    TSharedPtr<FJsonValue> ReadPropertyValue(const UObject* Object, const FProperty* Property)
    {
        const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
        if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
        {
            return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(Value));
        }
        if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
        {
            const int64 Numeric = Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
            return MakeShared<FJsonValueString>(Enum->GetEnum()->GetNameStringByValue(Numeric));
        }
        if (const FByteProperty* Byte = CastField<FByteProperty>(Property); Byte != nullptr && Byte->Enum != nullptr)
        {
            return MakeShared<FJsonValueString>(Byte->Enum->GetNameStringByValue(Byte->GetPropertyValue(Value)));
        }
        if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
        {
            double Number = 0.0;
            if (Numeric->IsFloatingPoint())
            {
                Number = Numeric->GetFloatingPointPropertyValue(Value);
            }
            else if (Numeric->CanHoldValue<int64>(-1))
            {
                Number = static_cast<double>(Numeric->GetSignedIntPropertyValue(Value));
            }
            else
            {
                Number = static_cast<double>(Numeric->GetUnsignedIntPropertyValue(Value));
            }
            if (FMath::IsFinite(Number))
            {
                return MakeShared<FJsonValueNumber>(Number);
            }
            return TSharedPtr<FJsonValue>();
        }
        if (const FStrProperty* String = CastField<FStrProperty>(Property))
        {
            return MakeShared<FJsonValueString>(String->GetPropertyValue(Value).Left(2048));
        }
        if (const FNameProperty* Name = CastField<FNameProperty>(Property))
        {
            return MakeShared<FJsonValueString>(Name->GetPropertyValue(Value).ToString());
        }
        if (const FTextProperty* Text = CastField<FTextProperty>(Property))
        {
            return MakeShared<FJsonValueString>(Text->GetPropertyValue(Value).ToString().Left(2048));
        }
        if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
        {
            if (Struct->Struct == TBaseStructure<FVector>::Get())
            {
                return MakeShared<FJsonValueObject>(VectorToJson(*static_cast<const FVector*>(Value)));
            }
            if (Struct->Struct == TBaseStructure<FRotator>::Get())
            {
                return MakeShared<FJsonValueObject>(RotatorToJson(*static_cast<const FRotator*>(Value)));
            }
            if (Struct->Struct == TBaseStructure<FVector2D>::Get())
            {
                const FVector2D& Vector = *static_cast<const FVector2D*>(Value);
                TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetNumberField(TEXT("x"), Vector.X);
                Result->SetNumberField(TEXT("y"), Vector.Y);
                return MakeShared<FJsonValueObject>(Result);
            }
            if (Struct->Struct == TBaseStructure<FLinearColor>::Get())
            {
                const FLinearColor& Color = *static_cast<const FLinearColor*>(Value);
                TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetNumberField(TEXT("r"), Color.R);
                Result->SetNumberField(TEXT("g"), Color.G);
                Result->SetNumberField(TEXT("b"), Color.B);
                Result->SetNumberField(TEXT("a"), Color.A);
                return MakeShared<FJsonValueObject>(Result);
            }
        }
        if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
            ObjectProperty != nullptr && ObjectProperty->PropertyClass->IsChildOf(AActor::StaticClass()))
        {
            const AActor* ReferencedActor = Cast<AActor>(ObjectProperty->GetObjectPropertyValue(Value));
            if (ReferencedActor != nullptr)
            {
                return MakeShared<FJsonValueString>(ActorIdentifier(ReferencedActor));
            }
            return MakeShared<FJsonValueNull>();
        }
        return nullptr;
    }

    TSharedRef<FJsonObject> ActorToJson(AActor* Actor, bool bIncludeProperties)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("id"), ActorIdentifier(Actor));
        Result->SetStringField(TEXT("name"), Actor->GetName());
        Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
        Result->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
        Result->SetObjectField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
        Result->SetObjectField(TEXT("rotation"), RotatorToJson(Actor->GetActorRotation()));
        Result->SetObjectField(TEXT("scale"), VectorToJson(Actor->GetActorScale3D()));
        if (const USceneComponent* Root = Actor->GetRootComponent())
        {
            const UEnum* MobilityEnum = StaticEnum<EComponentMobility::Type>();
            Result->SetStringField(TEXT("mobility"), MobilityEnum->GetNameStringByValue(Root->Mobility));
        }

        if (bIncludeProperties)
        {
            TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
            int32 Reported = 0;
            for (TFieldIterator<FProperty> It(Actor->GetClass()); It && Reported < MaximumReportedProperties; ++It)
            {
                FProperty* Property = *It;
                if (!IsEditableSafeProperty(Property))
                {
                    continue;
                }
                TSharedPtr<FJsonValue> Value = ReadPropertyValue(Actor, Property);
                if (Value.IsValid())
                {
                    Properties->SetField(Property->GetName(), Value);
                    ++Reported;
                }
            }
            Result->SetObjectField(TEXT("editableProperties"), Properties);
            Result->SetBoolField(TEXT("propertiesTruncated"), Reported >= MaximumReportedProperties);
        }
        return Result;
    }

    bool SetPropertyValue(
        UObject* Object,
        FProperty* Property,
        const TSharedPtr<FJsonValue>& JsonValue,
        bool bApply,
        FString& OutError)
    {
        if (!IsEditableSafeProperty(Property) || !JsonValue.IsValid())
        {
            OutError = TEXT("The requested property is not in CinderLink's safe editable subset.");
            return false;
        }

        void* Value = Property->ContainerPtrToValuePtr<void>(Object);
        if (bApply)
        {
            Object->PreEditChange(Property);
        }
        bool bSet = false;

        if (FBoolProperty* Bool = CastField<FBoolProperty>(Property); Bool != nullptr && JsonValue->Type == EJson::Boolean)
        {
            if (bApply)
            {
                Bool->SetPropertyValue(Value, JsonValue->AsBool());
            }
            bSet = true;
        }
        else if (FEnumProperty* Enum = CastField<FEnumProperty>(Property); Enum != nullptr && JsonValue->Type == EJson::String)
        {
            const int64 Numeric = Enum->GetEnum()->GetValueByNameString(JsonValue->AsString());
            if (Numeric != INDEX_NONE)
            {
                if (bApply)
                {
                    Enum->GetUnderlyingProperty()->SetIntPropertyValue(Value, Numeric);
                }
                bSet = true;
            }
        }
        else if (FByteProperty* Byte = CastField<FByteProperty>(Property);
            Byte != nullptr && Byte->Enum != nullptr && JsonValue->Type == EJson::String)
        {
            const int64 Numeric = Byte->Enum->GetValueByNameString(JsonValue->AsString());
            if (Numeric != INDEX_NONE && Numeric >= 0 && Numeric <= MAX_uint8)
            {
                if (bApply)
                {
                    Byte->SetPropertyValue(Value, static_cast<uint8>(Numeric));
                }
                bSet = true;
            }
        }
        else if (FNumericProperty* Numeric = CastField<FNumericProperty>(Property); Numeric != nullptr && JsonValue->Type == EJson::Number)
        {
            const double Number = JsonValue->AsNumber();
            if (FMath::IsFinite(Number) && FMath::Abs(Number) <= 1.0e12)
            {
                if (Numeric->IsFloatingPoint())
                {
                    if (bApply)
                    {
                        Numeric->SetFloatingPointPropertyValue(Value, Number);
                    }
                    bSet = true;
                }
                else if (Number == FMath::TruncToDouble(Number))
                {
                    const int64 SignedValue = static_cast<int64>(Number);
                    if (Number < 0.0)
                    {
                        if (Numeric->CanHoldValue(SignedValue))
                        {
                            if (bApply)
                            {
                                Numeric->SetIntPropertyValue(Value, SignedValue);
                            }
                            bSet = true;
                        }
                    }
                    else
                    {
                        const uint64 UnsignedValue = static_cast<uint64>(Number);
                        if (Numeric->CanHoldValue(UnsignedValue))
                        {
                            if (bApply)
                            {
                                Numeric->SetIntPropertyValue(Value, UnsignedValue);
                            }
                            bSet = true;
                        }
                    }
                }
            }
        }
        else if (FStrProperty* String = CastField<FStrProperty>(Property); String != nullptr && JsonValue->Type == EJson::String)
        {
            const FString NewValue = JsonValue->AsString();
            int32 NullIndex = INDEX_NONE;
            if (NewValue.Len() <= 2048 && !NewValue.FindChar(TEXT('\0'), NullIndex))
            {
                if (bApply)
                {
                    String->SetPropertyValue(Value, NewValue);
                }
                bSet = true;
            }
        }
        else if (FNameProperty* Name = CastField<FNameProperty>(Property); Name != nullptr && JsonValue->Type == EJson::String)
        {
            const FString NewValue = JsonValue->AsString();
            if (NewValue.Len() <= 256 && !NewValue.Contains(TEXT("\r")) && !NewValue.Contains(TEXT("\n")))
            {
                if (bApply)
                {
                    Name->SetPropertyValue(Value, FName(*NewValue));
                }
                bSet = true;
            }
        }
        else if (FTextProperty* Text = CastField<FTextProperty>(Property); Text != nullptr && JsonValue->Type == EJson::String)
        {
            const FString NewValue = JsonValue->AsString();
            if (NewValue.Len() <= 2048)
            {
                if (bApply)
                {
                    Text->SetPropertyValue(Value, FText::FromString(NewValue));
                }
                bSet = true;
            }
        }
        else if (FStructProperty* Struct = CastField<FStructProperty>(Property); Struct != nullptr && JsonValue->Type == EJson::Object)
        {
            if (Struct->Struct == TBaseStructure<FVector>::Get())
            {
                FVector NewValue;
                if (ReadVector(JsonValue->AsObject(), NewValue))
                {
                    if (bApply)
                    {
                        *static_cast<FVector*>(Value) = NewValue;
                    }
                    bSet = true;
                }
            }
            else if (Struct->Struct == TBaseStructure<FRotator>::Get())
            {
                FRotator NewValue;
                if (ReadRotator(JsonValue->AsObject(), NewValue))
                {
                    if (bApply)
                    {
                        *static_cast<FRotator*>(Value) = NewValue;
                    }
                    bSet = true;
                }
            }
        }
        else if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
            ObjectProperty != nullptr && ObjectProperty->PropertyClass->IsChildOf(AActor::StaticClass()) &&
            JsonValue->Type == EJson::String)
        {
            FString FindError;
            AActor* ReferencedActor = FindActor(JsonValue->AsString(), FindError);
            if (ReferencedActor != nullptr && ReferencedActor->IsA(ObjectProperty->PropertyClass))
            {
                if (bApply)
                {
                    ObjectProperty->SetObjectPropertyValue(Value, ReferencedActor);
                }
                bSet = true;
            }
            else
            {
                OutError = FindError.IsEmpty() ? TEXT("The referenced actor has an incompatible class.") : FindError;
            }
        }

        if (!bSet)
        {
            if (OutError.IsEmpty())
            {
                OutError = TEXT("The JSON value does not match this property's safe supported type.");
            }
            return false;
        }

        if (bApply)
        {
            FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
            Object->PostEditChangeProperty(ChangeEvent);
            Object->MarkPackageDirty();
        }
        return true;
    }

    bool ConfirmSensitiveAction(const FString& Message)
    {
        if (FApp::IsUnattended())
        {
            return false;
        }
        return FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Message),
            FText::FromString(TEXT("CinderLink confirmation"))) == EAppReturnType::Yes;
    }

    bool IsInsideProjectWithoutReparsePoints(
        const FString& ProjectRoot,
        const FString& Candidate,
        FString& OutFullPath)
    {
        FString Root = FPaths::ConvertRelativePathToFull(ProjectRoot);
        FString Full = FPaths::ConvertRelativePathToFull(Candidate);
        FPaths::NormalizeDirectoryName(Root);
        FPaths::NormalizeFilename(Full);
        FString Prefix = Root;
        Prefix += TEXT("/");
        if (!Full.StartsWith(Prefix, ESearchCase::IgnoreCase))
        {
            return false;
        }

        FString Relative = Full.RightChop(Prefix.Len());
        TArray<FString> Parts;
        Relative.ParseIntoArray(Parts, TEXT("/"), true);
        FString Current = Root;
        for (const FString& Part : Parts)
        {
            Current = FPaths::Combine(Current, Part);
            const DWORD Attributes = ::GetFileAttributesW(*Current);
            if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                return false;
            }
        }
        OutFullPath = Full;
        return true;
    }

    TSharedRef<FJsonObject> ExecuteGetState(FString& OutSummary)
    {
        UWorld* World = GetEditorWorld();
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("editorWorldAvailable"), World != nullptr);
        Payload->SetBoolField(TEXT("inPIE"), IsPlaySessionActive());
        if (World != nullptr)
        {
            Payload->SetStringField(TEXT("currentLevel"), World->GetOutermost()->GetName());
            Payload->SetBoolField(TEXT("currentLevelDirty"), World->GetOutermost()->IsDirty());
            Payload->SetStringField(TEXT("worldName"), World->GetName());
        }

        if (GEditor != nullptr)
        {
            if (ULevelEditorSubsystem* LevelSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
            {
                FVector Location;
                FRotator Rotation;
                const FName Viewport = LevelSubsystem->GetActiveViewportConfigKey();
                if (!Viewport.IsNone() && LevelSubsystem->GetLevelViewportCameraInfo(Location, Rotation, Viewport))
                {
                    TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
                    Camera->SetStringField(TEXT("viewport"), Viewport.ToString());
                    Camera->SetObjectField(TEXT("location"), VectorToJson(Location));
                    Camera->SetObjectField(TEXT("rotation"), RotatorToJson(Rotation));
                    Payload->SetObjectField(TEXT("camera"), Camera);
                }
            }
        }
        return Success(Payload, TEXT("Read current Unreal Editor state."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteListActors(FString& OutSummary)
    {
        UWorld* World = GetEditorWorld();
        if (World == nullptr)
        {
            return Failure(TEXT("No editor world is currently available."), OutSummary);
        }

        TArray<TSharedPtr<FJsonValue>> Actors;
        bool bTruncated = false;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!IsValid(Actor) || Actor->HasAnyFlags(RF_Transient))
            {
                continue;
            }
            if (Actors.Num() >= MaximumActors)
            {
                bTruncated = true;
                break;
            }
            Actors.Add(MakeShared<FJsonValueObject>(ActorToJson(Actor, false)));
        }

        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
        Payload->SetArrayField(TEXT("actors"), Actors);
        Payload->SetBoolField(TEXT("truncated"), bTruncated);
        return Success(Payload, FString::Printf(TEXT("Listed %d actors."), Actors.Num()), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteGetActor(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        FString Selector;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("actor"), Selector))
        {
            return Failure(TEXT("The actor selector is required."), OutSummary);
        }
        FString FindError;
        AActor* Actor = FindActor(Selector, FindError);
        if (Actor == nullptr)
        {
            return Failure(FindError, OutSummary);
        }
        return Success(ActorToJson(Actor, true), TEXT("Read actor and safe editable properties."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteAssetQuery(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        FString Path;
        bool bRecursive = true;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("path"), Path) ||
            !IsSafeGamePath(Path, true) || HasWrongJsonType(Arguments, TEXT("recursive"), EJson::Boolean))
        {
            return Failure(TEXT("A valid /Game asset or directory path is required."), OutSummary);
        }
        Arguments->TryGetBoolField(TEXT("recursive"), bRecursive);
        if (GEditor == nullptr)
        {
            return Failure(TEXT("Unreal Editor is unavailable."), OutSummary);
        }
        UEditorAssetSubsystem* Assets = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
        if (Assets == nullptr)
        {
            return Failure(TEXT("Editor Asset Subsystem is unavailable."), OutSummary);
        }

        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), Path);
        Payload->SetBoolField(TEXT("exists"), Assets->DoesAssetExist(Path));
        Payload->SetBoolField(TEXT("directoryExists"), Assets->DoesDirectoryExist(Path));
        TArray<FString> Listed = Assets->ListAssets(Path, bRecursive, false);
        const bool bTruncated = Listed.Num() > 512;
        Listed.SetNum(FMath::Min(Listed.Num(), 512), EAllowShrinking::Yes);
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& Asset : Listed)
        {
            Values.Add(MakeShared<FJsonValueString>(Asset));
        }
        Payload->SetArrayField(TEXT("assets"), Values);
        Payload->SetBoolField(TEXT("truncated"), bTruncated);
        return Success(Payload, FString::Printf(TEXT("Queried %d assets."), Values.Num()), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteCreateLevel(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        FString AssetPath;
        bool bPartitioned = false;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
            !IsSafeGamePath(AssetPath) || HasWrongJsonType(Arguments, TEXT("partitioned"), EJson::Boolean))
        {
            return Failure(TEXT("A valid new /Game level asset_path is required."), OutSummary);
        }
        Arguments->TryGetBoolField(TEXT("partitioned"), bPartitioned);
        if (IsPlaySessionActive())
        {
            return Failure(TEXT("A level cannot be created while PIE is active."), OutSummary);
        }
        UWorld* CurrentWorld = GetEditorWorld();
        if (CurrentWorld != nullptr && CurrentWorld->GetOutermost()->IsDirty())
        {
            return Failure(TEXT("The current level is dirty. Save it explicitly before creating another level."), OutSummary);
        }
        UEditorAssetSubsystem* Assets = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
        ULevelEditorSubsystem* Levels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
        if (Assets == nullptr || Levels == nullptr || Assets->DoesAssetExist(AssetPath))
        {
            return Failure(TEXT("The target level already exists or an Editor subsystem is unavailable."), OutSummary);
        }
        if (!Levels->NewLevel(AssetPath, bPartitioned))
        {
            return Failure(TEXT("Unreal Editor could not create the requested level."), OutSummary);
        }
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("level"), AssetPath);
        Payload->SetBoolField(TEXT("partitioned"), bPartitioned);
        return Success(Payload, TEXT("Created a new project level."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteLoadLevel(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        FString AssetPath;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
            !IsSafeGamePath(AssetPath))
        {
            return Failure(TEXT("A valid /Game level asset_path is required."), OutSummary);
        }
        if (IsPlaySessionActive())
        {
            return Failure(TEXT("A level cannot be loaded while PIE is active."), OutSummary);
        }
        UWorld* CurrentWorld = GetEditorWorld();
        if (CurrentWorld != nullptr && CurrentWorld->GetOutermost()->IsDirty())
        {
            return Failure(TEXT("The current level is dirty. Save it explicitly before loading another level."), OutSummary);
        }
        UEditorAssetSubsystem* Assets = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
        ULevelEditorSubsystem* Levels = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
        if (Assets == nullptr || Levels == nullptr || !Assets->DoesAssetExist(AssetPath) || !Levels->LoadLevel(AssetPath))
        {
            return Failure(TEXT("Unreal Editor could not load the requested project level."), OutSummary);
        }
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("level"), AssetPath);
        return Success(Payload, TEXT("Loaded a project level."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteSaveLevel(FString& OutSummary)
    {
        if (IsPlaySessionActive())
        {
            return Failure(TEXT("The editor level cannot be saved while PIE is active."), OutSummary);
        }
        UWorld* World = GetEditorWorld();
        ULevelEditorSubsystem* Levels = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
        if (World == nullptr || Levels == nullptr || !IsSafeGamePath(World->GetOutermost()->GetName()) ||
            !Levels->SaveCurrentLevel())
        {
            return Failure(TEXT("Unreal Editor could not save the current /Game level."), OutSummary);
        }
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
        Payload->SetBoolField(TEXT("dirtyAfterSave"), World->GetOutermost()->IsDirty());
        return Success(Payload, TEXT("Saved the current project level."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteSpawnActor(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        FString ClassPath;
        FString Label;
        FString Folder;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("class_path"), ClassPath) ||
            !Arguments->TryGetStringField(TEXT("label"), Label) || !IsSafeClassPath(ClassPath) ||
            Label.IsEmpty() || !IsSafeLabelOrFolder(Label, 128) ||
            HasWrongJsonType(Arguments, TEXT("folder"), EJson::String) ||
            HasWrongJsonType(Arguments, TEXT("location"), EJson::Object) ||
            HasWrongJsonType(Arguments, TEXT("rotation"), EJson::Object) ||
            HasWrongJsonType(Arguments, TEXT("scale"), EJson::Object))
        {
            return Failure(TEXT("A safe actor class_path and non-empty label are required."), OutSummary);
        }
        Arguments->TryGetStringField(TEXT("folder"), Folder);
        if (!IsSafeLabelOrFolder(Folder, 256) || IsPlaySessionActive())
        {
            return Failure(TEXT("The folder is invalid or PIE is active."), OutSummary);
        }

        UWorld* World = GetEditorWorld();
        UEditorActorSubsystem* Actors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
        if (World == nullptr || Actors == nullptr || !IsSafeGamePath(World->GetOutermost()->GetName()))
        {
            return Failure(TEXT("A writable /Game editor level is required."), OutSummary);
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label, ESearchCase::CaseSensitive))
            {
                return Failure(TEXT("An actor with this exact label already exists."), OutSummary);
            }
        }

        UClass* ActorClass = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
        if (ActorClass == nullptr || !ActorClass->IsChildOf(AActor::StaticClass()) ||
            ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_NotPlaceable))
        {
            return Failure(TEXT("The supplied class is not a placeable actor class."), OutSummary);
        }

        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        FVector Scale = FVector::OneVector;
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (Arguments->TryGetObjectField(TEXT("location"), Object) && !ReadVector(*Object, Location))
        {
            return Failure(TEXT("location must contain finite x, y, and z values."), OutSummary);
        }
        if (Arguments->TryGetObjectField(TEXT("rotation"), Object) && !ReadRotator(*Object, Rotation))
        {
            return Failure(TEXT("rotation must contain finite pitch, yaw, and roll values."), OutSummary);
        }
        if (Arguments->TryGetObjectField(TEXT("scale"), Object) && !ReadVector(*Object, Scale))
        {
            return Failure(TEXT("scale must contain finite x, y, and z values."), OutSummary);
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("CinderLink: Spawn actor")));
        AActor* Actor = Actors->SpawnActorFromClass(ActorClass, Location, Rotation, false);
        if (Actor == nullptr)
        {
            return Failure(TEXT("Unreal Editor could not spawn the actor."), OutSummary);
        }
        Actor->Modify();
        Actor->SetActorLabel(Label, true);
        Actor->SetFolderPath(FName(*Folder));
        Actor->SetActorScale3D(Scale);
        Actor->MarkPackageDirty();
        return Success(ActorToJson(Actor, true), TEXT("Spawned an actor in the current level."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteUpdateActor(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        FString Selector;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("actor"), Selector) || IsPlaySessionActive())
        {
            return Failure(TEXT("An actor selector is required and PIE must be stopped."), OutSummary);
        }
        FString FindError;
        AActor* Actor = FindActor(Selector, FindError);
        if (Actor == nullptr)
        {
            return Failure(FindError, OutSummary);
        }
        UWorld* World = GetEditorWorld();
        if (World == nullptr || !IsSafeGamePath(World->GetOutermost()->GetName()))
        {
            return Failure(TEXT("Actor updates are limited to levels stored under /Game."), OutSummary);
        }

        FString Label;
        FString Folder;
        FString Mobility;
        const bool bHasLabel = Arguments->TryGetStringField(TEXT("label"), Label);
        const bool bHasFolder = Arguments->TryGetStringField(TEXT("folder"), Folder);
        const bool bHasMobility = Arguments->TryGetStringField(TEXT("mobility"), Mobility);
        if (HasWrongJsonType(Arguments, TEXT("label"), EJson::String) ||
            HasWrongJsonType(Arguments, TEXT("folder"), EJson::String) ||
            HasWrongJsonType(Arguments, TEXT("mobility"), EJson::String) ||
            HasWrongJsonType(Arguments, TEXT("location"), EJson::Object) ||
            HasWrongJsonType(Arguments, TEXT("rotation"), EJson::Object) ||
            HasWrongJsonType(Arguments, TEXT("scale"), EJson::Object) ||
            HasWrongJsonType(Arguments, TEXT("properties"), EJson::Object) ||
            (bHasLabel && (Label.IsEmpty() || !IsSafeLabelOrFolder(Label, 128))) ||
            (bHasFolder && !IsSafeLabelOrFolder(Folder, 256)))
        {
            return Failure(TEXT("The requested label or folder is invalid."), OutSummary);
        }
        if (bHasLabel)
        {
            for (TActorIterator<AActor> It(World); It; ++It)
            {
                if (*It != Actor && It->GetActorLabel().Equals(Label, ESearchCase::CaseSensitive))
                {
                    return Failure(TEXT("Another actor already has the requested label."), OutSummary);
                }
            }
        }

        FVector Location = Actor->GetActorLocation();
        FRotator Rotation = Actor->GetActorRotation();
        FVector Scale = Actor->GetActorScale3D();
        const TSharedPtr<FJsonObject>* Object = nullptr;
        const bool bHasLocation = Arguments->TryGetObjectField(TEXT("location"), Object);
        if (bHasLocation && !ReadVector(*Object, Location))
        {
            return Failure(TEXT("location must contain finite x, y, and z values."), OutSummary);
        }
        const bool bHasRotation = Arguments->TryGetObjectField(TEXT("rotation"), Object);
        if (bHasRotation && !ReadRotator(*Object, Rotation))
        {
            return Failure(TEXT("rotation must contain finite pitch, yaw, and roll values."), OutSummary);
        }
        const bool bHasScale = Arguments->TryGetObjectField(TEXT("scale"), Object);
        if (bHasScale && !ReadVector(*Object, Scale))
        {
            return Failure(TEXT("scale must contain finite x, y, and z values."), OutSummary);
        }

        const TSharedPtr<FJsonObject>* Properties = nullptr;
        const bool bHasProperties = Arguments->TryGetObjectField(TEXT("properties"), Properties) &&
            Properties != nullptr && Properties->IsValid();
        if (bHasProperties && (*Properties)->Values.Num() > MaximumEditableProperties)
        {
            return Failure(TEXT("Too many properties were requested in one action."), OutSummary);
        }

        USceneComponent* MobilityRoot = nullptr;
        EComponentMobility::Type NewMobility = EComponentMobility::Movable;
        if (bHasMobility)
        {
            MobilityRoot = Actor->GetRootComponent();
            if (MobilityRoot == nullptr)
            {
                return Failure(TEXT("This actor has no root component for mobility."), OutSummary);
            }
            if (Mobility.Equals(TEXT("Static"), ESearchCase::IgnoreCase))
            {
                NewMobility = EComponentMobility::Static;
            }
            else if (Mobility.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))
            {
                NewMobility = EComponentMobility::Stationary;
            }
            else if (Mobility.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))
            {
                NewMobility = EComponentMobility::Movable;
            }
            else
            {
                return Failure(TEXT("mobility must be Static, Stationary, or Movable."), OutSummary);
            }
        }

        if (bHasProperties)
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
            {
                FProperty* Property = FindFProperty<FProperty>(Actor->GetClass(), FName(*Pair.Key));
                FString PropertyError;
                if (!SetPropertyValue(Actor, Property, Pair.Value, false, PropertyError))
                {
                    return Failure(FString::Printf(TEXT("Property %s was rejected: %s"), *Pair.Key, *PropertyError), OutSummary);
                }
            }
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("CinderLink: Update actor")));
        Actor->Modify();
        if (bHasLocation || bHasRotation || bHasScale)
        {
            Actor->SetActorTransform(FTransform(Rotation, Location, Scale));
        }
        if (bHasLabel)
        {
            Actor->SetActorLabel(Label, true);
        }
        if (bHasFolder)
        {
            Actor->SetFolderPath(FName(*Folder));
        }
        if (bHasMobility)
        {
            MobilityRoot->Modify();
            MobilityRoot->SetMobility(NewMobility);
        }

        if (bHasProperties)
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
            {
                FProperty* Property = FindFProperty<FProperty>(Actor->GetClass(), FName(*Pair.Key));
                FString PropertyError;
                if (!SetPropertyValue(Actor, Property, Pair.Value, true, PropertyError))
                {
                    return Failure(FString::Printf(TEXT("Property %s was rejected: %s"), *Pair.Key, *PropertyError), OutSummary);
                }
            }
        }
        Actor->MarkPackageDirty();
        return Success(ActorToJson(Actor, true), TEXT("Updated an actor and returned fresh values."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteSetGameMode(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        FString ClassPath;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("class_path"), ClassPath) ||
            !IsSafeClassPath(ClassPath) || IsPlaySessionActive())
        {
            return Failure(TEXT("A safe GameMode class_path is required and PIE must be stopped."), OutSummary);
        }
        UWorld* World = GetEditorWorld();
        AWorldSettings* Settings = World != nullptr ? World->GetWorldSettings() : nullptr;
        UClass* GameModeClass = StaticLoadClass(AGameModeBase::StaticClass(), nullptr, *ClassPath);
        FClassProperty* Property = Settings != nullptr
            ? FindFProperty<FClassProperty>(Settings->GetClass(), TEXT("DefaultGameMode"))
            : nullptr;
        if (World == nullptr || !IsSafeGamePath(World->GetOutermost()->GetName()) ||
            Settings == nullptr || GameModeClass == nullptr || Property == nullptr ||
            !GameModeClass->IsChildOf(AGameModeBase::StaticClass()))
        {
            return Failure(TEXT("A /Game level, GameMode class, and World Settings property are required."), OutSummary);
        }
        const FScopedTransaction Transaction(FText::FromString(TEXT("CinderLink: Set GameMode")));
        Settings->Modify();
        Settings->PreEditChange(Property);
        Property->SetPropertyValue_InContainer(Settings, GameModeClass);
        FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
        Settings->PostEditChangeProperty(Event);
        Settings->MarkPackageDirty();

        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("class"), GameModeClass->GetPathName());
        Payload->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
        return Success(Payload, TEXT("Set the current level GameMode override."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteImportImage(
        const TSharedPtr<FJsonObject>& Arguments,
        const FString& ProjectRoot,
        FString& OutSummary)
    {
        FString RelativeSource;
        FString Destination;
        if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("source_relative_path"), RelativeSource) ||
            !Arguments->TryGetStringField(TEXT("destination_path"), Destination) ||
            RelativeSource.IsEmpty() || FPaths::IsRelative(RelativeSource) == false ||
            RelativeSource.Contains(TEXT("..")) || RelativeSource.Contains(TEXT(":")) ||
            !IsSafeGamePath(Destination, true))
        {
            return Failure(TEXT("Use a project-relative image source and a valid /Game destination directory."), OutSummary);
        }
        const FString Extension = FPaths::GetExtension(RelativeSource).ToLower();
        if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg") &&
            Extension != TEXT("exr") && Extension != TEXT("hdr"))
        {
            return Failure(TEXT("Only PNG, JPEG, EXR, and HDR image imports are allowed."), OutSummary);
        }
        FString FullSource;
        if (!IsInsideProjectWithoutReparsePoints(ProjectRoot, FPaths::Combine(ProjectRoot, RelativeSource), FullSource))
        {
            return Failure(TEXT("The source must be a real file inside the project with no reparse-point traversal."), OutSummary);
        }
        const int64 FileSize = IFileManager::Get().FileSize(*FullSource);
        if (FileSize <= 0 || FileSize > MaximumImportBytes)
        {
            return Failure(TEXT("The image is empty or exceeds CinderLink's 32 MiB import limit."), OutSummary);
        }
        FString AssetName = FPaths::GetBaseFilename(FullSource);
        if (AssetName.IsEmpty())
        {
            return Failure(TEXT("The image must have a non-empty safe base name."), OutSummary);
        }
        for (const TCHAR Character : AssetName)
        {
            if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
            {
                return Failure(TEXT("The image base name may contain only letters, numbers, and underscores."), OutSummary);
            }
        }
        Destination.RemoveFromEnd(TEXT("/"));
        const FString ExpectedAsset = Destination + TEXT("/") + AssetName;
        UEditorAssetSubsystem* Assets = GEditor != nullptr ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
        if (Assets == nullptr || Assets->DoesAssetExist(ExpectedAsset))
        {
            return Failure(TEXT("The destination asset already exists or the Asset Subsystem is unavailable."), OutSummary);
        }

        UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
        ImportData->GroupName = TEXT("CinderLink image import");
        ImportData->Filenames = {FullSource};
        ImportData->DestinationPath = Destination;
        ImportData->bReplaceExisting = false;
        ImportData->bSkipReadOnly = true;
        TArray<UObject*> Imported = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"))
            .Get().ImportAssetsAutomated(ImportData);
        if (Imported.IsEmpty())
        {
            return Failure(TEXT("Unreal Editor did not import an image asset."), OutSummary);
        }

        TArray<TSharedPtr<FJsonValue>> Paths;
        for (UObject* Asset : Imported)
        {
            if (IsValid(Asset))
            {
                Assets->SaveLoadedAsset(Asset, false);
                Paths.Add(MakeShared<FJsonValueString>(Asset->GetPathName()));
            }
        }
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("importedAssets"), Paths);
        return Success(Payload, TEXT("Imported and saved a project-local image."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteMapCheck(FString& OutSummary)
    {
        UWorld* World = GetEditorWorld();
        if (GEditor == nullptr || World == nullptr)
        {
            return Failure(TEXT("No editor world is available for Map Check."), OutSummary);
        }
        FStringOutputDevice Output;
        const bool bExecuted = GEditor->Exec(World, TEXT("MAP CHECK DONTDISPLAYDIALOG"), Output);
        if (!bExecuted)
        {
            return Failure(TEXT("Unreal Editor did not execute Map Check."), OutSummary);
        }
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("executed"), bExecuted);
        Payload->SetStringField(TEXT("output"), FString(Output).Left(16384));
        Payload->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
        return Success(Payload, TEXT("Ran Map Check without opening its results window."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteSetCamera(const TSharedPtr<FJsonObject>& Arguments, FString& OutSummary)
    {
        const TSharedPtr<FJsonObject>* LocationObject = nullptr;
        const TSharedPtr<FJsonObject>* RotationObject = nullptr;
        FVector Location;
        FRotator Rotation;
        if (!Arguments.IsValid() || !Arguments->TryGetObjectField(TEXT("location"), LocationObject) ||
            !Arguments->TryGetObjectField(TEXT("rotation"), RotationObject) ||
            !ReadVector(*LocationObject, Location) || !ReadRotator(*RotationObject, Rotation) ||
            IsPlaySessionActive())
        {
            return Failure(TEXT("Valid camera location and rotation are required, with PIE stopped."), OutSummary);
        }
        ULevelEditorSubsystem* Levels = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
        const FName Viewport = Levels != nullptr ? Levels->GetActiveViewportConfigKey() : NAME_None;
        if (Levels == nullptr || Viewport.IsNone())
        {
            return Failure(TEXT("No active level viewport is available."), OutSummary);
        }
        Levels->SetLevelViewportCameraInfo(Location, Rotation, Viewport);
        Levels->EditorInvalidateViewports();
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("viewport"), Viewport.ToString());
        Payload->SetObjectField(TEXT("location"), VectorToJson(Location));
        Payload->SetObjectField(TEXT("rotation"), RotatorToJson(Rotation));
        return Success(Payload, TEXT("Moved the active editor viewport camera."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecuteCapture(const FString& ProjectRoot, FString& OutSummary)
    {
        if (!ConfirmSensitiveAction(
            TEXT("Allow CinderLink to capture the active Unreal viewport and send that image to the configured AI model for this tool call?")))
        {
            return Failure(TEXT("The user declined viewport capture or the editor is unattended."), OutSummary);
        }
        FViewport* Viewport = GEditor != nullptr ? GEditor->GetActiveViewport() : nullptr;
        if (Viewport == nullptr)
        {
            return Failure(TEXT("No active Unreal viewport is available."), OutSummary);
        }
        const FIntPoint SourceSize = Viewport->GetSizeXY();
        TArray<FColor> Pixels;
        if (SourceSize.X <= 0 || SourceSize.Y <= 0 || !Viewport->ReadPixels(Pixels) ||
            Pixels.Num() != SourceSize.X * SourceSize.Y)
        {
            return Failure(TEXT("The active viewport could not be captured."), OutSummary);
        }

        int32 Width = SourceSize.X;
        int32 Height = SourceSize.Y;
        TArray<FColor> Resized;
        const int32 Largest = FMath::Max(Width, Height);
        if (Largest > MaximumCaptureDimension)
        {
            const double Scale = static_cast<double>(MaximumCaptureDimension) / Largest;
            const int32 NewWidth = FMath::Max(1, FMath::RoundToInt(Width * Scale));
            const int32 NewHeight = FMath::Max(1, FMath::RoundToInt(Height * Scale));
            FImageUtils::ImageResize(Width, Height, Pixels, NewWidth, NewHeight, Resized, true, true);
            Pixels = MoveTemp(Resized);
            Width = NewWidth;
            Height = NewHeight;
        }

        TArray64<uint8> Png64;
        FImageUtils::PNGCompressImageArray(
            Width,
            Height,
            TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()),
            Png64);
        if (Png64.IsEmpty() || Png64.Num() > MAX_int32)
        {
            return Failure(TEXT("The captured viewport could not be encoded safely."), OutSummary);
        }
        TArray<uint8> Png;
        Png.Append(Png64.GetData(), static_cast<int32>(Png64.Num()));

        const FString CaptureDirectory = FPaths::Combine(ProjectRoot, TEXT("Saved/CinderLink/Captures"));
        if (!IFileManager::Get().MakeDirectory(*CaptureDirectory, true))
        {
            return Failure(TEXT("Could not create the project-local capture directory."), OutSummary);
        }
        const FString FileName = FString::Printf(TEXT("Viewport-%s.png"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")));
        const FString CapturePath = FPaths::Combine(CaptureDirectory, FileName);
        if (!FFileHelper::SaveArrayToFile(Png, *CapturePath))
        {
            return Failure(TEXT("Could not save the project-local viewport capture."), OutSummary);
        }

        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("projectRelativePath"), FString(TEXT("Saved/CinderLink/Captures/")) + FileName);
        Payload->SetNumberField(TEXT("width"), Width);
        Payload->SetNumberField(TEXT("height"), Height);
        Payload->SetBoolField(TEXT("sentToModel"), true);
        const FString DataUrl = TEXT("data:image/png;base64,") + FBase64::Encode(Png);
        return Success(Payload, TEXT("Captured the viewport after explicit user confirmation."), OutSummary, DataUrl);
    }

    TSharedRef<FJsonObject> ExecutePieStart(FString& OutSummary)
    {
        ULevelEditorSubsystem* Levels = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
        if (Levels == nullptr || Levels->IsInPlayInEditor())
        {
            return Failure(TEXT("PIE is already active or the Level Editor Subsystem is unavailable."), OutSummary);
        }
        if (!ConfirmSensitiveAction(
            TEXT("Allow CinderLink to start Play In Editor? Project runtime code may access configured hardware or network services.")))
        {
            return Failure(TEXT("The user declined PIE start or the editor is unattended."), OutSummary);
        }
        Levels->EditorRequestBeginPlay();
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("startRequested"), true);
        return Success(Payload, TEXT("Requested PIE start after explicit user confirmation."), OutSummary);
    }

    TSharedRef<FJsonObject> ExecutePieStop(FString& OutSummary)
    {
        ULevelEditorSubsystem* Levels = GEditor != nullptr ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
        if (Levels == nullptr || !Levels->IsInPlayInEditor())
        {
            return Failure(TEXT("PIE is not active."), OutSummary);
        }
        Levels->EditorRequestEndPlay();
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("stopRequested"), true);
        return Success(Payload, TEXT("Requested PIE stop."), OutSummary);
    }
}

TArray<TSharedPtr<FJsonValue>> FCinderLinkEditorTools::BuildToolSpecs()
{
    TArray<TSharedPtr<FJsonValue>> Tools;

    Tools.Add(ToolSpec(
        TEXT("ue_editor_get_state"),
        TEXT("Read the current Unreal Editor level, dirty state, PIE state, and active viewport camera. Does not modify the project."),
        ObjectSchema(MakeShared<FJsonObject>())));
    Tools.Add(ToolSpec(
        TEXT("ue_level_list_actors"),
        TEXT("List actors in the current editor level with stable ids, labels, classes, folders, transforms, and mobility."),
        ObjectSchema(MakeShared<FJsonObject>())));

    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("actor"), StringSchema(TEXT("Actor id, exact label, object name, or full object path.")));
        Tools.Add(ToolSpec(
            TEXT("ue_level_get_actor"),
            TEXT("Read one actor and its safe editable primitive properties. Secret- or token-named properties are always excluded."),
            ObjectSchema(Properties, {TEXT("actor")})));
    }
    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("path"), StringSchema(TEXT("A /Game asset or directory path.")));
        Properties->SetObjectField(TEXT("recursive"), BooleanSchema(TEXT("Whether to list descendant assets.")));
        Tools.Add(ToolSpec(
            TEXT("ue_asset_query"),
            TEXT("Check a /Game asset path and list project assets without loading arbitrary host files."),
            ObjectSchema(Properties, {TEXT("path")})));
    }
    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("asset_path"), StringSchema(TEXT("New /Game level package path. Existing assets are never overwritten.")));
        Properties->SetObjectField(TEXT("partitioned"), BooleanSchema(TEXT("Create a World Partition level when true.")));
        Tools.Add(ToolSpec(
            TEXT("ue_level_create"),
            TEXT("Create and open a new level under /Game. Requires Allow UE Editor actions and refuses dirty-level or overwrite cases."),
            ObjectSchema(Properties, {TEXT("asset_path")})));
    }
    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("asset_path"), StringSchema(TEXT("Existing /Game level package path.")));
        Tools.Add(ToolSpec(
            TEXT("ue_level_load"),
            TEXT("Load an existing /Game level. Requires Allow UE Editor actions and refuses to discard dirty changes."),
            ObjectSchema(Properties, {TEXT("asset_path")})));
    }
    Tools.Add(ToolSpec(
        TEXT("ue_level_save"),
        TEXT("Save the current /Game level. Requires Allow UE Editor actions."),
        ObjectSchema(MakeShared<FJsonObject>())));

    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("class_path"), StringSchema(TEXT("Actor class under /Script or /Game.")));
        Properties->SetObjectField(TEXT("label"), StringSchema(TEXT("Unique exact actor label.")));
        Properties->SetObjectField(TEXT("folder"), StringSchema(TEXT("Optional World Outliner folder.")));
        Properties->SetObjectField(TEXT("location"), VectorSchema(TEXT("Optional world location in centimeters.")));
        Properties->SetObjectField(TEXT("rotation"), VectorSchema(TEXT("Optional world rotation in degrees."), true));
        Properties->SetObjectField(TEXT("scale"), VectorSchema(TEXT("Optional actor scale.")));
        Tools.Add(ToolSpec(
            TEXT("ue_level_spawn_actor"),
            TEXT("Spawn a non-abstract actor in the current /Game level. Requires Allow UE Editor actions; no deletion or replacement is possible."),
            ObjectSchema(Properties, {TEXT("class_path"), TEXT("label")})));
    }
    {
        TSharedRef<FJsonObject> AnyProperties = MakeShared<FJsonObject>();
        AnyProperties->SetStringField(TEXT("type"), TEXT("object"));
        AnyProperties->SetStringField(TEXT("description"), TEXT("At most 32 exact editable property names mapped to primitive JSON values or current-level actor ids."));
        AnyProperties->SetBoolField(TEXT("additionalProperties"), true);

        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("actor"), StringSchema(TEXT("Actor id, exact label, object name, or full object path.")));
        Properties->SetObjectField(TEXT("label"), StringSchema(TEXT("Optional new unique label.")));
        Properties->SetObjectField(TEXT("folder"), StringSchema(TEXT("Optional World Outliner folder.")));
        Properties->SetObjectField(TEXT("location"), VectorSchema(TEXT("Optional world location.")));
        Properties->SetObjectField(TEXT("rotation"), VectorSchema(TEXT("Optional world rotation."), true));
        Properties->SetObjectField(TEXT("scale"), VectorSchema(TEXT("Optional actor scale.")));
        Properties->SetObjectField(TEXT("mobility"), StringSchema(TEXT("Static, Stationary, or Movable.")));
        Properties->SetObjectField(TEXT("properties"), AnyProperties);
        Tools.Add(ToolSpec(
            TEXT("ue_level_update_actor"),
            TEXT("Update transform, label, folder, mobility, and safe editable primitive properties, then return fresh values. Requires Allow UE Editor actions."),
            ObjectSchema(Properties, {TEXT("actor")})));
    }
    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("class_path"), StringSchema(TEXT("AGameModeBase subclass path under /Script or /Game.")));
        Tools.Add(ToolSpec(
            TEXT("ue_level_set_game_mode"),
            TEXT("Set the current level World Settings GameMode override. Requires Allow UE Editor actions."),
            ObjectSchema(Properties, {TEXT("class_path")})));
    }
    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("source_relative_path"), StringSchema(TEXT("PNG, JPEG, EXR, or HDR path relative to the project root. Reparse points are refused.")));
        Properties->SetObjectField(TEXT("destination_path"), StringSchema(TEXT("Destination directory under /Game. Existing assets are never replaced.")));
        Tools.Add(ToolSpec(
            TEXT("ue_asset_import_image"),
            TEXT("Import a project-local image of at most 32 MiB into a new /Game asset. Requires Allow UE Editor actions."),
            ObjectSchema(Properties, {TEXT("source_relative_path"), TEXT("destination_path")})));
    }
    Tools.Add(ToolSpec(
        TEXT("ue_level_map_check"),
        TEXT("Run Unreal Map Check on the current editor world without opening the results window."),
        ObjectSchema(MakeShared<FJsonObject>())));
    {
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("location"), VectorSchema(TEXT("Editor viewport camera location.")));
        Properties->SetObjectField(TEXT("rotation"), VectorSchema(TEXT("Editor viewport camera rotation."), true));
        Tools.Add(ToolSpec(
            TEXT("ue_viewport_set_camera"),
            TEXT("Move the active level viewport camera. Requires Allow UE Editor actions."),
            ObjectSchema(Properties, {TEXT("location"), TEXT("rotation")})));
    }
    Tools.Add(ToolSpec(
        TEXT("ue_viewport_capture"),
        TEXT("Capture the active Unreal viewport and send it to the model. Requires Allow UE Editor actions plus a visible per-call user confirmation."),
        ObjectSchema(MakeShared<FJsonObject>())));
    Tools.Add(ToolSpec(
        TEXT("ue_pie_start"),
        TEXT("Request Play In Editor. Requires Allow UE Editor actions plus a visible per-call user confirmation because runtime code may access hardware or networks."),
        ObjectSchema(MakeShared<FJsonObject>())));
    Tools.Add(ToolSpec(
        TEXT("ue_pie_stop"),
        TEXT("Stop the active Play In Editor session. Requires Allow UE Editor actions."),
        ObjectSchema(MakeShared<FJsonObject>())));
    return Tools;
}

bool FCinderLinkEditorTools::IsKnownTool(const FString& ToolName)
{
    return ReadTools().Contains(ToolName) || MutationTools().Contains(ToolName);
}

bool FCinderLinkEditorTools::IsMutationTool(const FString& ToolName)
{
    return MutationTools().Contains(ToolName);
}

TSharedRef<FJsonObject> FCinderLinkEditorTools::Execute(
    const FString& ToolName,
    const TSharedPtr<FJsonObject>& Arguments,
    const FString& ProjectRoot,
    bool bAllowEditorActions,
    FString& OutSummary)
{
    if (!IsKnownTool(ToolName))
    {
        return Failure(TEXT("CinderLink does not expose this Unreal Editor tool."), OutSummary);
    }
    if (IsMutationTool(ToolName) && !bAllowEditorActions)
    {
        return Failure(TEXT("This turn did not enable Allow UE Editor actions."), OutSummary);
    }
    if (!IsInGameThread())
    {
        return Failure(TEXT("UE Editor tools must execute on the game thread."), OutSummary);
    }

    if (ToolName == TEXT("ue_editor_get_state")) return ExecuteGetState(OutSummary);
    if (ToolName == TEXT("ue_level_list_actors")) return ExecuteListActors(OutSummary);
    if (ToolName == TEXT("ue_level_get_actor")) return ExecuteGetActor(Arguments, OutSummary);
    if (ToolName == TEXT("ue_asset_query")) return ExecuteAssetQuery(Arguments, OutSummary);
    if (ToolName == TEXT("ue_level_create")) return ExecuteCreateLevel(Arguments, OutSummary);
    if (ToolName == TEXT("ue_level_load")) return ExecuteLoadLevel(Arguments, OutSummary);
    if (ToolName == TEXT("ue_level_save")) return ExecuteSaveLevel(OutSummary);
    if (ToolName == TEXT("ue_level_spawn_actor")) return ExecuteSpawnActor(Arguments, OutSummary);
    if (ToolName == TEXT("ue_level_update_actor")) return ExecuteUpdateActor(Arguments, OutSummary);
    if (ToolName == TEXT("ue_level_set_game_mode")) return ExecuteSetGameMode(Arguments, OutSummary);
    if (ToolName == TEXT("ue_asset_import_image")) return ExecuteImportImage(Arguments, ProjectRoot, OutSummary);
    if (ToolName == TEXT("ue_level_map_check")) return ExecuteMapCheck(OutSummary);
    if (ToolName == TEXT("ue_viewport_set_camera")) return ExecuteSetCamera(Arguments, OutSummary);
    if (ToolName == TEXT("ue_viewport_capture")) return ExecuteCapture(ProjectRoot, OutSummary);
    if (ToolName == TEXT("ue_pie_start")) return ExecutePieStart(OutSummary);
    if (ToolName == TEXT("ue_pie_stop")) return ExecutePieStop(OutSummary);
    return Failure(TEXT("CinderLink rejected an unreachable tool dispatch."), OutSummary);
}
