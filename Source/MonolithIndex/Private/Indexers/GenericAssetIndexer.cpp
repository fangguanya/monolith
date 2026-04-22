#include "Indexers/GenericAssetIndexer.h"
#include "MonolithIndexerShadowMode.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundCue.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

/*
 * GenericAssetIndexer 负责给“暂时没有专用 indexer 的普通资产”提供一个轻量 metadata 索引。
 *
 * 它不会深入解析复杂图结构，只会抽出一些最容易理解、最常用的描述信息，
 * 比如：
 * - 资源名字
 * - 资源类型
 * - 三角面数、尺寸、采样率之类的关键属性
 *
 * 这样做的好处是：
 * - 即使没有专门 indexer，也至少能在搜索和详情里看到一些有价值的信息；
 * - 同时还能复用 artifact / shadow 迁移链路。
 */
namespace GenericAssetIndexerInternal
{
	/** 写进 artifact payload 的轻量元数据。 */
	struct FMetadataPayload
	{
		/** 展示给用户看的名字。 */
		FString NodeName;
		/** 资产真正的类名。 */
		FString NodeClass;
		/** 额外属性，统一塞成一段 JSON 文本。 */
		FString Properties;
	};

	/** 小端写入 4 字节整数。 */
	static void WriteUInt32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	/** 先写长度，再写 UTF-8 字符串正文。 */
	static void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/** 从 payload 里按同样协议读回字符串。 */
	static bool ReadString(const TArray<uint8>& Bytes, int32& Offset, FString& OutString)
	{
		if (Offset + 4 > Bytes.Num())
		{
			return false;
		}

		const uint32 Length =
			static_cast<uint32>(Bytes[Offset]) |
			(static_cast<uint32>(Bytes[Offset + 1]) << 8) |
			(static_cast<uint32>(Bytes[Offset + 2]) << 16) |
			(static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4;

		if (Length > static_cast<uint32>(Bytes.Num() - Offset))
		{
			return false;
		}

		FUTF8ToTCHAR Convert(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset), static_cast<int32>(Length));
		OutString = FString(Convert.Length(), Convert.Get());
		Offset += static_cast<int32>(Length);
		return true;
	}

	/*
	 * 这里是真正的“资产观察器”。
	 *
	 * 它会根据不同资产类型，挑一些最容易理解的属性写进 JSON。
	 * 这些属性不是为了完全还原资产，而是为了提供搜索和快速浏览价值。
	 */
	static bool BuildMetadataPayload(UObject* LoadedAsset, FMetadataPayload& OutPayload)
	{
		if (!LoadedAsset)
		{
			return false;
		}

		OutPayload.NodeName = LoadedAsset->GetName();
		OutPayload.NodeClass = LoadedAsset->GetClass()->GetName();

		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

		if (UStaticMesh* SM = Cast<UStaticMesh>(LoadedAsset))
		{
			// 静态网格最常用的信息是面数、点数、LOD 和材质槽。
			if (SM->GetRenderData() && SM->GetRenderData()->LODResources.Num() > 0)
			{
				const FStaticMeshLODResources& LOD0 = SM->GetRenderData()->LODResources[0];
				Props->SetNumberField(TEXT("triangles"), LOD0.GetNumTriangles());
				Props->SetNumberField(TEXT("vertices"), LOD0.GetNumVertices());
				Props->SetNumberField(TEXT("sections"), LOD0.Sections.Num());
			}
			Props->SetNumberField(TEXT("lod_count"), SM->GetNumLODs());
			Props->SetNumberField(TEXT("material_slots"), SM->GetStaticMaterials().Num());

			FBoxSphereBounds Bounds = SM->GetBounds();
			Props->SetStringField(TEXT("bounds_extent"),
				FString::Printf(TEXT("%.1f x %.1f x %.1f"),
					Bounds.BoxExtent.X * 2, Bounds.BoxExtent.Y * 2, Bounds.BoxExtent.Z * 2));

			Props->SetBoolField(TEXT("has_collision"), SM->GetBodySetup() != nullptr);
		}
		else if (USkeletalMesh* SK = Cast<USkeletalMesh>(LoadedAsset))
		{
			// 骨骼网格更关注 skeleton、骨骼数和 physics asset。
			Props->SetNumberField(TEXT("lod_count"), SK->GetLODNum());
			Props->SetNumberField(TEXT("material_slots"), SK->GetMaterials().Num());

			if (SK->GetSkeleton())
			{
				Props->SetNumberField(TEXT("bone_count"), SK->GetSkeleton()->GetReferenceSkeleton().GetNum());
				Props->SetStringField(TEXT("skeleton"), SK->GetSkeleton()->GetPathName());
			}

			if (SK->GetPhysicsAsset())
			{
				Props->SetStringField(TEXT("physics_asset"), SK->GetPhysicsAsset()->GetPathName());
			}
		}
		else if (UTexture2D* Tex = Cast<UTexture2D>(LoadedAsset))
		{
			// 贴图类重点记录尺寸、像素格式、mip 和采样相关属性。
			Props->SetNumberField(TEXT("width"), Tex->GetSizeX());
			Props->SetNumberField(TEXT("height"), Tex->GetSizeY());
			Props->SetStringField(TEXT("format"), GPixelFormats[Tex->GetPixelFormat()].Name);
			Props->SetNumberField(TEXT("mip_count"), Tex->GetNumMips());
			Props->SetBoolField(TEXT("srgb"), Tex->SRGB);
			Props->SetBoolField(TEXT("has_alpha"), Tex->HasAlphaChannel());
			Props->SetStringField(TEXT("compression"),
				UEnum::GetValueAsString(Tex->CompressionSettings));
			Props->SetStringField(TEXT("lod_group"),
				UEnum::GetValueAsString(Tex->LODGroup));
			Props->SetStringField(TEXT("filter"),
				UEnum::GetValueAsString(Tex->Filter));
			Props->SetStringField(TEXT("address_x"),
				UEnum::GetValueAsString(Tex->GetTextureAddressX()));
			Props->SetStringField(TEXT("address_y"),
				UEnum::GetValueAsString(Tex->GetTextureAddressY()));
#if WITH_EDITORONLY_DATA
			Props->SetBoolField(TEXT("virtual_texture_streaming"), Tex->VirtualTextureStreaming != 0);
			Props->SetBoolField(TEXT("compression_no_alpha"), Tex->CompressionNoAlpha != 0);
#endif
			EMaterialSamplerType SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Tex);
			UEnum* SamplerEnum = StaticEnum<EMaterialSamplerType>();
			if (SamplerEnum)
			{
				Props->SetStringField(TEXT("recommended_sampler_type"),
					SamplerEnum->GetNameStringByValue(static_cast<int64>(SamplerType)));
			}
		}
		else if (USoundWave* Sound = Cast<USoundWave>(LoadedAsset))
		{
			// 声音资产最直观的就是时长、采样率和声道数。
			Props->SetNumberField(TEXT("duration"), Sound->Duration);
			Props->SetNumberField(TEXT("sample_rate"), Sound->GetSampleRateForCurrentPlatform());
			Props->SetNumberField(TEXT("channels"), Sound->NumChannels);
			Props->SetBoolField(TEXT("looping"), Sound->bLooping);
		}

		// 最终统一压成一段紧凑 JSON，方便存进数据库和 artifact payload。
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutPayload.Properties);
		return FJsonSerializer::Serialize(Props, *Writer, true);
	}

	/** 把 metadata payload 按稳定协议写成字节。 */
	static void SerializePayload(const FMetadataPayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		OutBytes.Add(1);
		WriteString(OutBytes, Payload.NodeName);
		WriteString(OutBytes, Payload.NodeClass);
		WriteString(OutBytes, Payload.Properties);
	}

	/** 从 artifact payload 中还原 metadata。 */
	static bool DeserializePayload(const TArray<uint8>& Bytes, FMetadataPayload& OutPayload)
	{
		if (Bytes.Num() < 1 || Bytes[0] != 1)
		{
			return false;
		}

		int32 Offset = 1;
		return ReadString(Bytes, Offset, OutPayload.NodeName)
			&& ReadString(Bytes, Offset, OutPayload.NodeClass)
			&& ReadString(Bytes, Offset, OutPayload.Properties);
	}

	/** 把轻量 metadata 包装成数据库里的一个 Metadata 节点。 */
	static FIndexedNode MakeMetadataNode(const FMetadataPayload& Payload, const int64 AssetId)
	{
		FIndexedNode MetaNode;
		MetaNode.AssetId = AssetId;
		MetaNode.NodeType = TEXT("Metadata");
		MetaNode.NodeName = Payload.NodeName;
		MetaNode.NodeClass = Payload.NodeClass;
		MetaNode.Properties = Payload.Properties;
		return MetaNode;
	}

	/** 真正落库时只需要插入这一行 metadata node。 */
	static bool MaterializeMetadataNode(const FMetadataPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		return DB.InsertNode(MakeMetadataNode(Payload, AssetId)) > 0;
	}
}

bool FGenericAssetIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	// artifact 路径：先抽 metadata，再序列化成 payload。
	GenericAssetIndexerInternal::FMetadataPayload Payload;
	if (!GenericAssetIndexerInternal::BuildMetadataPayload(LoadedAsset, Payload))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	GenericAssetIndexerInternal::SerializePayload(Payload, OutArtifact.Payload);
	return true;
}

bool FGenericAssetIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	// 从缓存命中的 artifact 恢复出 metadata，并落回生产表。
	GenericAssetIndexerInternal::FMetadataPayload Payload;
	if (!GenericAssetIndexerInternal::DeserializePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return GenericAssetIndexerInternal::MaterializeMetadataNode(Payload, DB, AssetId);
}

bool FGenericAssetIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, const int64 AssetId, const FString& CohortName)
{
	// shadow 路径不会碰生产表，而是改写成 shadow row 供 diff 比较。
	GenericAssetIndexerInternal::FMetadataPayload Payload;
	if (!GenericAssetIndexerInternal::DeserializePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	FMonolithShadowIndexedNode ShadowNode;
	ShadowNode.Node = GenericAssetIndexerInternal::MakeMetadataNode(Payload, AssetId);
	ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);

	TArray<FMonolithShadowIndexedNode> ShadowNodes;
	ShadowNodes.Add(MoveTemp(ShadowNode));
	return DB.ReplaceShadowNodesForAsset(CohortName, AssetId, ShadowNodes);
}
