// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "CinderLinkProcess.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Windows/WindowsHWrapper.h"

namespace
{
    HANDLE AsHandle(void* Value)
    {
        return static_cast<HANDLE>(Value);
    }

    void CloseNativeHandle(void*& Value)
    {
        if (Value != nullptr)
        {
            ::CloseHandle(AsHandle(Value));
            Value = nullptr;
        }
    }

    bool CreateParentReadPipe(void*& OutParentRead, void*& OutChildWrite, FString& OutError)
    {
        SECURITY_ATTRIBUTES SecurityAttributes{};
        SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        SecurityAttributes.bInheritHandle = 1;

        HANDLE ReadHandle = nullptr;
        HANDLE WriteHandle = nullptr;
        if (!::CreatePipe(&ReadHandle, &WriteHandle, &SecurityAttributes, 0))
        {
            OutError = FString::Printf(TEXT("CreatePipe failed (%lu)."), ::GetLastError());
            return false;
        }

        if (!::SetHandleInformation(ReadHandle, HANDLE_FLAG_INHERIT, 0))
        {
            ::CloseHandle(ReadHandle);
            ::CloseHandle(WriteHandle);
            OutError = FString::Printf(TEXT("SetHandleInformation failed (%lu)."), ::GetLastError());
            return false;
        }

        OutParentRead = ReadHandle;
        OutChildWrite = WriteHandle;
        return true;
    }

    bool CreateParentWritePipe(void*& OutParentWrite, void*& OutChildRead, FString& OutError)
    {
        SECURITY_ATTRIBUTES SecurityAttributes{};
        SecurityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        SecurityAttributes.bInheritHandle = 1;

        HANDLE ReadHandle = nullptr;
        HANDLE WriteHandle = nullptr;
        if (!::CreatePipe(&ReadHandle, &WriteHandle, &SecurityAttributes, 0))
        {
            OutError = FString::Printf(TEXT("CreatePipe failed (%lu)."), ::GetLastError());
            return false;
        }

        if (!::SetHandleInformation(WriteHandle, HANDLE_FLAG_INHERIT, 0))
        {
            ::CloseHandle(ReadHandle);
            ::CloseHandle(WriteHandle);
            OutError = FString::Printf(TEXT("SetHandleInformation failed (%lu)."), ::GetLastError());
            return false;
        }

        OutParentWrite = WriteHandle;
        OutChildRead = ReadHandle;
        return true;
    }

    TArray<TCHAR> BuildSanitizedEnvironment(const FString& SafeTempDirectory)
    {
        // Paths and basic OS information required by Codex and programs it may
        // start. Credential-bearing and provider-routing variables are omitted.
        static const TCHAR* AllowedNames[] = {
            TEXT("ALLUSERSPROFILE"),
            TEXT("APPDATA"),
            TEXT("CODEX_HOME"),
            TEXT("ComSpec"),
            TEXT("HOMEDRIVE"),
            TEXT("HOMEPATH"),
            TEXT("HOME"),
            TEXT("LOCALAPPDATA"),
            TEXT("NUMBER_OF_PROCESSORS"),
            TEXT("OS"),
            TEXT("Path"),
            TEXT("PATHEXT"),
            TEXT("PROCESSOR_ARCHITECTURE"),
            TEXT("PROCESSOR_IDENTIFIER"),
            TEXT("ProgramData"),
            TEXT("ProgramFiles"),
            TEXT("ProgramFiles(x86)"),
            TEXT("SystemDrive"),
            TEXT("SystemRoot"),
            TEXT("USERPROFILE"),
            TEXT("windir")
        };

        TArray<FString> Entries;
        Entries.Reserve(UE_ARRAY_COUNT(AllowedNames));
        for (const TCHAR* Name : AllowedNames)
        {
            const FString Value = FPlatformMisc::GetEnvironmentVariable(Name);
            if (!Value.IsEmpty())
            {
                Entries.Add(FString::Printf(TEXT("%s=%s"), Name, *Value));
            }
        }

        // Codex grants its command sandbox access to the directory named by
        // TEMP/TMP. Point both variables inside the selected project so the
        // user's ordinary temp directory does not become readable to tools.
        Entries.Add(TEXT("TEMP=") + SafeTempDirectory);
        Entries.Add(TEXT("TMP=") + SafeTempDirectory);

        Entries.Sort([](const FString& Left, const FString& Right)
        {
            return Left.Compare(Right, ESearchCase::IgnoreCase) < 0;
        });

        TArray<TCHAR> Block;
        for (const FString& Entry : Entries)
        {
            Block.Append(*Entry, Entry.Len());
            Block.Add(TEXT('\0'));
        }
        if (Entries.IsEmpty())
        {
            Block.Add(TEXT('\0'));
        }
        Block.Add(TEXT('\0'));
        return Block;
    }

    FString FindOnPath(const FString& FileName)
    {
        const FString PathValue = FPlatformMisc::GetEnvironmentVariable(TEXT("Path"));
        TArray<FString> Directories;
        PathValue.ParseIntoArray(Directories, TEXT(";"), true);

        for (FString Directory : Directories)
        {
            Directory.TrimStartAndEndInline();
            Directory.TrimQuotesInline();
            if (Directory.IsEmpty())
            {
                continue;
            }

            const FString Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(Directory, FileName));
            if (IFileManager::Get().FileExists(*Candidate))
            {
                return Candidate;
            }
        }
        return FString();
    }

    FString FindNativeBinaryFromNpmLauncher()
    {
        const FString PathValue = FPlatformMisc::GetEnvironmentVariable(TEXT("Path"));
        TArray<FString> Directories;
        PathValue.ParseIntoArray(Directories, TEXT(";"), true);

        for (FString Directory : Directories)
        {
            Directory.TrimStartAndEndInline();
            Directory.TrimQuotesInline();
            if (Directory.IsEmpty() ||
                !IFileManager::Get().FileExists(*FPaths::Combine(Directory, TEXT("codex.cmd"))))
            {
                continue;
            }

            const TCHAR* RelativeCandidates[] = {
                TEXT("node_modules/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/bin/codex.exe"),
                TEXT("node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/bin/codex.exe")
            };
            for (const TCHAR* RelativeCandidate : RelativeCandidates)
            {
                const FString Candidate = FPaths::ConvertRelativePathToFull(
                    FPaths::Combine(Directory, RelativeCandidate));
                if (IFileManager::Get().FileExists(*Candidate))
                {
                    return Candidate;
                }
            }
        }
        return FString();
    }
}

FCinderLinkProcess::~FCinderLinkProcess()
{
    Stop();
}

FString FCinderLinkProcess::ResolveCodexExecutable(FString& OutError)
{
    OutError.Reset();
    FString Resolved = FindOnPath(TEXT("codex.exe"));
    if (Resolved.IsEmpty())
    {
        Resolved = FindNativeBinaryFromNpmLauncher();
    }
    if (Resolved.IsEmpty())
    {
        const FString UserCandidate = FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPlatformProcess::UserDir(), TEXT("codex.exe")));
        if (IFileManager::Get().FileExists(*UserCandidate))
        {
            Resolved = UserCandidate;
        }
    }
    if (Resolved.IsEmpty())
    {
        OutError = TEXT("The native codex.exe was not found. Install the official Codex CLI and restart Unreal Editor.");
    }
    return Resolved;
}

bool FCinderLinkProcess::Start(const FString& ExecutablePath, const FString& WorkingDirectory, FString& OutError)
{
    Stop();
    OutError.Reset();

    const FString FullExecutable = FPaths::ConvertRelativePathToFull(ExecutablePath);
    const FString FullWorkingDirectory = FPaths::ConvertRelativePathToFull(WorkingDirectory);
    FString SafeTempDirectory = FPaths::Combine(FullWorkingDirectory, TEXT("Saved/CinderLink/Temp"));
    SafeTempDirectory = FPaths::ConvertRelativePathToFull(SafeTempDirectory);
    FPaths::NormalizeDirectoryName(SafeTempDirectory);
    if (!FPaths::IsRelative(ExecutablePath) &&
        IFileManager::Get().FileExists(*FullExecutable) &&
        FPaths::GetExtension(FullExecutable).Equals(TEXT("exe"), ESearchCase::IgnoreCase))
    {
        // Validated below by CreateProcess as well.
    }
    else
    {
        OutError = TEXT("The Codex executable must be an existing absolute .exe path.");
        return false;
    }

    if (!IFileManager::Get().DirectoryExists(*FullWorkingDirectory))
    {
        OutError = TEXT("The Unreal project directory does not exist.");
        return false;
    }
    if ((!IFileManager::Get().DirectoryExists(*SafeTempDirectory) &&
         !IFileManager::Get().MakeDirectory(*SafeTempDirectory, true)) ||
        !IFileManager::Get().DirectoryExists(*SafeTempDirectory))
    {
        OutError = TEXT("Could not create CinderLink's project-local temporary directory.");
        return false;
    }

    void* ChildStdOutWrite = nullptr;
    void* ChildStdErrWrite = nullptr;
    void* ChildStdInRead = nullptr;

    if (!CreateParentReadPipe(StdOutReadHandle, ChildStdOutWrite, OutError) ||
        !CreateParentReadPipe(StdErrReadHandle, ChildStdErrWrite, OutError) ||
        !CreateParentWritePipe(StdInWriteHandle, ChildStdInRead, OutError))
    {
        CloseNativeHandle(ChildStdOutWrite);
        CloseNativeHandle(ChildStdErrWrite);
        CloseNativeHandle(ChildStdInRead);
        CloseHandles();
        return false;
    }

    STARTUPINFOW StartupInfo{};
    StartupInfo.cb = sizeof(STARTUPINFOW);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    StartupInfo.hStdInput = AsHandle(ChildStdInRead);
    StartupInfo.hStdOutput = AsHandle(ChildStdOutWrite);
    StartupInfo.hStdError = AsHandle(ChildStdErrWrite);

    PROCESS_INFORMATION ProcessInformation{};
    // Every override is explicit and has CLI precedence over the user's local
    // configuration. Unknown or rejected settings terminate startup because
    // --strict-config is enabled.
    static const TCHAR* HardenedArguments =
        TEXT("app-server --strict-config ")
        TEXT("-c analytics.enabled=false ")
        TEXT("-c feedback.enabled=false ")
        TEXT("-c otel.exporter=none ")
        TEXT("-c otel.metrics_exporter=none ")
        TEXT("-c otel.trace_exporter=none ")
        TEXT("-c web_search=disabled ")
        TEXT("-c tools.web_search=false ")
        TEXT("-c history.persistence=none ")
        TEXT("-c shell_environment_policy.inherit=none ")
        TEXT("-c shell_environment_policy.ignore_default_excludes=false ")
        TEXT("-c windows.sandbox=elevated ")
        TEXT("-c default_permissions=cinderlink-project-read ")
        TEXT("-c permissions.cinderlink-project-read.filesystem={':workspace_roots'={'.'='read'}} ")
        TEXT("-c permissions.cinderlink-project-read.network.enabled=false ")
        TEXT("-c permissions.cinderlink-project-edit.filesystem={':workspace_roots'={'.'='write'}} ")
        TEXT("-c permissions.cinderlink-project-edit.network.enabled=false ")
        TEXT("--disable apps ")
        TEXT("--disable browser_use ")
        TEXT("--disable browser_use_external ")
        TEXT("--disable computer_use ")
        TEXT("--disable hooks ")
        TEXT("--disable image_generation ")
        TEXT("--disable in_app_browser ")
        TEXT("--disable multi_agent ")
        TEXT("--disable plugins ")
        TEXT("--disable remote_plugin ")
        TEXT("--disable skill_mcp_dependency_install ")
        TEXT("--disable skill_search ")
        TEXT("--disable tool_suggest ")
        TEXT("--disable workspace_dependencies");
    FString CommandLine = FString::Printf(TEXT("\"%s\" %s"), *FullExecutable, HardenedArguments);
    TArray<TCHAR> MutableCommandLine = CommandLine.GetCharArray();
    TArray<TCHAR> Environment = BuildSanitizedEnvironment(SafeTempDirectory);

    const DWORD CreationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    const BOOL bCreated = ::CreateProcessW(
        *FullExecutable,
        MutableCommandLine.GetData(),
        nullptr,
        nullptr,
        1,
        CreationFlags,
        Environment.GetData(),
        *FullWorkingDirectory,
        &StartupInfo,
        &ProcessInformation);

    CloseNativeHandle(ChildStdOutWrite);
    CloseNativeHandle(ChildStdErrWrite);
    CloseNativeHandle(ChildStdInRead);

    if (!bCreated)
    {
        OutError = FString::Printf(TEXT("CreateProcess failed (%lu)."), ::GetLastError());
        CloseHandles();
        return false;
    }

    ProcessHandle = ProcessInformation.hProcess;
    ProcessId = ProcessInformation.dwProcessId;
    JobHandle = ::CreateJobObjectW(nullptr, nullptr);

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION JobInformation{};
    JobInformation.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool bJobReady = JobHandle != nullptr &&
        ::SetInformationJobObject(
            AsHandle(JobHandle),
            JobObjectExtendedLimitInformation,
            &JobInformation,
            sizeof(JobInformation)) != 0 &&
        ::AssignProcessToJobObject(AsHandle(JobHandle), AsHandle(ProcessHandle)) != 0;

    if (!bJobReady)
    {
        const DWORD JobError = ::GetLastError();
        ::TerminateProcess(AsHandle(ProcessHandle), 1);
        ::CloseHandle(ProcessInformation.hThread);
        OutError = FString::Printf(TEXT("Could not contain the Codex process tree in a Windows Job (%lu)."), JobError);
        CloseHandles();
        return false;
    }

    if (::ResumeThread(ProcessInformation.hThread) == static_cast<DWORD>(-1))
    {
        const DWORD ResumeError = ::GetLastError();
        ::TerminateJobObject(AsHandle(JobHandle), 1);
        ::CloseHandle(ProcessInformation.hThread);
        OutError = FString::Printf(TEXT("Could not start the contained Codex process (%lu)."), ResumeError);
        CloseHandles();
        return false;
    }

    ::CloseHandle(ProcessInformation.hThread);
    StdOutBuffer.Reset();
    StdErrBuffer.Reset();
    return true;
}

void FCinderLinkProcess::Stop()
{
    if (JobHandle != nullptr)
    {
        ::TerminateJobObject(AsHandle(JobHandle), 0);
    }
    else if (ProcessHandle != nullptr && IsRunning())
    {
        ::TerminateProcess(AsHandle(ProcessHandle), 0);
    }

    if (ProcessHandle != nullptr)
    {
        ::WaitForSingleObject(AsHandle(ProcessHandle), 2000);
    }

    CloseHandles();
    StdOutBuffer.Reset();
    StdErrBuffer.Reset();
    ProcessId = 0;
}

bool FCinderLinkProcess::IsRunning() const
{
    return ProcessHandle != nullptr && ::WaitForSingleObject(AsHandle(ProcessHandle), 0) == WAIT_TIMEOUT;
}

bool FCinderLinkProcess::WriteJsonLine(const FString& Json, FString& OutError)
{
    OutError.Reset();
    if (StdInWriteHandle == nullptr || !IsRunning())
    {
        OutError = TEXT("Codex App Server is not running.");
        return false;
    }

    const FString Line = Json + TEXT("\n");
    FTCHARToUTF8 Utf8(*Line);
    const uint8* Cursor = reinterpret_cast<const uint8*>(Utf8.Get());
    int32 Remaining = Utf8.Length();

    while (Remaining > 0)
    {
        DWORD Written = 0;
        if (!::WriteFile(AsHandle(StdInWriteHandle), Cursor, Remaining, &Written, nullptr) || Written == 0)
        {
            OutError = FString::Printf(TEXT("Writing to Codex App Server failed (%lu)."), ::GetLastError());
            return false;
        }
        Cursor += Written;
        Remaining -= static_cast<int32>(Written);
    }
    return true;
}

void FCinderLinkProcess::PumpOutput(const FLineCallback& OnStdOutLine, const FLineCallback& OnStdErrLine)
{
    DrainPipe(StdOutReadHandle, StdOutBuffer, OnStdOutLine);
    DrainPipe(StdErrReadHandle, StdErrBuffer, OnStdErrLine);

    if (!IsRunning())
    {
        ExtractLines(StdOutBuffer, OnStdOutLine, true);
        ExtractLines(StdErrBuffer, OnStdErrLine, true);
    }
}

void FCinderLinkProcess::DrainPipe(void* ReadHandle, TArray<uint8>& Buffer, const FLineCallback& OnLine)
{
    if (ReadHandle == nullptr)
    {
        return;
    }

    for (;;)
    {
        DWORD Available = 0;
        if (!::PeekNamedPipe(AsHandle(ReadHandle), nullptr, 0, nullptr, &Available, nullptr) || Available == 0)
        {
            break;
        }

        const DWORD Requested = FMath::Min<DWORD>(Available, 64 * 1024);
        const int32 StartIndex = Buffer.Num();
        Buffer.AddUninitialized(static_cast<int32>(Requested));

        DWORD Read = 0;
        if (!::ReadFile(AsHandle(ReadHandle), Buffer.GetData() + StartIndex, Requested, &Read, nullptr))
        {
            Buffer.SetNum(StartIndex, EAllowShrinking::No);
            break;
        }
        Buffer.SetNum(StartIndex + static_cast<int32>(Read), EAllowShrinking::No);

        if (Buffer.Num() > MaximumBufferedBytes)
        {
            Buffer.Reset();
            OnLine(TEXT("[CinderLink blocked an oversized child-process output line.]"));
            break;
        }

        ExtractLines(Buffer, OnLine, false);
    }
}

void FCinderLinkProcess::ExtractLines(TArray<uint8>& Buffer, const FLineCallback& OnLine, bool bFlushRemainder)
{
    for (;;)
    {
        int32 LineEnd = INDEX_NONE;
        for (int32 Index = 0; Index < Buffer.Num(); ++Index)
        {
            if (Buffer[Index] == static_cast<uint8>('\n'))
            {
                LineEnd = Index;
                break;
            }
        }

        if (LineEnd == INDEX_NONE)
        {
            if (!bFlushRemainder || Buffer.IsEmpty())
            {
                return;
            }
            LineEnd = Buffer.Num();
        }

        int32 ContentLength = LineEnd;
        if (ContentLength > 0 && Buffer[ContentLength - 1] == static_cast<uint8>('\r'))
        {
            --ContentLength;
        }

        if (ContentLength > 0)
        {
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), ContentLength);
            OnLine(FString(Converted.Length(), Converted.Get()));
        }

        const int32 RemoveCount = LineEnd < Buffer.Num() ? LineEnd + 1 : LineEnd;
        Buffer.RemoveAt(0, RemoveCount, EAllowShrinking::No);
    }
}

void FCinderLinkProcess::CloseHandles()
{
    CloseNativeHandle(StdInWriteHandle);
    CloseNativeHandle(StdOutReadHandle);
    CloseNativeHandle(StdErrReadHandle);
    CloseNativeHandle(ProcessHandle);
    CloseNativeHandle(JobHandle);
}
