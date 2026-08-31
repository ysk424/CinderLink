// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "CoreMinimal.h"

/**
 * Owns one Codex App Server process and its private anonymous pipes.
 *
 * The Windows implementation builds a fresh environment block from a fixed
 * allowlist. It deliberately does not inherit API keys or unrelated secrets.
 */
class FCinderLinkProcess
{
public:
    using FLineCallback = TFunction<void(const FString&)>;

    FCinderLinkProcess() = default;
    ~FCinderLinkProcess();

    FCinderLinkProcess(const FCinderLinkProcess&) = delete;
    FCinderLinkProcess& operator=(const FCinderLinkProcess&) = delete;

    static FString ResolveCodexExecutable(FString& OutError);

    bool Start(const FString& ExecutablePath, const FString& WorkingDirectory, FString& OutError);
    void Stop();
    bool IsRunning() const;
    bool WriteJsonLine(const FString& Json, FString& OutError);
    void PumpOutput(const FLineCallback& OnStdOutLine, const FLineCallback& OnStdErrLine);

private:
    static constexpr int32 MaximumBufferedBytes = 4 * 1024 * 1024;

    void DrainPipe(void* ReadHandle, TArray<uint8>& Buffer, const FLineCallback& OnLine);
    void ExtractLines(TArray<uint8>& Buffer, const FLineCallback& OnLine, bool bFlushRemainder);
    void CloseHandles();

    void* ProcessHandle = nullptr;
    void* JobHandle = nullptr;
    void* StdInWriteHandle = nullptr;
    void* StdOutReadHandle = nullptr;
    void* StdErrReadHandle = nullptr;
    uint32 ProcessId = 0;

    TArray<uint8> StdOutBuffer;
    TArray<uint8> StdErrBuffer;
};
