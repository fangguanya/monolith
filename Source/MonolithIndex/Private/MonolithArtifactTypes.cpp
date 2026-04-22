#include "MonolithArtifactTypes.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/EngineVersion.h"
#include "Misc/SecureHash.h"
#include "Misc/StringBuilder.h"
#include "MonolithSettings.h"
#include "Serialization/MemoryWriter.h"

/*
 * 这里放的是 artifact identity 的“底层拼装工厂”。
 *
 * 大方向只有两件事：
 * 1. 把 identity 按稳定顺序序列化，保证同样输入一定得到同样字节；
 * 2. 用 SavedHash 或 AR 快照生成 package fingerprint。
 *
 * “稳定顺序”特别重要，因为缓存 key 依赖哈希。
 * 如果字段顺序一会儿变这样、一会儿变那样，就会出现内容没变但 hash 变了的假失效。
 */
namespace MonolithArtifactTypesInternal
{
	/** 以小端顺序写入 1 字节整数。 */
	static void WriteUInt8(TArray<uint8>& Bytes, uint8 Value)
	{
		Bytes.Add(Value);
	}

	/** 以小端顺序写入 2 字节整数。 */
	static void WriteUInt16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	}

	/** 以小端顺序写入 4 字节整数。 */
	static void WriteUInt32(TArray<uint8>& Bytes, uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	/** 先写 UTF-8 长度，再写真正的字符串内容。 */
	static void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/** FName 统一转成字符串后写入，避免依赖进程内 name table。 */
	static void WriteName(TArray<uint8>& Bytes, const FName Value)
	{
		WriteString(Bytes, Value.ToString());
	}

	/** 指纹内部格式版本，未来改拼装规则时可以升级。 */
	static uint32 GetFingerprintSchemaVersion()
	{
		return 1;
	}

	/*
	 * dependency version 先排序再写入。
	 *
	 * 这样即使调用方传入顺序不一样，只要内容相同，最终 identity 也会相同。
	 */
	static TArray<FMonolithDependencyVersion> GetSortedDependencyVersions(const TArray<FMonolithDependencyVersion>& InVersions)
	{
		TArray<FMonolithDependencyVersion> Versions = InVersions;
		Versions.Sort([](const FMonolithDependencyVersion& A, const FMonolithDependencyVersion& B)
		{
			const FString AId = A.Id.ToString();
			const FString BId = B.Id.ToString();
			return AId == BId ? A.Version < B.Version : AId < BId;
		});
		return Versions;
	}

	/*
	 * ARSnapshot 指纹的思路是：
	 * - 读取当前资产的 tag；
	 * - 读取硬依赖和软依赖；
	 * - 按稳定顺序拼成字节；
	 * - 最后做 SHA1。
	 *
	 * 这样就算没有 package saved hash，也能得到一个较稳定的“内容快照”。
	 */
	static FString BuildArSnapshotFingerprint(const FAssetData& AssetData, IAssetRegistry& AssetRegistry)
	{
		TArray<FString> TagPairs;
		AssetData.EnumerateTags([&TagPairs](TPair<FName, FAssetTagValueRef> Pair)
		{
			TagPairs.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key.ToString(), *Pair.Value.AsString()));
		});
		TagPairs.Sort();

		FAssetRegistryDependencyOptions HardOptions;
		HardOptions.bIncludeSoftPackageReferences = false;
		HardOptions.bIncludeHardPackageReferences = true;
		HardOptions.bIncludeGamePackageReferences = true;
		HardOptions.bIncludeEditorOnlyPackageReferences = true;
		HardOptions.bIncludeSearchableNames = false;
		HardOptions.bIncludeSoftManagementReferences = false;
		HardOptions.bIncludeHardManagementReferences = false;

		FAssetRegistryDependencyOptions SoftOptions;
		SoftOptions.bIncludeSoftPackageReferences = true;
		SoftOptions.bIncludeHardPackageReferences = false;
		SoftOptions.bIncludeGamePackageReferences = true;
		SoftOptions.bIncludeEditorOnlyPackageReferences = true;
		SoftOptions.bIncludeSearchableNames = false;
		SoftOptions.bIncludeSoftManagementReferences = false;
		SoftOptions.bIncludeHardManagementReferences = false;

		TArray<FName> HardDependencies;
		TArray<FName> SoftDependencies;
		AssetRegistry.K2_GetDependencies(AssetData.PackageName, HardOptions, HardDependencies);
		AssetRegistry.K2_GetDependencies(AssetData.PackageName, SoftOptions, SoftDependencies);

		HardDependencies.Sort(FNameLexicalLess());
		SoftDependencies.Sort(FNameLexicalLess());

		TArray<uint8> Bytes;
		// 先写 schema，再写 tag / hard deps / soft deps，确保拼装顺序固定。
		WriteUInt32(Bytes, GetFingerprintSchemaVersion());
		WriteUInt32(Bytes, static_cast<uint32>(TagPairs.Num()));
		for (const FString& TagPair : TagPairs)
		{
			WriteString(Bytes, TagPair);
		}
		WriteUInt32(Bytes, static_cast<uint32>(HardDependencies.Num()));
		for (const FName Dependency : HardDependencies)
		{
			WriteName(Bytes, Dependency);
		}
		WriteUInt32(Bytes, static_cast<uint32>(SoftDependencies.Num()));
		for (const FName Dependency : SoftDependencies)
		{
			WriteName(Bytes, Dependency);
		}

		const FSHAHash Fingerprint = FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num());
		return Fingerprint.ToString();
	}
}

FString LexToString(const EMonolithIdentityProvider Provider)
{
	// 统一从这里出字符串，避免项目里写出很多不一致的枚举名字。
	switch (Provider)
	{
	case EMonolithIdentityProvider::SavedHash:
		return TEXT("SavedHash");
	case EMonolithIdentityProvider::ARSnapshotV1:
		return TEXT("ARSnapshotV1");
	case EMonolithIdentityProvider::ManifestV1:
		return TEXT("ManifestV1");
	default:
		return TEXT("SavedHash");
	}
}

bool TryParseMonolithIdentityProvider(const FString& Text, EMonolithIdentityProvider& OutProvider)
{
	// 解析时允许忽略大小写，命令行和 ini 写起来更宽容。
	if (Text.Equals(TEXT("SavedHash"), ESearchCase::IgnoreCase))
	{
		OutProvider = EMonolithIdentityProvider::SavedHash;
		return true;
	}
	if (Text.Equals(TEXT("ARSnapshotV1"), ESearchCase::IgnoreCase))
	{
		OutProvider = EMonolithIdentityProvider::ARSnapshotV1;
		return true;
	}
	if (Text.Equals(TEXT("ManifestV1"), ESearchCase::IgnoreCase))
	{
		OutProvider = EMonolithIdentityProvider::ManifestV1;
		return true;
	}
	return false;
}

EMonolithIdentityProvider GetConfiguredMonolithIdentityProvider()
{
	// 这里把配置读取和默认值兜底放一起，调用方不用重复判空。
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	EMonolithIdentityProvider Provider = EMonolithIdentityProvider::SavedHash;
	if (Settings && TryParseMonolithIdentityProvider(Settings->IndexIdentityProvider, Provider))
	{
		return Provider;
	}
	return EMonolithIdentityProvider::SavedHash;
}

FString LexToString(const EMonolithExecutionMode Mode)
{
	// 这个字符串主要给日志、诊断和调试面板使用。
	switch (Mode)
	{
	case EMonolithExecutionMode::AROnly:
		return TEXT("AROnly");
	case EMonolithExecutionMode::PackageScopedLoad:
		return TEXT("PackageScopedLoad");
	case EMonolithExecutionMode::GlobalReducer:
		return TEXT("GlobalReducer");
	case EMonolithExecutionMode::OfflineOnly:
		return TEXT("OfflineOnly");
	default:
		return TEXT("PackageScopedLoad");
	}
}

void SerializeMonolithArtifactIdentity(const FMonolithArtifactIdentityV1& Identity, TArray<uint8>& OutBytes)
{
	// 每次都从空数组开始，避免上一次残留字节污染结果。
	OutBytes.Reset();

	// 字段顺序就是 identity 的协议顺序，后续改动要非常谨慎。
	MonolithArtifactTypesInternal::WriteUInt8(OutBytes, Identity.IdentitySchemaVersion);
	MonolithArtifactTypesInternal::WriteName(OutBytes, Identity.IndexerId);
	MonolithArtifactTypesInternal::WriteUInt32(OutBytes, Identity.IndexerVersion);
	MonolithArtifactTypesInternal::WriteUInt8(OutBytes, Identity.ArtifactSchemaVersion);
	MonolithArtifactTypesInternal::WriteName(OutBytes, Identity.PackageName);
	MonolithArtifactTypesInternal::WriteUInt8(OutBytes, static_cast<uint8>(Identity.IdentityProvider));
	MonolithArtifactTypesInternal::WriteString(OutBytes, Identity.PackageFingerprint);
	MonolithArtifactTypesInternal::WriteUInt16(OutBytes, Identity.EngineMajorVersion);
	MonolithArtifactTypesInternal::WriteUInt16(OutBytes, Identity.EngineMinorVersion);

	// 依赖版本列表先排序，再写入稳定字节流。
	const TArray<FMonolithDependencyVersion> SortedVersions =
		MonolithArtifactTypesInternal::GetSortedDependencyVersions(Identity.DependencyVersions);
	MonolithArtifactTypesInternal::WriteUInt32(OutBytes, static_cast<uint32>(SortedVersions.Num()));
	for (const FMonolithDependencyVersion& Version : SortedVersions)
	{
		MonolithArtifactTypesInternal::WriteName(OutBytes, Version.Id);
		MonolithArtifactTypesInternal::WriteUInt32(OutBytes, Version.Version);
	}
}

TArray<uint8> SerializeMonolithArtifactIdentity(const FMonolithArtifactIdentityV1& Identity)
{
	// 这个重载只是为了让调用方在“不想自己准备数组”时写起来更顺手。
	TArray<uint8> Bytes;
	SerializeMonolithArtifactIdentity(Identity, Bytes);
	return Bytes;
}

FIoHash HashMonolithArtifactIdentity(const FMonolithArtifactIdentityV1& Identity)
{
	// 先得到稳定字节，再统一走 IoHash，这样不同 provider 也能共用同一套 key 逻辑。
	TArray<uint8> Bytes;
	SerializeMonolithArtifactIdentity(Identity, Bytes);
	return FIoHash::HashBuffer(Bytes.GetData(), Bytes.Num());
}

bool BuildMonolithSavedHashIdentity(
	const FAssetData& AssetData,
	IAssetRegistry& AssetRegistry,
	const FName IndexerId,
	const uint32 IndexerVersion,
	const uint8 ArtifactSchemaVersion,
	const TArray<FMonolithDependencyVersion>& DependencyVersions,
	FMonolithArtifactIdentityV1& OutIdentity)
{
	// SavedHash 模式优先使用包保存时的 hash；如果拿不到，就会退成空 hash 字符串。
	const TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(AssetData.PackageName);
	const FIoHash SavedHash = PackageData.IsSet() ? PackageData->GetPackageSavedHash() : FIoHash();

	OutIdentity = FMonolithArtifactIdentityV1();
	OutIdentity.IndexerId = IndexerId;
	OutIdentity.IndexerVersion = IndexerVersion;
	OutIdentity.ArtifactSchemaVersion = ArtifactSchemaVersion;
	OutIdentity.PackageName = AssetData.PackageName;
	OutIdentity.IdentityProvider = EMonolithIdentityProvider::SavedHash;
	OutIdentity.PackageFingerprint = LexToString(SavedHash);
	OutIdentity.EngineMajorVersion = static_cast<uint16>(FEngineVersion::Current().GetMajor());
	OutIdentity.EngineMinorVersion = static_cast<uint16>(FEngineVersion::Current().GetMinor());
	OutIdentity.DependencyVersions = DependencyVersions;
	return true;
}

bool BuildMonolithArSnapshotIdentity(
	const FAssetData& AssetData,
	IAssetRegistry& AssetRegistry,
	const FName IndexerId,
	const uint32 IndexerVersion,
	const uint8 ArtifactSchemaVersion,
	const TArray<FMonolithDependencyVersion>& DependencyVersions,
	FMonolithArtifactIdentityV1& OutIdentity)
{
	// ARSnapshot 模式不依赖 package saved hash，而是自己拼一份快照指纹。
	OutIdentity = FMonolithArtifactIdentityV1();
	OutIdentity.IndexerId = IndexerId;
	OutIdentity.IndexerVersion = IndexerVersion;
	OutIdentity.ArtifactSchemaVersion = ArtifactSchemaVersion;
	OutIdentity.PackageName = AssetData.PackageName;
	OutIdentity.IdentityProvider = EMonolithIdentityProvider::ARSnapshotV1;
	OutIdentity.PackageFingerprint = MonolithArtifactTypesInternal::BuildArSnapshotFingerprint(AssetData, AssetRegistry);
	OutIdentity.EngineMajorVersion = static_cast<uint16>(FEngineVersion::Current().GetMajor());
	OutIdentity.EngineMinorVersion = static_cast<uint16>(FEngineVersion::Current().GetMinor());
	OutIdentity.DependencyVersions = DependencyVersions;
	return true;
}

bool BuildConfiguredMonolithArtifactIdentity(
	const FAssetData& AssetData,
	IAssetRegistry& AssetRegistry,
	const FName IndexerId,
	const uint32 IndexerVersion,
	const uint8 ArtifactSchemaVersion,
	const TArray<FMonolithDependencyVersion>& DependencyVersions,
	FMonolithArtifactIdentityV1& OutIdentity)
{
	// 统一由配置决定走哪条路，调用方不用自己写 switch。
	switch (GetConfiguredMonolithIdentityProvider())
	{
	case EMonolithIdentityProvider::SavedHash:
		return BuildMonolithSavedHashIdentity(
			AssetData,
			AssetRegistry,
			IndexerId,
			IndexerVersion,
			ArtifactSchemaVersion,
			DependencyVersions,
			OutIdentity);
	case EMonolithIdentityProvider::ARSnapshotV1:
		return BuildMonolithArSnapshotIdentity(
			AssetData,
			AssetRegistry,
			IndexerId,
			IndexerVersion,
			ArtifactSchemaVersion,
			DependencyVersions,
			OutIdentity);
	default:
		return false;
	}
}
