#include "AssetVisualArtifact.h"
#include "AssetVisualEntry.h"

#include "Misc/AutomationTest.h"

/*
 * AssetVisual artifact 序列化测试覆盖：
 *  - round-trip：序列化后立刻反序列化必须 bit-identical
 *  - 携带 vs 不携带 preview PNG 都能正确解析
 *  - schema 不识别时 deserialize 必须返回 false（不允许静默错位）
 */

namespace AssetVisualArtifactTestInternal
{
	static FIndexedAssetVisualEntry MakeSampleEntry()
	{
		FIndexedAssetVisualEntry Entry;
		Entry.AssetPath = TEXT("/Game/Test/SM_Sample.SM_Sample");
		Entry.ShardId = TEXT("Game.Test");
		Entry.ShardPrefixDepth = 2;
		Entry.ProviderId = TEXT("geometric_v1");
		Entry.ProviderVersion = 1;
		Entry.RenderRecipeVersion = 1;
		Entry.EmbeddingDim = 4;
		Entry.EmbeddingDtype = 0;

		// 4 个 FP32 = 16 字节，按小端 IEEE754 写。
		const float Sample[4] = { 0.5f, -0.25f, 0.0f, 1.0f };
		Entry.EmbeddingBytes.SetNumUninitialized(sizeof(Sample));
		FMemory::Memcpy(Entry.EmbeddingBytes.GetData(), Sample, sizeof(Sample));
		return Entry;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualArtifactRoundTripWithPngTest,
	"Monolith.Index.AssetVisual.Artifact.RoundTripWithPng",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualArtifactRoundTripWithPngTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualArtifactTestInternal;

	const FIndexedAssetVisualEntry InputEntry = MakeSampleEntry();
	const TArray<uint8> InputPng = { 0x89, 0x50, 0x4E, 0x47, 0x01, 0x02, 0x03, 0x04 };

	TArray<uint8> Payload;
	AssetVisualArtifactSerializer::SerializePayload(InputEntry, InputPng, Payload);
	TestTrue(TEXT("payload should be non-empty"), Payload.Num() > 0);

	FIndexedAssetVisualEntry OutEntry;
	TArray<uint8> OutPng;
	const bool bOk = AssetVisualArtifactSerializer::DeserializePayload(Payload, OutEntry, OutPng);
	TestTrue(TEXT("deserialize must succeed"), bOk);

	TestEqual(TEXT("asset path"), OutEntry.AssetPath, InputEntry.AssetPath);
	TestEqual(TEXT("shard id"), OutEntry.ShardId, InputEntry.ShardId);
	TestEqual(TEXT("shard prefix depth"), OutEntry.ShardPrefixDepth, InputEntry.ShardPrefixDepth);
	TestEqual(TEXT("provider id"), OutEntry.ProviderId, InputEntry.ProviderId);
	TestEqual(TEXT("provider version"), static_cast<int32>(OutEntry.ProviderVersion), static_cast<int32>(InputEntry.ProviderVersion));
	TestEqual(TEXT("render recipe version"), static_cast<int32>(OutEntry.RenderRecipeVersion), static_cast<int32>(InputEntry.RenderRecipeVersion));
	TestEqual(TEXT("embedding dim"), OutEntry.EmbeddingDim, InputEntry.EmbeddingDim);
	TestEqual(TEXT("embedding dtype"), static_cast<int32>(OutEntry.EmbeddingDtype), static_cast<int32>(InputEntry.EmbeddingDtype));
	TestEqual(TEXT("embedding bytes count"), OutEntry.EmbeddingBytes.Num(), InputEntry.EmbeddingBytes.Num());
	TestEqual(TEXT("preview png bytes count"), OutPng.Num(), InputPng.Num());

	for (int32 Index = 0; Index < InputEntry.EmbeddingBytes.Num(); ++Index)
	{
		TestEqual(TEXT("embedding byte"), OutEntry.EmbeddingBytes[Index], InputEntry.EmbeddingBytes[Index]);
	}
	for (int32 Index = 0; Index < InputPng.Num(); ++Index)
	{
		TestEqual(TEXT("preview byte"), OutPng[Index], InputPng[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualArtifactRoundTripNoPngTest,
	"Monolith.Index.AssetVisual.Artifact.RoundTripNoPng",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualArtifactRoundTripNoPngTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualArtifactTestInternal;

	const FIndexedAssetVisualEntry InputEntry = MakeSampleEntry();
	const TArray<uint8> EmptyPng;

	TArray<uint8> Payload;
	AssetVisualArtifactSerializer::SerializePayload(InputEntry, EmptyPng, Payload);

	FIndexedAssetVisualEntry OutEntry;
	TArray<uint8> OutPng;
	TestTrue(TEXT("deserialize empty preview should still succeed"),
		AssetVisualArtifactSerializer::DeserializePayload(Payload, OutEntry, OutPng));
	TestEqual(TEXT("empty preview"), OutPng.Num(), 0);
	TestEqual(TEXT("asset path preserved"), OutEntry.AssetPath, InputEntry.AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualArtifactSchemaMismatchTest,
	"Monolith.Index.AssetVisual.Artifact.SchemaMismatchRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualArtifactSchemaMismatchTest::RunTest(const FString& Parameters)
{
	// 头字节为 0xFF 的 schema 必须立刻被拒，避免静默把损坏数据当作有效解析。
	const TArray<uint8> Garbage = { 0xFF, 0x00, 0x00, 0x00, 0x00 };
	FIndexedAssetVisualEntry Entry;
	TArray<uint8> Png;
	TestFalse(TEXT("garbage payload must be rejected"),
		AssetVisualArtifactSerializer::DeserializePayload(Garbage, Entry, Png));
	return true;
}
