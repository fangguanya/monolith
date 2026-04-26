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

	static constexpr uint8 PayloadSchemaVersion = 1;
}

namespace AssetVisualArtifactSerializer
{
	void SerializePayload(
		const FIndexedAssetVisualEntry& Entry,
		const TArray<uint8>& PreviewPng,
		TArray<uint8>& OutBytes)
	{
		using namespace AssetVisualArtifactSerializerInternal;

		WriteUInt8(OutBytes, PayloadSchemaVersion);
		WriteString(OutBytes, Entry.AssetPath);
		WriteString(OutBytes, Entry.ShardId);
		WriteUInt32(OutBytes, static_cast<uint32>(Entry.ShardPrefixDepth));
		WriteString(OutBytes, Entry.ProviderId);
		WriteUInt32(OutBytes, Entry.ProviderVersion);
		WriteUInt32(OutBytes, Entry.RenderRecipeVersion);
		WriteUInt32(OutBytes, static_cast<uint32>(Entry.EmbeddingDim));
		WriteUInt8(OutBytes, Entry.EmbeddingDtype);
		WriteBlob(OutBytes, Entry.EmbeddingBytes);
		WriteBlob(OutBytes, PreviewPng);
	}

	bool DeserializePayload(
		const TArray<uint8>& Bytes,
		FIndexedAssetVisualEntry& OutEntry,
		TArray<uint8>& OutPreviewPng)
	{
		using namespace AssetVisualArtifactSerializerInternal;

		int32 Offset = 0;
		uint8 SchemaVersion = 0;
		uint32 ShardPrefixDepth = 0;
		uint32 EmbeddingDim = 0;

		if (!ReadUInt8(Bytes, Offset, SchemaVersion))
		{
			return false;
		}
		if (SchemaVersion != PayloadSchemaVersion)
		{
			return false;
		}
		if (!ReadString(Bytes, Offset, OutEntry.AssetPath)
			|| !ReadString(Bytes, Offset, OutEntry.ShardId)
			|| !ReadUInt32(Bytes, Offset, ShardPrefixDepth)
			|| !ReadString(Bytes, Offset, OutEntry.ProviderId)
			|| !ReadUInt32(Bytes, Offset, OutEntry.ProviderVersion)
			|| !ReadUInt32(Bytes, Offset, OutEntry.RenderRecipeVersion)
			|| !ReadUInt32(Bytes, Offset, EmbeddingDim)
			|| !ReadUInt8(Bytes, Offset, OutEntry.EmbeddingDtype)
			|| !ReadBlob(Bytes, Offset, OutEntry.EmbeddingBytes)
			|| !ReadBlob(Bytes, Offset, OutPreviewPng))
		{
			return false;
		}

		OutEntry.ShardPrefixDepth = static_cast<int32>(ShardPrefixDepth);
		OutEntry.EmbeddingDim = static_cast<int32>(EmbeddingDim);
		return true;
	}
}
