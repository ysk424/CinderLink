// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 ysk424 and CinderLink contributors

#include "SCinderLinkPanel.h"

#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SCinderLinkPanel"

namespace
{
    constexpr int32 MaximumTranscriptCharacters = 1024 * 1024;
}

void SCinderLinkPanel::Construct(const FArguments& InArgs)
{
    ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    FPaths::NormalizeDirectoryName(ProjectRoot);
    RefreshExecutablePath();

    Client = MakeUnique<FCinderLinkAppServerClient>();
    Client->OnMessage.AddRaw(this, &SCinderLinkPanel::HandleMessage);

    ChildSlot
    [
        SNew(SBorder)
        .Padding(10.0f)
        .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
        [
            SNew(SVerticalBox)

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("Title", "CinderLink"))
                .TextStyle(FAppStyle::Get(), TEXT("HeadingExtraSmall"))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(SBorder)
                .Padding(8.0f)
                .BorderImage(FAppStyle::GetBrush(TEXT("DetailsView.CategoryTop")))
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text(LOCTEXT(
                        "SecurityBanner",
                        "Project-only filesystem · built-in UE tools only · no external tools · no escalation"))
                ]
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 1.0f)
            [
                SNew(STextBlock)
                .Text(this, &SCinderLinkPanel::GetExecutableText)
                .ToolTipText(LOCTEXT("ExecutableTooltip", "This exact executable will be launched when you connect."))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 1.0f, 0.0f, 7.0f)
            [
                SNew(STextBlock)
                .Text(this, &SCinderLinkPanel::GetProjectText)
                .ToolTipText(LOCTEXT("ProjectTooltip", "CinderLink requests this directory as the only project root."))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(this, &SCinderLinkPanel::GetConnectButtonText)
                    .OnClicked(this, &SCinderLinkPanel::OnConnectClicked)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("NewThread", "New thread"))
                    .IsEnabled(this, &SCinderLinkPanel::CanStartNewThread)
                    .OnClicked(this, &SCinderLinkPanel::OnNewThreadClicked)
                ]
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(this, &SCinderLinkPanel::GetStatusText)
                    .ColorAndOpacity_Lambda([this]()
                    {
                        return bStatusError
                            ? FSlateColor(FLinearColor(1.0f, 0.25f, 0.2f))
                            : FSlateColor::UseForeground();
                    })
                ]
            ]

            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            .Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SAssignNew(TranscriptBox, SMultiLineEditableTextBox)
                .IsReadOnly(true)
                .AllowContextMenu(true)
                .AutoWrapText(true)
                .HintText(LOCTEXT("TranscriptHint", "Conversation and selected local activity appear here."))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SAssignNew(InputBox, SMultiLineEditableTextBox)
                .AutoWrapText(true)
                .HintText(LOCTEXT("InputHint", "Ask Codex to inspect or change this Unreal project..."))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 7.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SAssignNew(AllowEditsCheckBox, SCheckBox)
                        .IsChecked(ECheckBoxState::Checked)
                        .ToolTipText(LOCTEXT(
                            "AllowEditsTooltip",
                            "Enabled by default. Turns may write only inside the current project root; clear this for read-only project access."))
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("AllowEdits", "Allow project file edits"))
                        ]
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 3.0f, 0.0f, 0.0f)
                    [
                        SAssignNew(AllowEditorActionsCheckBox, SCheckBox)
                        .IsChecked(ECheckBoxState::Checked)
                        .ToolTipText(LOCTEXT(
                            "AllowEditorActionsTooltip",
                            "Enabled by default. Turns may call CinderLink's allowlisted Unreal Editor actions; clear this for read-only Editor access. PIE start and image sending still require visible confirmation."))
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("AllowEditorActions", "Allow UE Editor actions"))
                        ]
                    ]
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("StopTurn", "Stop turn"))
                    .IsEnabled(this, &SCinderLinkPanel::CanInterrupt)
                    .OnClicked(this, &SCinderLinkPanel::OnInterruptClicked)
                ]
                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Send", "Send"))
                    .IsEnabled(this, &SCinderLinkPanel::CanSend)
                    .OnClicked(this, &SCinderLinkPanel::OnSendClicked)
                ]
            ]
        ]
    ];

    if (!ExecutableError.IsEmpty())
    {
        SetStatus(ExecutableError, true);
    }
    else
    {
        OnConnectClicked();
    }
}

SCinderLinkPanel::~SCinderLinkPanel()
{
    if (Client)
    {
        Client->OnMessage.RemoveAll(this);
        Client->Disconnect();
    }
}

FReply SCinderLinkPanel::OnConnectClicked()
{
    if (Client->IsProcessRunning())
    {
        Client->Disconnect();
        StreamingStartIndex = INDEX_NONE;
        SetStatus(TEXT("Disconnected"));
        AppendTranscript(TEXT("\n[CinderLink disconnected.]\n"));
        return FReply::Handled();
    }

    RefreshExecutablePath();
    if (ExecutablePath.IsEmpty())
    {
        SetStatus(ExecutableError, true);
        return FReply::Handled();
    }

    FString Error;
    if (!Client->Connect(ExecutablePath, ProjectRoot, Error))
    {
        SetStatus(Error, true);
    }
    else
    {
        SetStatus(TEXT("Starting App Server..."));
        AppendTranscript(TEXT("[Connected using local stdio. No CinderLink listener was opened.]\n"));
    }
    return FReply::Handled();
}

FReply SCinderLinkPanel::OnNewThreadClicked()
{
    FString Error;
    if (!Client->StartNewThread(Error))
    {
        SetStatus(Error, true);
    }
    else
    {
        StreamingStartIndex = INDEX_NONE;
        AppendTranscript(TEXT("\n[Starting a new thread...]\n"));
    }
    return FReply::Handled();
}

FReply SCinderLinkPanel::OnSendClicked()
{
    const FString Text = InputBox.IsValid() ? InputBox->GetText().ToString() : FString();
    const bool bAllowEdits = AllowEditsCheckBox.IsValid() && AllowEditsCheckBox->IsChecked();
    const bool bAllowEditorActions =
        AllowEditorActionsCheckBox.IsValid() && AllowEditorActionsCheckBox->IsChecked();

    FString Error;
    if (!Client->SendTurn(Text, bAllowEdits, bAllowEditorActions, Error))
    {
        SetStatus(Error, true);
        return FReply::Handled();
    }

    FString CleanText = Text;
    CleanText.TrimStartAndEndInline();
    AppendTranscript(TEXT("\nYou: ") + CleanText + TEXT("\n"));
    InputBox->SetText(FText::GetEmpty());
    StreamingStartIndex = INDEX_NONE;
    return FReply::Handled();
}

FReply SCinderLinkPanel::OnInterruptClicked()
{
    FString Error;
    if (!Client->InterruptTurn(Error))
    {
        SetStatus(Error, true);
    }
    return FReply::Handled();
}

void SCinderLinkPanel::HandleMessage(const FCinderLinkMessage& Message)
{
    switch (Message.Kind)
    {
    case ECinderLinkMessageKind::Status:
        SetStatus(Message.Text);
        break;

    case ECinderLinkMessageKind::AssistantDelta:
        if (StreamingStartIndex == INDEX_NONE)
        {
            StreamingStartIndex = Transcript.Len();
            AppendTranscript(TEXT("Assistant: "));
        }
        AppendTranscript(Message.Text);
        break;

    case ECinderLinkMessageKind::AssistantFinal:
        if (StreamingStartIndex != INDEX_NONE && StreamingStartIndex <= Transcript.Len())
        {
            Transcript.LeftInline(StreamingStartIndex, EAllowShrinking::No);
        }
        AppendTranscript(TEXT("Assistant: ") + Message.Text + TEXT("\n"));
        StreamingStartIndex = INDEX_NONE;
        break;

    case ECinderLinkMessageKind::Command:
        AppendTranscript(TEXT("\n[Local command] ") + Message.Text + TEXT("\n"));
        break;

    case ECinderLinkMessageKind::FileChange:
        AppendTranscript(TEXT("\n[Project files] ") + Message.Text + TEXT("\n"));
        break;

    case ECinderLinkMessageKind::EditorAction:
        AppendTranscript(TEXT("\n[UE Editor] ") + Message.Text + TEXT("\n"));
        break;

    case ECinderLinkMessageKind::Warning:
        SetStatus(Message.Text);
        AppendTranscript(TEXT("\n[Warning] ") + Message.Text + TEXT("\n"));
        break;

    case ECinderLinkMessageKind::Error:
        SetStatus(Message.Text, true);
        AppendTranscript(TEXT("\n[Error] ") + Message.Text + TEXT("\n"));
        StreamingStartIndex = INDEX_NONE;
        break;

    case ECinderLinkMessageKind::TurnCompleted:
        AppendTranscript(TEXT("\n[Turn ") + Message.Text + TEXT("]\n"));
        StreamingStartIndex = INDEX_NONE;
        SetStatus(TEXT("Ready"));
        break;
    }
}

void SCinderLinkPanel::RefreshExecutablePath()
{
    ExecutablePath = FCinderLinkProcess::ResolveCodexExecutable(ExecutableError);
}

void SCinderLinkPanel::AppendTranscript(const FString& Text)
{
    Transcript += Text;
    if (Transcript.Len() > MaximumTranscriptCharacters)
    {
        const int32 RemoveCount = Transcript.Len() - MaximumTranscriptCharacters;
        Transcript.RightChopInline(RemoveCount, EAllowShrinking::No);
        Transcript = TEXT("[Earlier transcript removed from memory.]\n") + Transcript;
        StreamingStartIndex = INDEX_NONE;
    }
    if (TranscriptBox.IsValid())
    {
        TranscriptBox->SetText(FText::FromString(Transcript));
    }
}

void SCinderLinkPanel::SetStatus(const FString& Text, bool bError)
{
    StatusText = Text;
    bStatusError = bError;
}

FText SCinderLinkPanel::GetConnectButtonText() const
{
    return Client && Client->IsProcessRunning()
        ? LOCTEXT("Disconnect", "Disconnect")
        : LOCTEXT("Connect", "Connect");
}

FText SCinderLinkPanel::GetStatusText() const
{
    return FText::FromString(StatusText);
}

FText SCinderLinkPanel::GetExecutableText() const
{
    return FText::FromString(
        ExecutablePath.IsEmpty()
            ? TEXT("Codex executable: not found")
            : TEXT("Codex executable: ") + ExecutablePath);
}

FText SCinderLinkPanel::GetProjectText() const
{
    return FText::FromString(TEXT("Project boundary: ") + ProjectRoot);
}

bool SCinderLinkPanel::CanSend() const
{
    return Client && Client->IsReady() && !Client->IsTurnInProgress();
}

bool SCinderLinkPanel::CanStartNewThread() const
{
    return Client && Client->IsReady() && !Client->IsTurnInProgress();
}

bool SCinderLinkPanel::CanInterrupt() const
{
    return Client && Client->IsReady() && Client->IsTurnInProgress();
}

#undef LOCTEXT_NAMESPACE
