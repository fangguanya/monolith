#include "Indexers/ConfigIndexer.h"

#include "MonolithIndexLog.h"

namespace ConfigIndexerInternal
{
	/** 这份全局 artifact 在 identity 里对应的逻辑“包名”。 */
	static const FName GlobalPackageName(TEXT("/Monolith/Global/Config"));
}

bool FConfigIndexer::IndexGlobal(FMonolithIndexDatabase& DB)
{
	// Config reducer 现在只保留 artifact 主链这一种实现。
	return MonolithGlobalArtifactHelpers::ExecuteIndexGlobalFromArtifact(*this, DB);
}

bool FConfigIndexer::BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const
{
	return MonolithGlobalArtifactHelpers::BuildInputFileManifestIdentity(
		*this,
		ConfigIndexerInternal::GlobalPackageName,
		[](TArray<MonolithGlobalArtifactHelpers::FInputFile>& OutFiles)
		{
			CollectConfigFiles(OutFiles);
		},
		OutIdentity);
}

bool FConfigIndexer::BuildGlobalArtifact(FMonolithArtifact& OutArtifact)
{
	return MonolithGlobalArtifactHelpers::BuildGlobalPayloadArtifact<MonolithSimpleArtifactSerialization::FConfigPayload>(
		*this,
		ConfigIndexerInternal::GlobalPackageName,
		[](MonolithSimpleArtifactSerialization::FConfigPayload& Payload)
		{
			return BuildPayload(Payload);
		},
		[](const MonolithSimpleArtifactSerialization::FConfigPayload& Payload, TArray<uint8>& OutBytes)
		{
			MonolithSimpleArtifactSerialization::SerializeConfigPayload(Payload, OutBytes);
		},
		[](const MonolithSimpleArtifactSerialization::FConfigPayload& Payload)
		{
			return Payload.Entries.Num();
		},
		OutArtifact);
}

bool FConfigIndexer::MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB)
{
	return MonolithGlobalArtifactHelpers::MaterializeGlobalPayloadArtifact<MonolithSimpleArtifactSerialization::FConfigPayload>(
		Artifact,
		DB,
		[](const TArray<uint8>& Bytes, MonolithSimpleArtifactSerialization::FConfigPayload& Payload)
		{
			return MonolithSimpleArtifactSerialization::DeserializeConfigPayload(Bytes, Payload);
		},
		[](const MonolithSimpleArtifactSerialization::FConfigPayload& Payload, FMonolithIndexDatabase& InDB)
		{
			return MonolithSimpleArtifactSerialization::MaterializeConfigPayload(Payload, InDB);
		},
		[](const MonolithSimpleArtifactSerialization::FConfigPayload& Payload)
		{
			return Payload.Entries.Num();
		},
		TEXT("ConfigIndexer"),
		TEXT("config entries"));
}

void FConfigIndexer::CollectConfigFiles(TArray<MonolithGlobalArtifactHelpers::FInputFile>& OutFiles)
{
	OutFiles.Reset();
	MonolithGlobalArtifactHelpers::AppendFilesRecursive(OutFiles, FPaths::ProjectConfigDir(), TEXT("*.ini"));
	MonolithGlobalArtifactHelpers::AppendFilesRecursive(OutFiles, FPaths::EngineConfigDir(), TEXT("*.ini"));
	MonolithGlobalArtifactHelpers::SortAndUniqueInputFiles(OutFiles);
}

bool FConfigIndexer::BuildPayload(MonolithSimpleArtifactSerialization::FConfigPayload& OutPayload)
{
	OutPayload = MonolithSimpleArtifactSerialization::FConfigPayload();

	TArray<MonolithGlobalArtifactHelpers::FInputFile> InputFiles;
	CollectConfigFiles(InputFiles);

	for (const MonolithGlobalArtifactHelpers::FInputFile& InputFile : InputFiles)
	{
		if (!ParseIniFileToPayload(InputFile, OutPayload))
		{
			return false;
		}
	}

	OutPayload.Entries.Sort([](
		const MonolithSimpleArtifactSerialization::FConfigPayloadEntry& A,
		const MonolithSimpleArtifactSerialization::FConfigPayloadEntry& B)
	{
		if (A.FilePath != B.FilePath)
		{
			return A.FilePath < B.FilePath;
		}
		if (A.Section != B.Section)
		{
			return A.Section < B.Section;
		}
		return A.Key == B.Key ? A.Value < B.Value : A.Key < B.Key;
	});

	UE_LOG(
		LogMonolithIndex,
		Log,
		TEXT("ConfigIndexer: prepared %d config entries from %d .ini files"),
		OutPayload.Entries.Num(),
		InputFiles.Num());
	return true;
}

bool FConfigIndexer::ParseIniFileToPayload(
	const MonolithGlobalArtifactHelpers::FInputFile& InputFile,
	MonolithSimpleArtifactSerialization::FConfigPayload& OutPayload)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *InputFile.AbsolutePath))
	{
		return false;
	}

	FString CurrentSection;
	for (const FString& RawLine : Lines)
	{
		const FString Line = RawLine.TrimStartAndEnd();

		// 空行和注释不应该进入索引，否则查询结果会被噪声污染。
		if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#")))
		{
			continue;
		}

		if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
		{
			CurrentSection = Line.Mid(1, Line.Len() - 2);
			continue;
		}

		int32 EqualsIndex = INDEX_NONE;
		if (!Line.FindChar(TEXT('='), EqualsIndex) || CurrentSection.IsEmpty())
		{
			continue;
		}

		MonolithSimpleArtifactSerialization::FConfigPayloadEntry Entry;
		Entry.FilePath = InputFile.LogicalPath;
		Entry.Section = CurrentSection;
		Entry.Key = Line.Left(EqualsIndex).TrimStartAndEnd();
		Entry.Value = Line.Mid(EqualsIndex + 1).TrimStartAndEnd();
		if (Entry.Key.IsEmpty())
		{
			continue;
		}

		OutPayload.Entries.Add(MoveTemp(Entry));
	}

	return true;
}
