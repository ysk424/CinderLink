// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#pragma once

#include "CoreMinimal.h"
#include "CinderLinkProtocol.h"
#include "Widgets/SCompoundWidget.h"

class SCheckBox;
class SMultiLineEditableTextBox;
class STextBlock;

class SCinderLinkPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCinderLinkPanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SCinderLinkPanel() override;

private:
    FReply OnConnectClicked();
    FReply OnNewThreadClicked();
    FReply OnSendClicked();
    FReply OnInterruptClicked();

    void HandleMessage(const FCinderLinkMessage& Message);
    void RefreshExecutablePath();
    void AppendTranscript(const FString& Text);
    void SetStatus(const FString& Text, bool bError = false);

    FText GetConnectButtonText() const;
    FText GetStatusText() const;
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

    FString ExecutablePath;
    FString ExecutableError;
    FString ProjectRoot;
    FString Transcript;
    FString StatusText = TEXT("Disconnected");
    int32 StreamingStartIndex = INDEX_NONE;
    bool bStatusError = false;
};
