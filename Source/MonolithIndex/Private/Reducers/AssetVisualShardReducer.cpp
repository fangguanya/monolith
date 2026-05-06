#include "Reducers/AssetVisualShardReducer.h"

#include "Hash/xxhash.h"
#include "MonolithArtifactTypes.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexLog.h"

/*
 * Shard 快照的 artifact 序列化协议（与 mesh 行 artifact 是不同的结构）：
 *
 *   uint8  PayloadSchema = 2
 *   string ShardId
 *   string ProviderId
 *   uint32 ProviderVersion
 *   uint32 RenderRecipeVersion
 *   uint32 EmbeddingDim
 *   uint8  EmbeddingDtype
 *   uint8  bL2Normalized
 *   uint32 EntryCount
 *   for each entry:
 *     string AssetPath
 *     uint8  PhaseId                   // v2 新增：让 retriever 把命中映射回相位
 *     bytes  EmbeddingBytes (length = EmbeddingDim * sizeof(EmbeddingDtype))
 *
 * 该 schema 与单 mesh artifact 互不影响——shard 快照只承载 ANN 检索需要的最小信息。
 */
namespace AssetVisualShardArtifactInternal
{
	// v1: 单 phase shard 快照（每行 string AssetPath + blob EmbeddingBytes）
	// v2: 多 phase 快照，每行追加 uint8 PhaseId；retriever 用 PhaseId 把命中映射回真实相位。
	//     与 v1 不兼容（字节多了 N 字节）；旧 DDC shard artifact 必须重新 reduce。
	static constexpr uint8 ShardPayloadSchema = 2;

	static void WriteUInt8(TArray<uint8>& Bytes, uint8 V) { Bytes.Add(V); }
	static void WriteUInt32(TArray<uint8>& Bytes, uint32 V)
	{
		Bytes.Add(static_cast<uint8>(V & 0xff));
		Bytes.Add(static_cast<uint8>((V >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((V >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((V >> 24) & 0xff));
	}
	static void WriteString(TArray<uint8>& Bytes, const FString& V)
	{
		FTCHARToUTF8 Conv(*V);
		WriteUInt32(Bytes, static_cast<uint32>(Conv.Length()));
		if (Conv.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());
		}
	}
	static void WriteBlob(TArray<uint8>& Bytes, const uint8* Data, int32 Count)
	{
		WriteUInt32(Bytes, static_cast<uint32>(Count));
		if (Count > 0) Bytes.Append(Data, Count);
	}

	static bool ReadUInt8(const TArray<uint8>& Bytes, int32& Off, uint8& Out)
	{
		if (Off + 1 > Bytes.Num()) return false;
		Out = Bytes[Off++];
		return true;
	}
	static bool ReadUInt32(const TArray<uint8>& Bytes, int32& Off, uint32& Out)
	{
		if (Off + 4 > Bytes.Num()) return false;
		Out = static_cast<uint32>(Bytes[Off])
			| (static_cast<uint32>(Bytes[Off + 1]) << 8)
			| (static_cast<uint32>(Bytes[Off + 2]) << 16)
			| (static_cast<uint32>(Bytes[Off + 3]) << 24);
		Off += 4;
		return true;
	}
	static bool ReadString(const TArray<uint8>& Bytes, int32& Off, FString& Out)
	{
		uint32 Len = 0;
		if (!ReadUInt32(Bytes, Off, Len)) return false;
		if (Len > static_cast<uint32>(Bytes.Num() - Off)) return false;
		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Off), static_cast<int32>(Len));
		Out = FString(Conv.Length(), Conv.Get());
		Off += static_cast<int32>(Len);
		return true;
	}

	/** 把多 mesh 行序列化成一份 shard 快照 payload。 */
	static void Serialize(
		const FString& ShardId,
		const FString& ProviderId,
		const uint32 ProviderVersion,
		const uint32 RenderRecipeVersion,
		const int32 EmbeddingDim,
		const uint8 EmbeddingDtype,
		const bool bL2Normalized,
		const TArray<FIndexedAssetVisualEntry>& Entries,
		TArray<uint8>& OutBytes)
	{
		WriteUInt8(OutBytes, ShardPayloadSchema);
		WriteString(OutBytes, ShardId);
		WriteString(OutBytes, ProviderId);
		WriteUInt32(OutBytes, ProviderVersion);
		WriteUInt32(OutBytes, RenderRecipeVersion);
		WriteUInt32(OutBytes, static_cast<uint32>(EmbeddingDim));
		WriteUInt8(OutBytes, EmbeddingDtype);
		WriteUInt8(OutBytes, bL2Normalized ? 1u : 0u);
		WriteUInt32(OutBytes, static_cast<uint32>(Entries.Num()));
		for (const FIndexedAssetVisualEntry& Entry : Entries)
		{
			WriteString(OutBytes, Entry.AssetPath);
			WriteUInt8(OutBytes, Entry.PhaseId);
			WriteBlob(OutBytes, Entry.EmbeddingBytes.GetData(), Entry.EmbeddingBytes.Num());
		}
	}

	/** 反序列化 shard 快照 payload 到 retriever 期望的结构。 */
	static bool Deserialize(
		const TArray<uint8>& Bytes,
		FAssetVisualShardEmbeddings& OutEmbeddings)
	{
		int32 Off = 0;
		uint8 Schema = 0;
		if (!ReadUInt8(Bytes, Off, Schema) || Schema != ShardPayloadSchema)
		{
			return false;
		}

		FString ShardId, ProviderId;
		uint32 ProviderVersion = 0, RenderRecipeVersion = 0, EmbeddingDim = 0, EntryCount = 0;
		uint8 EmbeddingDtype = 0, NormalizedFlag = 0;
		if (!ReadString(Bytes, Off, ShardId)
			|| !ReadString(Bytes, Off, ProviderId)
			|| !ReadUInt32(Bytes, Off, ProviderVersion)
			|| !ReadUInt32(Bytes, Off, RenderRecipeVersion)
			|| !ReadUInt32(Bytes, Off, EmbeddingDim)
			|| !ReadUInt8(Bytes, Off, EmbeddingDtype)
			|| !ReadUInt8(Bytes, Off, NormalizedFlag)
			|| !ReadUInt32(Bytes, Off, EntryCount))
		{
			return false;
		}

		OutEmbeddings.ShardId = ShardId;
		OutEmbeddings.EmbeddingDim = static_cast<int32>(EmbeddingDim);
		OutEmbeddings.bL2Normalized = (NormalizedFlag != 0);
		OutEmbeddings.AssetPaths.Reset();
		OutEmbeddings.Vectors.Reset();
		OutEmbeddings.RowPhaseIds.Reset();
		OutEmbeddings.AssetPaths.Reserve(EntryCount);
		OutEmbeddings.RowPhaseIds.Reserve(EntryCount);

		// 只支持 FP32 与 FP16；FP16 在这里立刻 widen 成 FP32 给 retriever 用，
		// retriever 内部不必再判 dtype。
		const int32 BytesPerElement = (EmbeddingDtype == 0) ? 4 : 2;
		const int32 BytesPerVector = OutEmbeddings.EmbeddingDim * BytesPerElement;
		OutEmbeddings.Vectors.Reserve(EntryCount * OutEmbeddings.EmbeddingDim);

		for (uint32 Index = 0; Index < EntryCount; ++Index)
		{
			FString AssetPath;
			uint8 PhaseId = 0;
			uint32 BlobLen = 0;
			if (!ReadString(Bytes, Off, AssetPath)
				|| !ReadUInt8(Bytes, Off, PhaseId)
				|| !ReadUInt32(Bytes, Off, BlobLen))
			{
				return false;
			}
			if (static_cast<int32>(BlobLen) != BytesPerVector)
			{
				return false;
			}
			if (Off + static_cast<int32>(BlobLen) > Bytes.Num())
			{
				return false;
			}

			OutEmbeddings.AssetPaths.Add(MoveTemp(AssetPath));
			OutEmbeddings.RowPhaseIds.Add(PhaseId);
			if (EmbeddingDtype == 0)
			{
				const float* SrcF32 = reinterpret_cast<const float*>(Bytes.GetData() + Off);
				OutEmbeddings.Vectors.Append(SrcF32, OutEmbeddings.EmbeddingDim);
			}
			else
			{
				// FP16 → FP32 widen：标准 IEEE half precision。
				const uint16* SrcF16 = reinterpret_cast<const uint16*>(Bytes.GetData() + Off);
				for (int32 D = 0; D < OutEmbeddings.EmbeddingDim; ++D)
				{
					const uint16 H = SrcF16[D];
					const uint32 Sign = (H >> 15) & 0x1;
					const uint32 Exp = (H >> 10) & 0x1f;
					const uint32 Mant = H & 0x3ff;
					uint32 F = 0;
					if (Exp == 0)
					{
						if (Mant == 0)
						{
							F = Sign << 31;
						}
						else
						{
							// subnormal half → normal float
							int32 E = -14;
							uint32 M = Mant;
							while ((M & 0x400) == 0) { M <<= 1; --E; }
							M &= 0x3ff;
							F = (Sign << 31) | (static_cast<uint32>(E + 127) << 23) | (M << 13);
						}
					}
					else if (Exp == 31)
					{
						F = (Sign << 31) | 0x7f800000 | (Mant << 13);
					}
					else
					{
						F = (Sign << 31) | (static_cast<uint32>(Exp - 15 + 127) << 23) | (Mant << 13);
					}
					float V = 0.0f;
					FMemory::Memcpy(&V, &F, sizeof(float));
					OutEmbeddings.Vectors.Add(V);
				}
			}
			Off += static_cast<int32>(BlobLen);
		}
		return true;
	}
}

namespace AssetVisualShardReducerInternal
{
	/** 给 cohort 选择对应 DDC bucket name 与 capacity policy。 */
	struct FCohortConfig
	{
		FString BucketName;
		FName ProviderId;
		FAssetVisualShardCapacityPolicy Policy;
	};

	/** 构造 reducer artifact identity；shard 级 identity 用 ProviderId + ShardId + UpstreamHash。
	 *  这样 shard 内任一 mesh 变化都会导致 UpstreamHash 改变 → identity hash 改变 → 写新 artifact。 */
	static FMonolithArtifactIdentityV1 MakeShardIdentity(
		const FName ProviderId,
		const FString& ShardId,
		const uint32 ProviderVersion,
		const uint32 RenderRecipeVersion,
		const FString& UpstreamFingerprint)
	{
		FMonolithArtifactIdentityV1 Identity;
		Identity.IdentitySchemaVersion = 1;
		Identity.IndexerId = ProviderId;
		Identity.IndexerVersion = ProviderVersion;
		Identity.ArtifactSchemaVersion = AssetVisualShardArtifactInternal::ShardPayloadSchema;
		Identity.PackageName = FName(*FString::Printf(TEXT("AssetVisualShard:%s"), *ShardId));
		Identity.IdentityProvider = EMonolithIdentityProvider::ManifestV1;
		Identity.PackageFingerprint = FString::Printf(TEXT("%s|recipe=%u"), *UpstreamFingerprint, RenderRecipeVersion);
		return Identity;
	}

	/** 对 shard 内全部 mesh 行计算稳定 upstream 指纹（顺序无关）。 */
	static FString ComputeShardUpstreamFingerprint(const TArray<FIndexedAssetVisualEntry>& Entries)
	{
		// 用每条 mesh 的 (AssetPath, EmbeddingBytes) hash 之 XOR 累加得到顺序无关的指纹。
		uint64 Mix = 0;
		for (const FIndexedAssetVisualEntry& Entry : Entries)
		{
			TArray<uint8> Buf;
			Buf.Reserve(Entry.AssetPath.Len() + Entry.EmbeddingBytes.Num() + 16);
			AssetVisualShardArtifactInternal::WriteString(Buf, Entry.AssetPath);
			AssetVisualShardArtifactInternal::WriteBlob(Buf, Entry.EmbeddingBytes.GetData(), Entry.EmbeddingBytes.Num());
			const uint64 H = FXxHash64::HashBuffer(Buf.GetData(), static_cast<uint64>(Buf.Num())).Hash;
			Mix ^= H;
		}
		return FString::Printf(TEXT("%016llx-n%d"), Mix, Entries.Num());
	}
}

FAssetVisualShardReducerStats FAssetVisualShardReducer::RebuildAllShards(
	FMonolithIndexDatabase& DB,
	IMonolithArtifactCache& DdcCache,
	const FString& CohortName,
	const FAssetVisualShardCapacityPolicy& Policy)
{
	FAssetVisualShardReducerStats Stats;

	// 1) 拉全 cohort 行 → 算 shard 划分
	const TArray<FIndexedAssetVisualEntry> AllEntries = DB.GetAssetVisualEntries(CohortName, FString());
	if (AllEntries.Num() == 0)
	{
		return Stats;
	}

	TArray<FString> AssetPaths;
	AssetPaths.Reserve(AllEntries.Num());
	for (const FIndexedAssetVisualEntry& Entry : AllEntries)
	{
		AssetPaths.Add(Entry.AssetPath);
	}
	const TMap<FString, FAssetVisualShardKey> ShardAssignment = AssignAssetVisualShardsForCohort(AssetPaths, Policy);

	// 2) 把 mesh 行按 ShardId 分桶
	TMap<FString, TArray<FIndexedAssetVisualEntry>> EntriesByShard;
	for (const FIndexedAssetVisualEntry& Entry : AllEntries)
	{
		const FAssetVisualShardKey* Key = ShardAssignment.Find(Entry.AssetPath);
		if (!Key)
		{
			continue;
		}
		EntriesByShard.FindOrAdd(Key->ShardId).Add(Entry);
		++Stats.EntriesProcessed;
	}
	Stats.ShardCount = EntriesByShard.Num();

	// 3) 每个 shard 序列化成 artifact 写 DDC
	if (AllEntries.Num() == 0)
	{
		return Stats;
	}
	const FString ProviderId = AllEntries[0].ProviderId;
	const uint32 ProviderVersion = AllEntries[0].ProviderVersion;
	const uint32 RenderRecipeVersion = AllEntries[0].RenderRecipeVersion;
	const int32 EmbeddingDim = AllEntries[0].EmbeddingDim;
	const uint8 EmbeddingDtype = AllEntries[0].EmbeddingDtype;
	// 整 cohort 强制使用 L2 normalized embedding（geometric/semantic provider 都遵守）。
	const bool bL2Normalized = true;

	for (TPair<FString, TArray<FIndexedAssetVisualEntry>>& Pair : EntriesByShard)
	{
		const FString& ShardId = Pair.Key;
		TArray<FIndexedAssetVisualEntry>& ShardEntries = Pair.Value;

		// 写入前对 shard 内部按 AssetPath 排序，保证 reducer 的顺序无关 + 输出稳定。
		ShardEntries.Sort([](const FIndexedAssetVisualEntry& A, const FIndexedAssetVisualEntry& B)
		{
			return A.AssetPath < B.AssetPath;
		});

		const FString UpstreamFingerprint = AssetVisualShardReducerInternal::ComputeShardUpstreamFingerprint(ShardEntries);

		FMonolithArtifact Artifact;
		Artifact.ArtifactSchemaVersion = AssetVisualShardArtifactInternal::ShardPayloadSchema;
		Artifact.IndexerId = FName(*ProviderId);
		Artifact.IndexerVersion = ProviderVersion;
		Artifact.ExecutionMode = EMonolithExecutionMode::GlobalReducer;
		Artifact.PackageName = FString::Printf(TEXT("AssetVisualShard:%s"), *ShardId);
		AssetVisualShardArtifactInternal::Serialize(
			ShardId, ProviderId, ProviderVersion, RenderRecipeVersion,
			EmbeddingDim, EmbeddingDtype, bL2Normalized,
			ShardEntries, Artifact.Payload);

		const FMonolithArtifactIdentityV1 Identity = AssetVisualShardReducerInternal::MakeShardIdentity(
			Artifact.IndexerId, ShardId, ProviderVersion, RenderRecipeVersion, UpstreamFingerprint);
		Artifact.IdentityHash = HashMonolithArtifactIdentity(Identity);

		const bool bPutOk = DdcCache.Put(Identity, Artifact, EMonolithArtifactCacheRequestMode::Background);
		if (bPutOk)
		{
			++Stats.ShardsWritten;
		}
		else
		{
			++Stats.ShardsSkipped;
			UE_LOG(LogMonolithIndex, Warning,
				TEXT("AssetVisualShardReducer 写 shard '%s' (cohort %s) 失败"),
				*ShardId, *CohortName);
		}
	}
	return Stats;
}

