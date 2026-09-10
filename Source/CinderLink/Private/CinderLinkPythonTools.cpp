// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkPythonTools.h"

#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "IPythonScriptPlugin.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Windows/WindowsHWrapper.h"

namespace
{
    constexpr int32 MaxCodeCharacters = 128 * 1024;
    constexpr int32 MaxOutputCharacters = 32 * 1024;
    constexpr int32 MaxFiles = 10000;
    constexpr int64 MaxBackupBytes = 1024ll * 1024 * 1024;
    bool bExecutingPython = false;

    FString Json(const TSharedRef<FJsonObject>& Value)
    {
        FString Text;
        const auto Writer = TJsonWriterFactory<>::Create(&Text);
        FJsonSerializer::Serialize(Value, Writer);
        return Text;
    }

    TSharedRef<FJsonObject> Result(bool bSuccess, const FString& Message)
    {
        auto Value = MakeShared<FJsonObject>();
        Value->SetBoolField(TEXT("success"), bSuccess);
        Value->SetStringField(TEXT("message"), Message);
        return Value;
    }

    bool Identifier(const FString& Text, int32 Limit = 64)
    {
        if (Text.IsEmpty() || Text.Len() > Limit) return false;
        for (TCHAR C : Text)
        {
            if (!(C >= 'a' && C <= 'z') && !(C >= 'A' && C <= 'Z') &&
                !(C >= '0' && C <= '9') && C != '_' && C != '-') return false;
        }
        const FString Upper = Text.ToUpper();
        if (Upper == TEXT("CON") || Upper == TEXT("PRN") || Upper == TEXT("AUX") || Upper == TEXT("NUL") ||
            (Upper.Len() == 4 && (Upper.StartsWith(TEXT("COM")) || Upper.StartsWith(TEXT("LPT"))) &&
            Upper[3] >= '0' && Upper[3] <= '9')) return false;
        return true;
    }

    // Validates bridge-owned paths, including existing ancestors, before any I/O.
    // This is NOT a sandbox for the Python code subsequently run inside Unreal.
    bool LocalPath(const FString& Root, const FString& Relative, FString& Full)
    {
        if (Relative.IsEmpty() || Relative.Contains(TEXT("\\")) || Relative.Contains(TEXT(":")) ||
            Relative.StartsWith(TEXT("/"))) return false;
        TArray<FString> Parts;
        Relative.ParseIntoArray(Parts, TEXT("/"), false);
        for (const FString& Part : Parts)
        {
            if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") ||
                Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" "))) return false;
        }
        FString Base = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Base);
        Full = FPaths::Combine(Base, Relative);
        FString Current = Full;
        while (!Current.IsEmpty())
        {
            const DWORD Attributes = ::GetFileAttributesW(*Current);
            if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            const FString Parent = FPaths::GetPath(Current);
            if (Parent == Current) break;
            Current = Parent;
        }
        return Full.StartsWith(Base + TEXT("/"), ESearchCase::IgnoreCase);
    }

    bool SaveJson(const FString& Path, const TSharedRef<FJsonObject>& Value)
    {
        return FFileHelper::SaveStringToFile(Json(Value), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    TSharedPtr<FJsonObject> LoadJson(const FString& Path)
    {
        if (IFileManager::Get().FileSize(*Path) > 8 * 1024 * 1024) return nullptr;
        FString Text;
        TSharedPtr<FJsonObject> Value;
        if (FFileHelper::LoadFileToString(Text, *Path))
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value);
        return Value;
    }

    bool Scan(const FString& Root, const FString& Relative, TMap<FString, FString>& Files,
              int64& Bytes, FString& Error)
    {
        FString Full;
        if (!LocalPath(Root, Relative, Full)) { Error = TEXT("A backup path contains a link or invalid component."); return false; }
        if (IFileManager::Get().DirectoryExists(*Full))
        {
            TArray<FString> Children;
            IFileManager::Get().FindFiles(Children, *(Full / TEXT("*")), true, true);
            for (const FString& Child : Children)
                if (!Scan(Root, Relative / Child, Files, Bytes, Error)) return false;
        }
        else if (IFileManager::Get().FileExists(*Full) && !Files.Contains(Relative))
        {
            Bytes += IFileManager::Get().FileSize(*Full);
            if (Bytes > MaxBackupBytes || Files.Num() >= MaxFiles)
            { Error = TEXT("Backup exceeds 1 GiB or 10,000 files. Work on a smaller copied content folder."); return false; }
            const FMD5Hash Hash = FMD5Hash::HashFile(*Full);
            if (!Hash.IsValid()) { Error = TEXT("A backup file could not be read."); return false; }
            Files.Add(Relative, LexToString(Hash));
        }
        return true;
    }

    bool BackupRoots(const TSharedPtr<FJsonObject>& Args, TArray<FString>& Roots, FString& Error)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Args->TryGetArrayField(TEXT("backup_paths"), Values) || !Values || Values->Num() > 32)
        { Error = TEXT("Supply backup_paths: up to 32 /Game content directories; [] is for inspection-only scripts."); return false; }
        for (const auto& Value : *Values)
        {
            FString Path;
            if (!Value.IsValid() || !Value->TryGetString(Path) ||
                !(Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"))) )
            { Error = TEXT("backup_paths must contain /Game directories."); return false; }
            TArray<FString> Parts;
            Path.RightChop(1).ParseIntoArray(Parts, TEXT("/"), false);
            for (const auto& Part : Parts)
                if (!Identifier(Part, 128)) { Error = TEXT("Use /Game directory names containing letters, digits, underscores or hyphens."); return false; }
            const FString Relative = TEXT("Content") + Path.RightChop(5);
            Roots.AddUnique(Relative);
        }
        return true;
    }

    bool HasDirtyPackages(const TArray<FString>& Roots, FString& Error)
    {
        for (TObjectIterator<UPackage> It; It; ++It)
        {
            if (!It->IsDirty()) continue;
            for (const FString& Root : Roots)
            {
                const FString PackageRoot = TEXT("/Game") + Root.RightChop(7);
                if (It->GetName() == PackageRoot || It->GetName().StartsWith(PackageRoot + TEXT("/")))
                { Error = TEXT("Save or revert dirty assets in the backup folders first: ") + It->GetName(); return true; }
            }
        }
        return false;
    }

    TSharedRef<FJsonObject> Run(const TSharedPtr<FJsonObject>& Args, const FString& Root)
    {
        FString Code, Label, Error;
        if (!Args->TryGetStringField(TEXT("code"), Code) || Code.TrimStartAndEnd().IsEmpty() ||
            Code.Len() > MaxCodeCharacters)
            return Result(false, TEXT("code must be nonempty Python of at most 131,072 characters without NUL."));
        for (int32 Index = 0; Index < Code.Len(); ++Index)
            if (Code[Index] == 0) return Result(false, TEXT("NUL is not valid Python source."));
        if (!Args->TryGetStringField(TEXT("label"), Label) || !Identifier(Label))
            return Result(false, TEXT("label must contain 1-64 letters, digits, underscores or hyphens."));
        if (bExecutingPython || !GEditor || GEditor->PlayWorld || GEditor->IsPlaySessionRequestQueued())
            return Result(false, TEXT("Stop PIE/Simulate and finish the current Python call before authoring."));
        IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
        if (!Python || !Python->IsPythonAvailable() || !Python->IsPythonInitialized())
            return Result(false, TEXT("Enable Python Editor Script Plugin and restart Unreal Editor; Python is not ready."));
        TArray<FString> Roots;
        if (!BackupRoots(Args, Roots, Error) || HasDirtyPackages(Roots, Error)) return Result(false, Error);
        TMap<FString, FString> Before;
        int64 Bytes = 0;
        for (const auto& Path : Roots)
            if (!Scan(Root, Path, Before, Bytes, Error)) return Result(false, Error);

        const FString RunId = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString RunRelative = TEXT("Saved/CinderLink/Python/") + RunId;
        FString RunPath;
        if (!LocalPath(Root, RunRelative, RunPath) || !IFileManager::Get().MakeDirectory(*RunPath, true))
            return Result(false, TEXT("Could not create a project-local Python run directory."));
        const FString Script = RunPath / TEXT("script.py");
        if (!FFileHelper::SaveStringToFile(Code, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            return Result(false, TEXT("Could not archive the script; nothing executed."));

        auto Record = Result(false, TEXT("Prepared; Python has not completed. No automatic rollback is performed."));
        Record->SetStringField(TEXT("run_id"), RunId);
        Record->SetStringField(TEXT("label"), Label);
        Record->SetStringField(TEXT("script_path"), RunRelative / TEXT("script.py"));
        Record->SetStringField(TEXT("script_md5"), LexToString(FMD5Hash::HashFile(*Script)));
        Record->SetStringField(TEXT("status"), TEXT("prepared"));
        Record->SetStringField(TEXT("backup_scope"), TEXT("Declared content directories on disk only. Unsaved state, dependencies elsewhere and arbitrary Python effects are not covered."));
        TArray<TSharedPtr<FJsonValue>> RootValues, Backups;
        for (const auto& Path : Roots) RootValues.Add(MakeShared<FJsonValueString>(Path));
        Record->SetArrayField(TEXT("backup_roots"), RootValues);
        for (const auto& Pair : Before)
        {
            FString Source, Destination;
            if (!LocalPath(Root, Pair.Key, Source) || !LocalPath(Root, RunRelative / TEXT("backup") / Pair.Key, Destination) ||
                !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true) ||
                IFileManager::Get().Copy(*Destination, *Source, false) != COPY_OK ||
                LexToString(FMD5Hash::HashFile(*Destination)) != Pair.Value)
                return Result(false, TEXT("Backup copy/verification failed; nothing executed. Partial archive: ") + RunRelative);
            auto Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("path"), Pair.Key);
            Entry->SetStringField(TEXT("md5"), Pair.Value);
            Backups.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Record->SetArrayField(TEXT("backups"), Backups);
        const FString Manifest = RunPath / TEXT("result.json");
        if (!SaveJson(Manifest, Record)) return Result(false, TEXT("Could not save the backup manifest; nothing executed."));

        Record->SetStringField(TEXT("status"), TEXT("running"));
        if (!SaveJson(Manifest, Record)) return Result(false, TEXT("Could not record execution start; nothing executed."));
        const double Started = FPlatformTime::Seconds();
        FPythonCommandEx Command;
        Command.Command = FString::Printf(TEXT("\"%s\""), *Script);
        Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
        Command.FileExecutionScope = EPythonFileExecutionScope::Private;
        Command.Flags = EPythonCommandFlags::Unattended;
        bool bSuccess;
        {
            TGuardValue<bool> Executing(bExecutingPython, true);
            bSuccess = Python->ExecPythonCommandEx(Command);
        }
        Record->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
        Record->SetBoolField(TEXT("python_success"), bSuccess);
        Record->SetStringField(TEXT("status"), bSuccess ? TEXT("completed") : TEXT("failed"));
        FString Output;
        bool bTruncated = false;
        for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
        {
            const FString Line = FString(LexToString(Entry.Type)) + TEXT(": ") + Entry.Output + TEXT("\n");
            const int32 Remaining = MaxOutputCharacters - Output.Len();
            Output += Line.Left(Remaining);
            bTruncated |= Line.Len() > Remaining;
            if (Output.Len() == MaxOutputCharacters) { bTruncated = true; break; }
        }
        Record->SetStringField(TEXT("output"), Output);
        Record->SetStringField(TEXT("error"), bSuccess ? FString() : Command.CommandResult.Left(MaxOutputCharacters));
        Record->SetBoolField(TEXT("output_truncated"), bTruncated || Command.CommandResult.Len() > MaxOutputCharacters);

        TMap<FString, FString> After;
        Bytes = 0;
        bool bScanned = true;
        for (const auto& Path : Roots)
            if (!Scan(Root, Path, After, Bytes, Error)) { bScanned = false; break; }
        TArray<TSharedPtr<FJsonValue>> Changes;
        if (bScanned)
        {
            TSet<FString> Paths;
            for (const auto& Pair : Before) Paths.Add(Pair.Key);
            for (const auto& Pair : After) Paths.Add(Pair.Key);
            for (const auto& Path : Paths)
            {
                const FString* Previous = Before.Find(Path);
                const FString* Current = After.Find(Path);
                if (Previous && Current && *Previous == *Current) continue;
                auto Change = MakeShared<FJsonObject>();
                Change->SetStringField(TEXT("path"), Path);
                Change->SetStringField(TEXT("change"), !Previous ? TEXT("created") : !Current ? TEXT("deleted") : TEXT("modified"));
                Changes.Add(MakeShared<FJsonValueObject>(Change));
            }
        }
        Record->SetArrayField(TEXT("disk_changes"), Changes);
        Record->SetBoolField(TEXT("change_scan_complete"), bScanned);
        if (!bScanned) Record->SetStringField(TEXT("change_scan_error"), Error);
        Record->SetBoolField(TEXT("success"), bSuccess && bScanned);
        Record->SetStringField(TEXT("message"), FString::Printf(TEXT("Python %s; %d backed-up files; %d disk changes. Run: %s"),
            bSuccess ? TEXT("completed") : TEXT("failed (changes may remain)"), Before.Num(), Changes.Num(), *RunId));
        if (!SaveJson(Manifest, Record))
        {
            Record->SetBoolField(TEXT("success"), false);
            Record->SetStringField(TEXT("message"), TEXT("Python ran, but the result archive could not be updated. Preserve this response; do not retry blindly."));
        }
        return Record;
    }

    TSharedRef<FJsonObject> ReadRun(const TSharedPtr<FJsonObject>& Args, const FString& Root)
    {
        FString RunId, Path;
        if (!Args->TryGetStringField(TEXT("run_id"), RunId) || !Identifier(RunId, 80) ||
            !LocalPath(Root, TEXT("Saved/CinderLink/Python/") + RunId + TEXT("/result.json"), Path))
            return Result(false, TEXT("Invalid project-local run id."));
        auto Record = LoadJson(Path);
        FString StoredId, Message;
        bool bRecordedSuccess;
        if (!Record.IsValid() || !Record->TryGetStringField(TEXT("run_id"), StoredId) || StoredId != RunId ||
            !Record->TryGetBoolField(TEXT("success"), bRecordedSuccess) || !Record->TryGetStringField(TEXT("message"), Message))
            return Result(false, TEXT("No valid result exists for this run."));
        return Record.ToSharedRef();
    }

    TSharedRef<FJsonObject> SaveRecipe(const TSharedPtr<FJsonObject>& Args, const FString& Root)
    {
        auto Record = ReadRun(Args, Root);
        FString RunId, Name, Source, Destination, Hash;
        if (!Record->TryGetStringField(TEXT("run_id"), RunId) || !Identifier(RunId, 80) ||
            !Record->TryGetStringField(TEXT("script_md5"), Hash)) return Result(false, TEXT("Run archive is missing."));
        if (!Args->TryGetStringField(TEXT("name"), Name) || !Identifier(Name) ||
            !LocalPath(Root, TEXT("Saved/CinderLink/Python/") + RunId + TEXT("/script.py"), Source) ||
            !LocalPath(Root, TEXT("Scripts/CinderLink/") + Name + TEXT(".py"), Destination))
            return Result(false, TEXT("Invalid recipe name or path."));
        if (IFileManager::Get().FileSize(*Source) < 0 || IFileManager::Get().FileSize(*Source) > MaxCodeCharacters * 4 ||
            LexToString(FMD5Hash::HashFile(*Source)) != Hash)
            return Result(false, TEXT("Archived script changed or is missing; recipe was not saved."));
        if (IFileManager::Get().FileExists(*Destination))
            return Result(false, TEXT("Recipe already exists. Choose a new version name; existing recipes are not overwritten."));
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true) ||
            IFileManager::Get().Copy(*Destination, *Source, false) != COPY_OK)
            return Result(false, TEXT("Could not save the recipe."));
        auto Reply = Result(true, TEXT("Saved recipe: Scripts/CinderLink/") + Name + TEXT(".py"));
        Reply->SetStringField(TEXT("recipe_path"), TEXT("Scripts/CinderLink/") + Name + TEXT(".py"));
        return Reply;
    }

    TSharedPtr<FJsonValue> Spec(const FString& Name, const FString& Description,
        const TArray<TPair<FString, FString>>& Fields, bool bBackupPaths = false)
    {
        auto Properties = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Required;
        for (const auto& Field : Fields)
        {
            auto Schema = MakeShared<FJsonObject>();
            Schema->SetStringField(TEXT("type"), TEXT("string"));
            Schema->SetStringField(TEXT("description"), Field.Value);
            Properties->SetObjectField(Field.Key, Schema);
            Required.Add(MakeShared<FJsonValueString>(Field.Key));
        }
        if (bBackupPaths)
        {
            auto Schema = MakeShared<FJsonObject>();
            Schema->SetStringField(TEXT("type"), TEXT("array"));
            Schema->SetStringField(TEXT("description"), TEXT("/Game directories to snapshot BEFORE running (existing files, 1 GiB/10,000-file limit). Include every modified asset's parent folder and any World Partition external actor/object folders. [] only for inspection. This declaration does not restrict Python."));
            auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("type"), TEXT("string"));
            Schema->SetObjectField(TEXT("items"), Item);
            Properties->SetObjectField(TEXT("backup_paths"), Schema);
            Required.Add(MakeShared<FJsonValueString>(TEXT("backup_paths")));
        }
        auto Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));
        Schema->SetObjectField(TEXT("properties"), Properties);
        Schema->SetBoolField(TEXT("additionalProperties"), false);
        if (!Required.IsEmpty()) Schema->SetArrayField(TEXT("required"), Required);
        auto Tool = MakeShared<FJsonObject>();
        Tool->SetStringField(TEXT("type"), TEXT("function"));
        Tool->SetStringField(TEXT("name"), Name);
        Tool->SetStringField(TEXT("description"), Description);
        Tool->SetObjectField(TEXT("inputSchema"), Schema);
        return MakeShared<FJsonValueObject>(Tool);
    }
}

TArray<TSharedPtr<FJsonValue>> FCinderLinkPythonTools::BuildToolSpecs()
{
    return {
        Spec(TEXT("ue_python_status"), TEXT("Read Python availability and whether this turn has explicitly enabled Python authoring."), {}),
        Spec(TEXT("ue_python_execute"), TEXT("Run Python inside the open Unreal Editor, with Unreal's unrestricted host permissions, ONLY in explicitly enabled Python authoring mode. Also requires project edits and UE actions. Archive code and declared backups before execution; return bounded output, exceptions and disk changes. No hard timeout or automatic rollback: use short batches, no long loops or background callbacks. Never access unrelated files, credentials, network or hardware. Save intended assets explicitly. Private scope is namespace isolation, not a security sandbox."),
            {{TEXT("label"), TEXT("Short run label: letters, digits, underscores, hyphens (1-64).")},
             {TEXT("code"), TEXT("UE Python source, at most 131,072 characters. Use import unreal; print findings and changed asset paths. Archive is written before execution.")}}, true),
        Spec(TEXT("ue_python_get_run"), TEXT("Read the archived result of a Python run, including output, errors and backup metadata. No Python execution."),
            {{TEXT("run_id"), TEXT("Run id returned by ue_python_execute.")}}),
        Spec(TEXT("ue_python_save_recipe"), TEXT("Copy an archived script into Scripts/CinderLink/<name>.py for review and Git tracking. Requires Python authoring, project edits and UE actions. Refuses overwrite or a changed archive. Does not execute the recipe."),
            {{TEXT("run_id"), TEXT("Run id returned by ue_python_execute.")}, {TEXT("name"), TEXT("Recipe name, without extension: letters, digits, underscores, hyphens (1-64).")}})
    };
}

bool FCinderLinkPythonTools::IsKnownTool(const FString& Name)
{
    return Name == TEXT("ue_python_status") || Name == TEXT("ue_python_execute") ||
        Name == TEXT("ue_python_get_run") || Name == TEXT("ue_python_save_recipe");
}

bool FCinderLinkPythonTools::IsMutationTool(const FString& Name)
{
    return Name == TEXT("ue_python_execute") || Name == TEXT("ue_python_save_recipe");
}

TSharedRef<FJsonObject> FCinderLinkPythonTools::Execute(const FString& Name,
    const TSharedPtr<FJsonObject>& Arguments, const FString& ProjectRoot, bool bAuthoringEnabled, FString& OutSummary)
{
    TSharedRef<FJsonObject> Reply = Result(false, TEXT("Unknown Python tool."));
    if (!IsInGameThread()) Reply = Result(false, TEXT("Python tools require Unreal's game thread."));
    else if (!Arguments.IsValid()) Reply = Result(false, TEXT("Tool arguments must be an object."));
    else if (IsMutationTool(Name) && !bAuthoringEnabled)
        Reply = Result(false, TEXT("Python authoring is disabled. The user must enable it in the panel, with project edits and UE actions, before sending a turn."));
    else if (Name == TEXT("ue_python_status"))
    {
        auto Python = IPythonScriptPlugin::Get();
        Reply = Result(true, TEXT("Python executes with Unreal Editor permissions; the Codex filesystem sandbox does not apply to it."));
        Reply->SetBoolField(TEXT("authoring_enabled"), bAuthoringEnabled);
        Reply->SetBoolField(TEXT("python_ready"), Python && Python->IsPythonAvailable() && Python->IsPythonInitialized());
        Reply->SetBoolField(TEXT("pie_active_or_queued"), GEditor && (GEditor->PlayWorld || GEditor->IsPlaySessionRequestQueued()));
    }
    else if (Name == TEXT("ue_python_execute")) Reply = Run(Arguments, ProjectRoot);
    else if (Name == TEXT("ue_python_get_run")) Reply = ReadRun(Arguments, ProjectRoot);
    else if (Name == TEXT("ue_python_save_recipe")) Reply = SaveRecipe(Arguments, ProjectRoot);
    Reply->TryGetStringField(TEXT("message"), OutSummary);
    return Reply;
}
