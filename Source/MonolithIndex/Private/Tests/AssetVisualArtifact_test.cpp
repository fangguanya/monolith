#include "AssetVisualArtifact.h"
#include "AssetVisualEntry.h"

#include "Misc/AutomationTest.h"

/*
 * AssetVisual artifact 序列化测试覆盖：
 *  - 单 phase round-trip：序列化后立刻反序列化必须 bit-identical
 *  - 多 phase round-trip：3 phase 数据全部正确还原（每 phase 独立 PNG + embedding）
 *  - 不携带 preview PNG（semantic 路径）也能正确解析
 *  - schema 不识别（v1 / v2 旧格式 / 垃圾字节）时 deserialize 必须返回 false
 */

namespace AssetVisualArtifactTestInternal
{
	static FIndexedAssetVisualEntry MakeSampleEntry(uint8 PhaseId = 0, float PhaseT = 0.0f, const TCHAR* PhaseLabel = TEXT(""))
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
		Entry.PhaseId = PhaseId;
		Entry.PhaseT = PhaseT;
		Entry.PhaseLabel = PhaseLabel;

		// 4 个 FP32 = 16 字节，按小端 IEEE754 写。每个 phase 给不同 embedding 让 round-trip 区分。
		const float Sample[4] = { 0.5f + PhaseId * 0.1f, -0.25f, 0.0f, 1.0f };
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

	TArray<FIndexedAssetVisualEntry> InputEntries;
	InputEntries.Add(MakeSampleEntry());
	TArray<TArray<uint8>> InputPngs;
	InputPngs.Add(TArray<uint8>{ 0x89, 0x50, 0x4E, 0x47, 0x01, 0x02, 0x03, 0x04 });

	TArray<uint8> Payload;
	AssetVisualArtifactSerializer::SerializePayload(InputEntries, InputPngs, Payload);
	TestTrue(TEXT("payload should be non-empty"), Payload.Num() > 0);

	TArray<FIndexedAssetVisualEntry> OutEntries;
	TArray<TArray<uint8>> OutPngs;
	const bool bOk = AssetVisualArtifactSerializer::DeserializePayload(Payload, OutEntries, OutPngs);
	TestTrue(TEXT("deserialize must succeed"), bOk);
	TestEqual(TEXT("entry count"), OutEntries.Num(), 1);
	TestEqual(TEXT("png count matches entry count"), OutPngs.Num(), OutEntries.Num());
	if (OutEntries.Num() != 1 || OutPngs.Num() != 1)
	{
		return false;
	}

	const FIndexedAssetVisualEntry& OutEntry = OutEntries[0];
	const FIndexedAssetVisualEntry& InputEntry = InputEntries[0];
	TestEqual(TEXT("asset path"), OutEntry.AssetPath, InputEntry.AssetPath);
	TestEqual(TEXT("shard id"), OutEntry.ShardId, InputEntry.ShardId);
	TestEqual(TEXT("shard prefix depth"), OutEntry.ShardPrefixDepth, InputEntry.ShardPrefixDepth);
	TestEqual(TEXT("provider id"), OutEntry.ProviderId, InputEntry.ProviderId);
	TestEqual(TEXT("provider version"), static_cast<int32>(OutEntry.ProviderVersion), static_cast<int32>(InputEntry.ProviderVersion));
	TestEqual(TEXT("render recipe version"), static_cast<int32>(OutEntry.RenderRecipeVersion), static_cast<int32>(InputEntry.RenderRecipeVersion));
	TestEqual(TEXT("embedding dim"), OutEntry.EmbeddingDim, InputEntry.EmbeddingDim);
	TestEqual(TEXT("embedding dtype"), static_cast<int32>(OutEntry.EmbeddingDtype), static_cast<int32>(InputEntry.EmbeddingDtype));
	TestEqual(TEXT("embedding bytes count"), OutEntry.EmbeddingBytes.Num(), InputEntry.EmbeddingBytes.Num());
	TestEqual(TEXT("phase id"), static_cast<int32>(OutEntry.PhaseId), static_cast<int32>(InputEntry.PhaseId));
	TestEqual(TEXT("phase t"), OutEntry.PhaseT, InputEntry.PhaseT);
	TestEqual(TEXT("phase label"), OutEntry.PhaseLabel, InputEntry.PhaseLabel);
	TestEqual(TEXT("preview png bytes count"), OutPngs[0].Num(), InputPngs[0].Num());

	for (int32 Index = 0; Index < InputEntry.EmbeddingBytes.Num(); ++Index)
	{
		TestEqual(TEXT("embedding byte"), OutEntry.EmbeddingBytes[Index], InputEntry.EmbeddingBytes[Index]);
	}
	for (int32 Index = 0; Index < InputPngs[0].Num(); ++Index)
	{
		TestEqual(TEXT("preview byte"), OutPngs[0][Index], InputPngs[0][Index]);
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

	TArray<FIndexedAssetVisualEntry> InputEntries;
	InputEntries.Add(MakeSampleEntry());
	TArray<TArray<uint8>> EmptyPngs;
	EmptyPngs.Add(TArray<uint8>());

	TArray<uint8> Payload;
	AssetVisualArtifactSerializer::SerializePayload(InputEntries, EmptyPngs, Payload);

	TArray<FIndexedAssetVisualEntry> OutEntries;
	TArray<TArray<uint8>> OutPngs;
	TestTrue(TEXT("deserialize empty preview should still succeed"),
		AssetVisualArtifactSerializer::DeserializePayload(Payload, OutEntries, OutPngs));
	TestEqual(TEXT("entry count"), OutEntries.Num(), 1);
	TestEqual(TEXT("png count"), OutPngs.Num(), 1);
	if (OutPngs.Num() == 1)
	{
		TestEqual(TEXT("empty preview"), OutPngs[0].Num(), 0);
	}
	if (OutEntries.Num() == 1)
	{
		TestEqual(TEXT("asset path preserved"), OutEntries[0].AssetPath, InputEntries[0].AssetPath);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualArtifactMultiPhaseRoundTripTest,
	"Monolith.Index.AssetVisual.Artifact.MultiPhaseRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualArtifactMultiPhaseRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace AssetVisualArtifactTestInternal;

	// Anim 风格的 3 phase：early/middle/late，归一化时间。
	TArray<FIndexedAssetVisualEntry> InputEntries;
	InputEntries.Add(MakeSampleEntry(0, 0.25f, TEXT("early")));
	InputEntries.Add(MakeSampleEntry(1, 0.50f, TEXT("middle")));
	InputEntries.Add(MakeSampleEntry(2, 0.75f, TEXT("late")));

	// 每 phase 给一份不同字节的 PNG，验证 round-trip 后顺序与字节都对应。
	TArray<TArray<uint8>> InputPngs;
	InputPngs.Add(TArray<uint8>{ 0x89, 0x50, 0x4E, 0x47, 0xa0 });
	InputPngs.Add(TArray<uint8>{ 0x89, 0x50, 0x4E, 0x47, 0xb1 });
	InputPngs.Add(TArray<uint8>{ 0x89, 0x50, 0x4E, 0x47, 0xc2 });

	TArray<uint8> Payload;
	AssetVisualArtifactSerializer::SerializePayload(InputEntries, InputPngs, Payload);
	TestTrue(TEXT("multi-phase payload non-empty"), Payload.Num() > 0);

	TArray<FIndexedAssetVisualEntry> OutEntries;
	TArray<TArray<uint8>> OutPngs;
	TestTrue(TEXT("multi-phase deserialize must succeed"),
		AssetVisualArtifactSerializer::DeserializePayload(Payload, OutEntries, OutPngs));
	TestEqual(TEXT("phase count"), OutEntries.Num(), 3);
	TestEqual(TEXT("png count matches"), OutPngs.Num(), 3);

	if (OutEntries.Num() != 3 || OutPngs.Num() != 3)
	{
		return false;
	}

	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FIndexedAssetVisualEntry& In = InputEntries[Index];
		const FIndexedAssetVisualEntry& Out = OutEntries[Index];
		TestEqual(TEXT("phase id"), static_cast<int32>(Out.PhaseId), static_cast<int32>(In.PhaseId));
		TestEqual(TEXT("phase t"), Out.PhaseT, In.PhaseT);
		TestEqual(TEXT("phase label"), Out.PhaseLabel, In.PhaseLabel);
		TestEqual(TEXT("phase asset path shared"), Out.AssetPath, In.AssetPath);
		TestEqual(TEXT("phase embedding bytes"), Out.EmbeddingBytes.Num(), In.EmbeddingBytes.Num());
		TestEqual(TEXT("phase png bytes"), OutPngs[Index].Num(), InputPngs[Index].Num());
		// Phase 内 embedding 字节 + PNG 字节都必须 bit-identical 还原。
		for (int32 ByteIndex = 0; ByteIndex < In.EmbeddingBytes.Num(); ++ByteIndex)
		{
			TestEqual(TEXT("phase embedding byte"), Out.EmbeddingBytes[ByteIndex], In.EmbeddingBytes[ByteIndex]);
		}
		for (int32 ByteIndex = 0; ByteIndex < InputPngs[Index].Num(); ++ByteIndex)
		{
			TestEqual(TEXT("phase png byte"), OutPngs[Index][ByteIndex], InputPngs[Index][ByteIndex]);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAssetVisualArtifactSchemaMismatchTest,
	"Monolith.Index.AssetVisual.Artifact.SchemaMismatchRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetVisualArtifactSchemaMismatchTest::RunTest(const FString& Parameters)
{
	// 三种 schema mismatch 都必须立刻被拒，避免静默把损坏数据当作有效解析：
	//  - 0xFF 垃圾头
	//  - v1 旧格式（schema=1）
	//  - v2 中间格式（schema=2，多 phase 但共享单 PNG，与 v3 per-phase PNG 不兼容）
	const TArray<uint8> Garbage = { 0xFF, 0x00, 0x00, 0x00, 0x00 };
	TArray<FIndexedAssetVisualEntry> Entries;
	TArray<TArray<uint8>> Pngs;
	TestFalse(TEXT("garbage payload must be rejected"),
		AssetVisualArtifactSerializer::DeserializePayload(Garbage, Entries, Pngs));

	const TArray<uint8> LegacyV1 = { 0x01, 0x00, 0x00, 0x00, 0x00 };
	TArray<FIndexedAssetVisualEntry> EntriesV1;
	TArray<TArray<uint8>> PngsV1;
	TestFalse(TEXT("legacy v1 payload must be rejected by v3 reader"),
		AssetVisualArtifactSerializer::DeserializePayload(LegacyV1, EntriesV1, PngsV1));

	const TArray<uint8> LegacyV2 = { 0x02, 0x00, 0x00, 0x00, 0x00 };
	TArray<FIndexedAssetVisualEntry> EntriesV2;
	TArray<TArray<uint8>> PngsV2;
	TestFalse(TEXT("legacy v2 payload must be rejected by v3 reader"),
		AssetVisualArtifactSerializer::DeserializePayload(LegacyV2, EntriesV2, PngsV2));
	return true;
}
