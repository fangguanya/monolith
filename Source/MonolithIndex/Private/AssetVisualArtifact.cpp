#include "AssetVisualArtifact.h"

/*
 * 序列化实现刻意手写显式 little-endian + length-prefixed string，与 MeshCatalogIndexer 的
 * payload 序列化风格保持一致。这样跨平台读写、artifact identity 哈希都不会被默认 FArchive
 * 行为漂移。
 */
namespace AssetVisualArtifactSerializerInternal
{
	static void WriteUInt8(TArray<uint8>& Bytes, const uint8 Value)
	{
		Bytes.Add(Value);
	}

	static void WriteUInt32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	static void WriteFloatBits(TArray<uint8>& Bytes, const float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(uint32));
		WriteUInt32(Bytes, Bits);
	}

	static void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	static void WriteBlob(TArray<uint8>& Bytes, const TArray<uint8>& Blob)
	{
		WriteUInt32(Bytes, static_cast<uint32>(Blob.Num()));
		if (Blob.Num() > 0)
		{
			Bytes.Append(Blob.GetData(), Blob.Num());
		}
	}

	static bool ReadUInt8(const TArray<uint8>& Bytes, int32& Offset, uint8& OutValue)
	{
		if (Offset + 1 > Bytes.Num())
		{
			return false;
		}
		OutValue = Bytes[Offset++];
		return true;
	}

	static bool ReadUInt32(const TArray<uint8>& Bytes, int32& Offset, uint32& OutValue)
	{
		if (Offset + 4 > Bytes.Num())
		{
			return false;
		}
		OutValue =
			static_cast<uint32>(Bytes[Offset]) |
			(static_cast<uint32>(Bytes[Offset + 1]) << 8) |
			(static_cast<uint32>(Bytes[Offset + 2]) << 16) |
			(static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4;
		return true;
	}

	static bool ReadFloatBits(const TArray<uint8>& Bytes, int32& Offset, float& OutValue)
	{
		uint32 Bits = 0;
		if (!ReadUInt32(Bytes, Offset, Bits))
		{
			return false;
		}
		FMemory::Memcpy(&OutValue, &Bits, sizeof(uint32));
		return true;
	}

	static bool ReadString(const TArray<uint8>& Bytes, int32& Offset, FString& OutValue)
	{
		uint32 Length = 0;
		if (!ReadUInt32(Bytes, Offset, Length))
		{
			return false;
		}
		if (Length > static_cast<uint32>(Bytes.Num() - Offset))
		{
			return false;
		}
		FUTF8ToTCHAR Convert(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset), static_cast<int32>(Length));
		OutValue = FString(Convert.Length(), Convert.Get());
		Offset += static_cast<int32>(Length);
		return true;
	}

	static bool ReadBlob(const TArray<uint8>& Bytes, int32& Offset, TArray<uint8>& OutBlob)
	{
		uint32 Length = 0;
		if (!ReadUInt32(Bytes, Offset, Length))
		{
			return false;
		}
		if (Length > static_cast<uint32>(Bytes.Num() - Offset))
		{
			return false;
		}
		OutBlob.Reset();
		if (Length > 0)
		{
			OutBlob.Append(Bytes.GetData() + Offset, static_cast<int32>(Length));
		}
		Offset += static_cast<int32>(Length);
		return true;
	}

	static constexpr uint8 PayloadSchemaVersion = 3;
}

namespace AssetVisualArtifactSerializer
{
	void SerializePayload(
		const TArray<FIndexedAssetVisualEntry>& Entries,
		const TArray<TArray<uint8>>& PerPhasePreviewPngs,
		TArray<uint8>& OutBytes)
	{
		using namespace AssetVisualArtifactSerializerInternal;

		// Caller 必须保证至少一份 entry；空数组 payload 没有共享字段可写。
		// 走到这里前 BuildArtifact 已经判断过 entry 非空。
		check(Entries.Num() > 0);
		// PNG 数组长度必须严格匹配 entry 数；不匹配是上游契约违反，立刻 check 出来。
		check(PerPhasePreviewPngs.Num() == Entries.Num());

		// 共享字段从第一份 entry 取；调用方契约要求所有 entry 在共享字段上必须一致。
		const FIndexedAssetVisualEntry& Head = Entries[0];

		WriteUInt8(OutBytes, PayloadSchemaVersion);
		WriteString(OutBytes, Head.AssetPath);
		WriteString(OutBytes, Head.ShardId);
		WriteUInt32(OutBytes, static_cast<uint32>(Head.ShardPrefixDepth));
		WriteString(OutBytes, Head.ProviderId);
		WriteUInt32(OutBytes, Head.ProviderVersion);
		WriteUInt32(OutBytes, Head.RenderRecipeVersion);
		WriteUInt32(OutBytes, static_cast<uint32>(Head.EmbeddingDim));
		WriteUInt8(OutBytes, Head.EmbeddingDtype);

		WriteUInt32(OutBytes, static_cast<uint32>(Entries.Num()));
		for (int32 PhaseIndex = 0; PhaseIndex < Entries.Num(); ++PhaseIndex)
		{
			const FIndexedAssetVisualEntry& Entry = Entries[PhaseIndex];
			WriteUInt8(OutBytes, Entry.PhaseId);
			WriteFloatBits(OutBytes, Entry.PhaseT);
			WriteString(OutBytes, Entry.PhaseLabel);
			WriteBlob(OutBytes, Entry.EmbeddingBytes);
			WriteBlob(OutBytes, PerPhasePreviewPngs[PhaseIndex]);
		}
	}

	bool DeserializePayload(
		const TArray<uint8>& Bytes,
		TArray<FIndexedAssetVisualEntry>& OutEntries,
		TArray<TArray<uint8>>& OutPerPhasePreviewPngs)
	{
		using namespace AssetVisualArtifactSerializerInternal;

		OutEntries.Reset();
		OutPerPhasePreviewPngs.Reset();

		int32 Offset = 0;
		uint8 SchemaVersion = 0;
		if (!ReadUInt8(Bytes, Offset, SchemaVersion))
		{
			return false;
		}
		if (SchemaVersion != PayloadSchemaVersion)
		{
			return false;
		}

		FString AssetPath;
		FString ShardId;
		uint32 ShardPrefixDepth = 0;
		FString ProviderId;
		uint32 ProviderVersion = 1;
		uint32 RenderRecipeVersion = 1;
		uint32 EmbeddingDim = 0;
		uint8 EmbeddingDtype = 0;

		if (!ReadString(Bytes, Offset, AssetPath)
			|| !ReadString(Bytes, Offset, ShardId)
			|| !ReadUInt32(Bytes, Offset, ShardPrefixDepth)
			|| !ReadString(Bytes, Offset, ProviderId)
			|| !ReadUInt32(Bytes, Offset, ProviderVersion)
			|| !ReadUInt32(Bytes, Offset, RenderRecipeVersion)
			|| !ReadUInt32(Bytes, Offset, EmbeddingDim)
			|| !ReadUInt8(Bytes, Offset, EmbeddingDtype))
		{
			return false;
		}

		uint32 NumPhases = 0;
		if (!ReadUInt32(Bytes, Offset, NumPhases))
		{
			return false;
		}
		// 防御：至少 1 份 phase；NumPhases=0 说明 payload 损坏。
		if (NumPhases == 0)
		{
			return false;
		}
		// 防御：NumPhases 上限保护（VFX/Anim 实际只有 3 phase；放宽到 256 给未来扩展空间）。
		if (NumPhases > 256)
		{
			return false;
		}

		OutEntries.Reserve(static_cast<int32>(NumPhases));
		OutPerPhasePreviewPngs.Reserve(static_cast<int32>(NumPhases));
		for (uint32 PhaseIndex = 0; PhaseIndex < NumPhases; ++PhaseIndex)
		{
			FIndexedAssetVisualEntry Entry;
			Entry.AssetPath = AssetPath;
			Entry.ShardId = ShardId;
			Entry.ShardPrefixDepth = static_cast<int32>(ShardPrefixDepth);
			Entry.ProviderId = ProviderId;
			Entry.ProviderVersion = ProviderVersion;
			Entry.RenderRecipeVersion = RenderRecipeVersion;
			Entry.EmbeddingDim = static_cast<int32>(EmbeddingDim);
			Entry.EmbeddingDtype = EmbeddingDtype;

			uint8 PhaseId = 0;
			float PhaseT = 0.0f;
			TArray<uint8> PhasePng;
			if (!ReadUInt8(Bytes, Offset, PhaseId)
				|| !ReadFloatBits(Bytes, Offset, PhaseT)
				|| !ReadString(Bytes, Offset, Entry.PhaseLabel)
				|| !ReadBlob(Bytes, Offset, Entry.EmbeddingBytes)
				|| !ReadBlob(Bytes, Offset, PhasePng))
			{
				return false;
			}
			Entry.PhaseId = PhaseId;
			Entry.PhaseT = PhaseT;
			OutEntries.Add(MoveTemp(Entry));
			OutPerPhasePreviewPngs.Add(MoveTemp(PhasePng));
		}
		return true;
	}
}
