// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "CoreMinimal.h"
#include "CinderLinkProtocol.h"
#include "Widgets/SCompoundWidget.h"

class SCheckBox;
class SMultiLineEditableTextBox;
class STextBlock;
enum class ECheckBoxState : uint8;

class SCinderLinkPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCinderLinkPanel) : _AutoConnect(true) {}
        SLATE_ARGUMENT(bool, AutoConnect)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCinderLinkPanel() override;

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FCinderLinkPythonPanelPolicyTest;
    friend class FCinderLinkSteeringTest;
#endif
    FReply OnConnectClicked();
    FReply OnNewThreadClicked();
    FReply OnSendClicked();
    FReply OnInterruptClicked();
    void OnPythonAuthoringChanged(ECheckBoxState State);
    void OnEditPermissionChanged(ECheckBoxState State);
    void ResetPythonAuthoring();
    FText GetModeText() const;

    void HandleMessage(const FCinderLinkMessage& Message);
    void RefreshExecutablePath();
    void AppendTranscript(const FString& Text);
    void SetStatus(const FString& Text, bool bError = false);

    FText GetConnectButtonText() const;
    FText GetStatusText() const;
    FText GetActivityText() const;
    FText GetSendButtonText() const;
    FText GetModelText() const;
    FText GetExecutableText() const;
    FText GetProjectText() const;
    bool CanSend() const;
    bool CanStartNewThread() const;
    bool CanInterrupt() const;

    TUniquePtr<FCinderLinkAppServerClient> Client;
    TSharedPtr<SMultiLineEditableTextBox> TranscriptBox;
    TSharedPtr<SMultiLineEditableTextBox> InputBox;
    TSharedPtr<SCheckBox> AllowEditsCheckBox;
    TSharedPtr<SCheckBox> AllowEditorActionsCheckBox;
    TSharedPtr<SCheckBox> PythonAuthoringCheckBox;

    FString ExecutablePath;
    FString ExecutableError;
    FString ProjectRoot;
    FString Transcript;
    FString StatusText = TEXT("Disconnected");
    int32 StreamingStartIndex = INDEX_NONE;
    int32 StreamingTextLength = 0;
    bool bStatusError = false;
};
