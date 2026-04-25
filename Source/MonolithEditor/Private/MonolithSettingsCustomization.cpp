#include "MonolithSettingsCustomization.h"
#include "MonolithSettings.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithSourceSubsystem.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "OutputLogModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MonolithSettingsCustomization"

TSharedRef<IDetailCustomization> FMonolithSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FMonolithSettingsCustomization);
}

void FMonolithSettingsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	IDetailCategoryBuilder& McpCat = DetailBuilder.EditCategory("MCP Server");

	McpCat.AddCustomRow(LOCTEXT("McpThreadingRow", "MCP Threading"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("McpThreadingLabel", "MCP Threading"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(420.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("McpThreadingValue", "Read-only project index queries now run on background threads. Asset edits and other editor-mutating actions stay on the game thread."))
			.AutoWrapText(true)
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];

	McpCat.AddCustomRow(LOCTEXT("McpLogRow", "MCP Logs"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("McpLogLabel", "MCP Logs"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SButton)
			.Text(LOCTEXT("OpenMcpLogBtn", "Open MCP Log"))
			.ToolTipText(LOCTEXT("OpenMcpLogTooltip", "Open Output Log and focus Monolith MCP execution logs."))
			.OnClicked_Lambda([]()
			{
				FOutputLogModule& OutputLogModule = FOutputLogModule::Get();
				OutputLogModule.UpdateOutputLogFilter(
					{ FName(TEXT("LogMonolith")), FName(TEXT("LogMonolithIndex")) },
					true,
					true,
					true);
				OutputLogModule.FocusOutputLogAndScrollToEnd();
				return FReply::Handled();
			})
		];

	IDetailCategoryBuilder& IndexCat = DetailBuilder.EditCategory("Indexing");

	// Re-Index Project button
	IndexCat.AddCustomRow(LOCTEXT("ReindexProjectRow", "Re-Index Project"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ProjectIndexLabel", "Project Index"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SButton)
			.Text(LOCTEXT("ReindexProjectBtn", "Re-Index Now"))
			.IsEnabled_Lambda([]()
			{
				if (GEditor)
				{
					if (auto* Sub = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
					{
						return !Sub->IsIndexing();
					}
				}
				return false;
			})
			.OnClicked_Lambda([]()
			{
				if (GEditor)
				{
					GEditor->Exec(nullptr, TEXT("Monolith.StartIndex full"));
				}
				return FReply::Handled();
			})
		];

	// Re-Index Engine Source button
	IndexCat.AddCustomRow(LOCTEXT("ReindexEngineRow", "Re-Index Engine Source"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("EngineSourceLabel", "Engine Source"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		[
			SNew(SButton)
			.Text(LOCTEXT("ReindexEngineBtn", "Re-Index Now"))
			.IsEnabled_Lambda([]()
			{
				if (GEditor)
				{
					if (auto* Sub = GEditor->GetEditorSubsystem<UMonolithSourceSubsystem>())
					{
						return !Sub->IsIndexing();
					}
				}
				return false;
			})
			.OnClicked_Lambda([]()
			{
				if (GEditor)
				{
					if (auto* Sub = GEditor->GetEditorSubsystem<UMonolithSourceSubsystem>())
					{
						Sub->TriggerReindex();
					}
				}
				return FReply::Handled();
			})
		];
}

#undef LOCTEXT_NAMESPACE
