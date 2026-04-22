#include "Indexers/MeshCatalogIndexer.h"

#include "MonolithIndexerShadowMode.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "AssetRegistry/IAssetRegistry.h"

/*
 * 这份实现文件把“静态网格目录行”收口成单资产 artifact。
 *
 * 以前的旧实现会：
 * - 自己再扫一遍全项目 StaticMesh；
 * - 直接删整张 mesh_catalog 再重建；
 * - 在 full index 尾部额外跑一次 post-pass。
 *
 * 现在的新实现只做一件事：
 * - 给当前这一个 StaticMesh 生成一条稳定目录行。
 *
 * 这样 full / incremental / live / warmup / shadow 就都能共用这一份逻辑，
 * 不会再出现“查询表一套、artifact 一套、post-pass 又一套”的重复实现。
 */

namespace MeshCatalogIndexerInternal
{
	/** 统一按小端写 1 个字节。 */
	static void WriteUInt8(TArray<uint8>& Bytes, const uint8 Value)
	{
		Bytes.Add(Value);
	}

	/** 统一按小端写 4 字节整数。 */
	static void WriteUInt32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	/** 统一按小端写 8 字节整数。 */
	static void WriteUInt64(TArray<uint8>& Bytes, const uint64 Value)
	{
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			Bytes.Add(static_cast<uint8>((Value >> (ByteIndex * 8)) & 0xff));
		}
	}

	/** 把 UTF-8 字符串写成“长度 + 正文”格式。 */
	static void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/** double 直接按 IEEE754 位模式稳定落字节。 */
	static void WriteDouble(TArray<uint8>& Bytes, const double Value)
	{
		uint64 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(double));
		WriteUInt64(Bytes, Bits);
	}

	/** 从当前位置读 1 个字节。 */
	static bool ReadUInt8(const TArray<uint8>& Bytes, int32& Offset, uint8& OutValue)
	{
		if (Offset + 1 > Bytes.Num())
		{
			return false;
		}

		OutValue = Bytes[Offset];
		++Offset;
		return true;
	}

	/** 从当前位置读 4 字节整数。 */
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

	/** 从当前位置读 8 字节整数。 */
	static bool ReadUInt64(const TArray<uint8>& Bytes, int32& Offset, uint64& OutValue)
	{
		if (Offset + 8 > Bytes.Num())
		{
			return false;
		}

		OutValue = 0;
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			OutValue |= (static_cast<uint64>(Bytes[Offset + ByteIndex]) << (ByteIndex * 8));
		}
		Offset += 8;
		return true;
	}

	/** 从当前位置按 UTF-8 协议读回字符串。 */
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

	/** 从当前位置按 IEEE754 位模式读回 double。 */
	static bool ReadDouble(const TArray<uint8>& Bytes, int32& Offset, double& OutValue)
	{
		uint64 Bits = 0;
		if (!ReadUInt64(Bytes, Offset, Bits))
		{
			return false;
		}

		FMemory::Memcpy(&OutValue, &Bits, sizeof(double));
		return true;
	}

	/** 最长轴尺寸对应的粗粒度分类。 */
	static FString ClassifySizeClass(const double BoundsMax)
	{
		if (BoundsMax < 10.0) return TEXT("tiny");
		if (BoundsMax < 50.0) return TEXT("small");
		if (BoundsMax < 200.0) return TEXT("medium");
		if (BoundsMax < 500.0) return TEXT("large");
		return TEXT("huge");
	}

	/*
	 * 从包路径推一个“够稳定、够便于筛选”的类别前缀。
	 *
	 * 规则故意保持很朴素：
	 * - `/Game/Furniture/Chair/SM_X` -> `Furniture.Chair`
	 * - `/PluginName/Props/Urban/SM_X` -> `Props.Urban`
	 * - 路径太短时就退成单段或 `Uncategorized`
	 */
	static FString InferCategoryFromPackagePath(const FString& PackagePath)
	{
		FString Folder = FPaths::GetPath(PackagePath);
		if (Folder.StartsWith(TEXT("/Game/")))
		{
			Folder.RemoveFromStart(TEXT("/Game/"));
		}
		else if (Folder.StartsWith(TEXT("/")))
		{
			Folder.RemoveFromStart(TEXT("/"));
			int32 SlashIndex = INDEX_NONE;
			if (Folder.FindChar(TEXT('/'), SlashIndex))
			{
				Folder.RightChopInline(SlashIndex + 1);
			}
		}

		TArray<FString> Parts;
		Folder.ParseIntoArray(Parts, TEXT("/"), true);
		if (Parts.Num() >= 2)
		{
			return Parts[0] + TEXT(".") + Parts[1];
		}
		if (Parts.Num() == 1)
		{
			return Parts[0];
		}
		return TEXT("Uncategorized");
	}

	/** 把目录行写成稳定 payload。 */
	static void SerializePayload(const FIndexedMeshCatalogEntry& Entry, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteString(OutBytes, Entry.AssetPath);
		WriteDouble(OutBytes, Entry.BoundsX);
		WriteDouble(OutBytes, Entry.BoundsY);
		WriteDouble(OutBytes, Entry.BoundsZ);
		WriteDouble(OutBytes, Entry.BoundsMin);
		WriteDouble(OutBytes, Entry.BoundsMid);
		WriteDouble(OutBytes, Entry.BoundsMax);
		WriteDouble(OutBytes, Entry.Volume);
		WriteString(OutBytes, Entry.SizeClass);
		WriteString(OutBytes, Entry.Category);
		WriteUInt32(OutBytes, static_cast<uint32>(Entry.TriCount));
		WriteUInt8(OutBytes, Entry.bHasCollision ? 1u : 0u);
		WriteUInt32(OutBytes, static_cast<uint32>(Entry.LodCount));
		WriteDouble(OutBytes, Entry.PivotOffsetZ);
		WriteUInt8(OutBytes, Entry.bDegenerate ? 1u : 0u);
	}

	/** 把 payload 读回目录行。 */
	static bool DeserializePayload(const TArray<uint8>& Bytes, FIndexedMeshCatalogEntry& OutEntry)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 TriCount = 0;
		uint8 bHasCollision = 0;
		uint32 LodCount = 0;
		uint8 bDegenerate = 0;
		const bool bSuccess = ReadUInt8(Bytes, Offset, Version)
			&& Version == 1
			&& ReadString(Bytes, Offset, OutEntry.AssetPath)
			&& ReadDouble(Bytes, Offset, OutEntry.BoundsX)
			&& ReadDouble(Bytes, Offset, OutEntry.BoundsY)
			&& ReadDouble(Bytes, Offset, OutEntry.BoundsZ)
			&& ReadDouble(Bytes, Offset, OutEntry.BoundsMin)
			&& ReadDouble(Bytes, Offset, OutEntry.BoundsMid)
			&& ReadDouble(Bytes, Offset, OutEntry.BoundsMax)
			&& ReadDouble(Bytes, Offset, OutEntry.Volume)
			&& ReadString(Bytes, Offset, OutEntry.SizeClass)
			&& ReadString(Bytes, Offset, OutEntry.Category)
			&& ReadUInt32(Bytes, Offset, TriCount)
			&& ReadUInt8(Bytes, Offset, bHasCollision)
			&& ReadUInt32(Bytes, Offset, LodCount)
			&& ReadDouble(Bytes, Offset, OutEntry.PivotOffsetZ)
			&& ReadUInt8(Bytes, Offset, bDegenerate);
		if (!bSuccess)
		{
			return false;
		}

		OutEntry.TriCount = static_cast<int32>(TriCount);
		OutEntry.bHasCollision = bHasCollision != 0;
		OutEntry.LodCount = static_cast<int32>(LodCount);
		OutEntry.bDegenerate = bDegenerate != 0;
		return true;
	}
}

bool FMeshCatalogIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;

	FIndexedMeshCatalogEntry Entry;
	if (!BuildPayload(AssetData, Cast<UStaticMesh>(LoadedAsset), Entry))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	MeshCatalogIndexerInternal::SerializePayload(Entry, OutArtifact.Payload);
	return OutArtifact.Payload.Num() > 0;
}

bool FMeshCatalogIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	FIndexedMeshCatalogEntry Entry;
	if (!MeshCatalogIndexerInternal::DeserializePayload(Artifact.Payload, Entry))
	{
		return false;
	}

	Entry.AssetId = AssetId;
	return DB.InsertMeshCatalogEntry(Entry) > 0;
}

bool FMeshCatalogIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	FIndexedMeshCatalogEntry Entry;
	if (!MeshCatalogIndexerInternal::DeserializePayload(Artifact.Payload, Entry))
	{
		return false;
	}

	FMonolithShadowIndexedMeshCatalogEntry ShadowEntry;
	ShadowEntry.Entry = Entry;
	ShadowEntry.Entry.AssetId = AssetId;
	ShadowEntry.RowHash = ComputeMeshCatalogRowHash(ShadowEntry.Entry);

	TArray<FMonolithShadowIndexedMeshCatalogEntry> ShadowEntries;
	ShadowEntries.Add(MoveTemp(ShadowEntry));
	return DB.ReplaceShadowMeshCatalogEntriesForAsset(CohortName, AssetId, ShadowEntries);
}

bool FMeshCatalogIndexer::BuildPayload(const FAssetData& AssetData, UStaticMesh* Mesh, FIndexedMeshCatalogEntry& OutEntry) const
{
	if (!Mesh)
	{
		return false;
	}

	FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return false;
	}

	FBoxSphereBounds Bounds = Mesh->GetBounds();
	double Axes[3] =
	{
		static_cast<double>(Bounds.BoxExtent.X) * 2.0,
		static_cast<double>(Bounds.BoxExtent.Y) * 2.0,
		static_cast<double>(Bounds.BoxExtent.Z) * 2.0
	};

	if (Axes[0] > Axes[1]) Swap(Axes[0], Axes[1]);
	if (Axes[1] > Axes[2]) Swap(Axes[1], Axes[2]);
	if (Axes[0] > Axes[1]) Swap(Axes[0], Axes[1]);

	OutEntry = FIndexedMeshCatalogEntry();
	OutEntry.AssetPath = AssetData.GetObjectPathString();
	OutEntry.BoundsX = static_cast<double>(Bounds.BoxExtent.X) * 2.0;
	OutEntry.BoundsY = static_cast<double>(Bounds.BoxExtent.Y) * 2.0;
	OutEntry.BoundsZ = static_cast<double>(Bounds.BoxExtent.Z) * 2.0;
	OutEntry.BoundsMin = Axes[0];
	OutEntry.BoundsMid = Axes[1];
	OutEntry.BoundsMax = Axes[2];
	OutEntry.Volume = OutEntry.BoundsX * OutEntry.BoundsY * OutEntry.BoundsZ;
	OutEntry.SizeClass = MeshCatalogIndexerInternal::ClassifySizeClass(OutEntry.BoundsMax);
	OutEntry.Category = MeshCatalogIndexerInternal::InferCategoryFromPackagePath(AssetData.PackageName.ToString());
	OutEntry.TriCount = RenderData->LODResources[0].GetNumTriangles();
	OutEntry.bHasCollision = Mesh->GetBodySetup() != nullptr;
	OutEntry.LodCount = Mesh->GetNumLODs();
	OutEntry.PivotOffsetZ = 0.0 - static_cast<double>(Bounds.Origin.Z - Bounds.BoxExtent.Z);
	OutEntry.bDegenerate = OutEntry.BoundsMin < 1.0;
	return true;
}
