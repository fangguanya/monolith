#include "Indexers/GameplayTagIndexer.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "GameplayTagContainer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"

/*
 * GameplayTagIndexer 也统一收口到“单一 payload 真相”：
 * - 先从 Asset Registry 元数据里抽出 tag 引用列表；
 * - 生产表 / artifact / shadow 都围着这份列表转。
 *
 * 这样以后如果团队想改 tag 提取规则，只需要改 BuildPayload 一处。
 */

namespace GameplayTagIndexerInternal
{
	static const FName OwnedGameplayTagsKey(TEXT("OwnedGameplayTags"));
	static const FName GameplayTagsKey(TEXT("GameplayTags"));

	/** 把提取出的 tag + context 追加进 payload。 */
	static void AppendTagReference(
		const FString& TagName,
		const FString& Context,
		MonolithSimpleArtifactSerialization::FTagReferencePayload& OutPayload)
	{
		if (TagName.IsEmpty())
		{
			return;
		}

		MonolithSimpleArtifactSerialization::FTagReferencePayloadEntry Entry;
		Entry.TagName = TagName;
		Entry.Context = Context;
		OutPayload.References.Add(MoveTemp(Entry));
	}

	/** 从 `TagName="Foo.Bar"` 这种结构体导出文本里把 tag 名抠出来。 */
	static void AppendTagReferencesFromExportText(
		const FString& Value,
		const FString& Context,
		MonolithSimpleArtifactSerialization::FTagReferencePayload& OutPayload)
	{
		FString Remaining = Value;
		const FString TagToken = TEXT("TagName=\"");
		int32 TokenPosition = INDEX_NONE;
		while ((TokenPosition = Remaining.Find(TagToken, ESearchCase::CaseSensitive)) != INDEX_NONE)
		{
			Remaining.RightChopInline(TokenPosition + TagToken.Len());
			const int32 EndQuote = Remaining.Find(TEXT("\""), ESearchCase::CaseSensitive);
			if (EndQuote == INDEX_NONE)
			{
				break;
			}

			AppendTagReference(Remaining.Left(EndQuote), Context, OutPayload);
			Remaining.RightChopInline(EndQuote + 1);
		}
	}

	/** 从逗号分隔的简单列表里提取 tag 名。 */
	static void AppendTagReferencesFromSimpleList(
		const FString& Value,
		const FString& Context,
		MonolithSimpleArtifactSerialization::FTagReferencePayload& OutPayload)
	{
		TArray<FString> ParsedTags;
		Value.ParseIntoArray(ParsedTags, TEXT(","));
		for (FString& TagName : ParsedTags)
		{
			TagName.TrimStartAndEndInline();
			AppendTagReference(TagName, Context, OutPayload);
		}
	}

	/** 对单个元数据值选择合适的解析方式。 */
	static void AppendTagReferencesFromValue(
		const FString& Value,
		const FString& Context,
		MonolithSimpleArtifactSerialization::FTagReferencePayload& OutPayload)
	{
		if (Value.IsEmpty())
		{
			return;
		}

		if (Value.Contains(TEXT("TagName=\"")))
		{
			AppendTagReferencesFromExportText(Value, Context, OutPayload);
			return;
		}

		AppendTagReferencesFromSimpleList(Value, Context, OutPayload);
	}

	/** 只从 AssetData 里提取“这份资产当前引用了哪些 tag”。 */
	static bool BuildPayload(
		const FAssetData& AssetData,
		MonolithSimpleArtifactSerialization::FTagReferencePayload& OutPayload)
	{
		OutPayload = MonolithSimpleArtifactSerialization::FTagReferencePayload();
		if (AssetData.PackageName.IsNone())
		{
			return false;
		}

		// 先走最常见的两个键，既容易读，也能避免后面的 EnumerateTags 重复处理。
		for (const FName Key : { OwnedGameplayTagsKey, GameplayTagsKey })
		{
			FString TagValueString;
			if (AssetData.GetTagValue(Key, TagValueString))
			{
				AppendTagReferencesFromValue(TagValueString, Key.ToString(), OutPayload);
			}
		}

		AssetData.EnumerateTags([&OutPayload](const TPair<FName, FAssetTagValueRef>& TagPair)
		{
			if (TagPair.Key == OwnedGameplayTagsKey || TagPair.Key == GameplayTagsKey)
			{
				return;
			}

			const FString Value = TagPair.Value.GetValue();
			if (!Value.Contains(TEXT("TagName=\"")))
			{
				return;
			}

			AppendTagReferencesFromExportText(Value, TagPair.Key.ToString(), OutPayload);
		});

		OutPayload.References.Sort([](
			const MonolithSimpleArtifactSerialization::FTagReferencePayloadEntry& A,
			const MonolithSimpleArtifactSerialization::FTagReferencePayloadEntry& B)
		{
			if (A.TagName != B.TagName)
			{
				return A.TagName < B.TagName;
			}

			return A.Context < B.Context;
		});

		return true;
	}
}

bool FGameplayTagIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)LoadedAsset;
	(void)AssetRegistry;

	MonolithSimpleArtifactSerialization::FTagReferencePayload Payload;
	if (!GameplayTagIndexerInternal::BuildPayload(AssetData, Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MonolithSimpleArtifactSerialization::SerializeTagReferencePayload(Payload, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FGameplayTagIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FTagReferencePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeTagReferencePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeTagReferencePayload(Payload, DB, AssetId);
}

bool FGameplayTagIndexer::MaterializeArtifactToShadow(
	const FMonolithArtifact& Artifact,
	FMonolithIndexDatabase& DB,
	int64 AssetId,
	const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FTagReferencePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeTagReferencePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeTagReferencePayloadToShadow(Payload, DB, AssetId, CohortName);
}
