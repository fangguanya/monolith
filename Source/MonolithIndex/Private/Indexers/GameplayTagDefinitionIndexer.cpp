#include "Indexers/GameplayTagDefinitionIndexer.h"

#include "GameplayTagsManager.h"
#include "Indexers/MonolithGlobalArtifactHelpers.h"

namespace GameplayTagDefinitionIndexerInternal
{
	/** 这份全局 artifact 在 identity 里对应的逻辑“包名”。 */
	static const FName GlobalPackageName(TEXT("/Monolith/Global/GameplayTagDefinitions"));
}

bool FGameplayTagDefinitionIndexer::IndexGlobal(FMonolithIndexDatabase& DB)
{
	// GameplayTag 定义树现在只保留 artifact 主链这一种实现。
	return MonolithGlobalArtifactHelpers::ExecuteIndexGlobalFromArtifact(*this, DB);
}

bool FGameplayTagDefinitionIndexer::BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const
{
	return MonolithGlobalArtifactHelpers::BuildPayloadFingerprintIdentity<MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload>(
		*this,
		GameplayTagDefinitionIndexerInternal::GlobalPackageName,
		[](MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload)
		{
			return BuildPayload(Payload);
		},
		[](const MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload, TArray<uint8>& OutBytes)
		{
			MonolithSimpleArtifactSerialization::SerializeGameplayTagDefinitionPayload(Payload, OutBytes);
		},
		OutIdentity);
}

bool FGameplayTagDefinitionIndexer::BuildGlobalArtifact(FMonolithArtifact& OutArtifact)
{
	return MonolithGlobalArtifactHelpers::BuildGlobalPayloadArtifact<MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload>(
		*this,
		GameplayTagDefinitionIndexerInternal::GlobalPackageName,
		[](MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload)
		{
			return BuildPayload(Payload);
		},
		[](const MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload, TArray<uint8>& OutBytes)
		{
			MonolithSimpleArtifactSerialization::SerializeGameplayTagDefinitionPayload(Payload, OutBytes);
		},
		[](const MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload)
		{
			return Payload.Definitions.Num();
		},
		OutArtifact);
}

bool FGameplayTagDefinitionIndexer::MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB)
{
	return MonolithGlobalArtifactHelpers::MaterializeGlobalPayloadArtifact<MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload>(
		Artifact,
		DB,
		[](const TArray<uint8>& Bytes, MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload)
		{
			return MonolithSimpleArtifactSerialization::DeserializeGameplayTagDefinitionPayload(Bytes, Payload);
		},
		[](const MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload, FMonolithIndexDatabase& InDB)
		{
			return MonolithSimpleArtifactSerialization::MaterializeGameplayTagDefinitionPayload(Payload, InDB);
		},
		[](const MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& Payload)
		{
			return Payload.Definitions.Num();
		},
		TEXT("GameplayTagDefinitionIndexer"),
		TEXT("tag definitions"));
}

bool FGameplayTagDefinitionIndexer::BuildPayload(MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& OutPayload)
{
	OutPayload = MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload();

	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
	TArray<TSharedPtr<FGameplayTagNode>> RootNodes;
	TagManager.GetFilteredGameplayRootTags(FString(), RootNodes);
	for (const TSharedPtr<FGameplayTagNode>& RootNode : RootNodes)
	{
		if (RootNode.IsValid())
		{
			AppendNodeDefinitions(*RootNode, OutPayload);
		}
	}

	OutPayload.Definitions.Sort([](
		const MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayloadEntry& A,
		const MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayloadEntry& B)
	{
		return A.TagName == B.TagName ? A.ParentTag < B.ParentTag : A.TagName < B.TagName;
	});
	return true;
}

void FGameplayTagDefinitionIndexer::AppendNodeDefinitions(
	const FGameplayTagNode& Node,
	MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayload& OutPayload)
{
	const FString TagName = Node.GetCompleteTagString();
	if (TagName.IsEmpty())
	{
		return;
	}

	MonolithSimpleArtifactSerialization::FGameplayTagDefinitionPayloadEntry Entry;
	Entry.TagName = TagName;
	if (const TSharedPtr<FGameplayTagNode> ParentNode = Node.GetParentTagNode())
	{
		Entry.ParentTag = ParentNode->GetCompleteTagString();
	}
	OutPayload.Definitions.Add(MoveTemp(Entry));

	for (const TSharedPtr<FGameplayTagNode>& ChildNode : Node.GetChildTagNodes())
	{
		if (ChildNode.IsValid())
		{
			AppendNodeDefinitions(*ChildNode, OutPayload);
		}
	}
}
