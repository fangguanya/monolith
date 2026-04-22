#pragma once

#include "CoreMinimal.h"
#include "IO/IoHash.h"

class IAssetRegistry;
struct FAssetData;

/*
 * 这份头文件定义的是 Monolith artifact 体系里最核心的“名词”。
 *
 * 可以把一份 artifact 想成：
 * - “这次索引算出来的结果包”。
 *
 * 而 artifact identity 则像：
 * - “这个结果包对应的身份证”。
 *
 * 只要身份证里的关键信息没有变化，我们就认为旧结果还能复用；
 * 一旦身份证变了，就说明这份结果可能已经过期，需要重新生成。
 */

/** 指定“身份证指纹”是用什么方式算出来的。 */
enum class EMonolithIdentityProvider : uint8
{
	/** 直接使用包保存时的 hash，速度快，适合大多数本地场景。 */
	SavedHash,
	/** 使用 Asset Registry 快照信息拼出指纹，更适合不依赖磁盘 saved hash 的场景。 */
	ARSnapshotV1,
	/** 使用一份显式 reducer manifest 指纹。
	 * 这类 identity 不对应单个包，而对应 Config / Cpp / reducer 之类的全局快照。 */
	ManifestV1,
};

/** 指定 indexer 运行时大概要走哪种执行模式。 */
enum class EMonolithExecutionMode : uint8
{
	/** 只读 Asset Registry，不真正加载资产。 */
	AROnly,
	/** 按包加载资产后，在包级别完成索引。 */
	PackageScopedLoad,
	/** 先收集局部结果，后面再交给全局 reducer 汇总。 */
	GlobalReducer,
	/** 只能在离线命令或后台 warmup 里跑，不适合交互时机。 */
	OfflineOnly,
};

/** 记录某个依赖模块自己的版本号，方便把“规则版本”也编进身份证。 */
struct FMonolithDependencyVersion
{
	/** 依赖项的名字，比如某个 indexer 或某个辅助规则集。 */
	FName Id;
	/** 该依赖项当前的版本号。 */
	uint32 Version = 0;
};

/*
 * artifact identity 是“这份结果为什么还能复用”的完整说明。
 *
 * 它里面既有：
 * - 资产是谁；
 * - 哪个 indexer 生成的；
 * - indexer 和 schema 版本是多少；
 * - 当前使用了哪种指纹算法；
 * - 以及引擎版本、依赖版本这些会影响结果的上下文。
 */
struct FMonolithArtifactIdentityV1
{
	/** identity 自己的结构版本，方便将来升级序列化格式。 */
	uint8 IdentitySchemaVersion = 1;
	/** 由哪个 indexer 生成。 */
	FName IndexerId;
	/** indexer 逻辑版本，逻辑一改就应当升级。 */
	uint32 IndexerVersion = 1;
	/** artifact payload 的格式版本。 */
	uint8 ArtifactSchemaVersion = 1;
	/** 这份结果属于哪个包。 */
	FName PackageName;
	/** 当前用哪种 provider 来生成指纹。 */
	EMonolithIdentityProvider IdentityProvider = EMonolithIdentityProvider::SavedHash;
	/** 真正参与比较的指纹字符串。 */
	FString PackageFingerprint;
	/** 构建该 identity 时的主版本号。 */
	uint16 EngineMajorVersion = 0;
	/** 构建该 identity 时的次版本号。 */
	uint16 EngineMinorVersion = 0;
	/** 参与 identity 的附加依赖版本列表。 */
	TArray<FMonolithDependencyVersion> DependencyVersions;
};

/*
 * artifact 是真正要缓存和回放的结果包。
 *
 * 其中 Payload 是二进制内容。
 * 不同 indexer 会约定自己的 payload 格式，然后在 materialize 阶段再把它还原成数据库行。
 */
struct FMonolithArtifact
{
	/** payload 本身的结构版本。 */
	uint8 ArtifactSchemaVersion = 1;
	/** 由哪个 indexer 生成。 */
	FName IndexerId;
	/** 生成它时 indexer 的版本。 */
	uint32 IndexerVersion = 1;
	/** 这份 artifact 设计给哪种执行模式使用。 */
	EMonolithExecutionMode ExecutionMode = EMonolithExecutionMode::PackageScopedLoad;
	/** 属于哪个包，便于日志和排查。 */
	FString PackageName;
	/** identity 序列化后再哈希得到的稳定 key。 */
	FIoHash IdentityHash;
	/** 真正的二进制内容。 */
	TArray<uint8> Payload;
};

/** 把 identity provider 枚举转成日志可读字符串。 */
MONOLITHINDEX_API FString LexToString(EMonolithIdentityProvider Provider);
/** 从配置字符串里反解 provider。 */
MONOLITHINDEX_API bool TryParseMonolithIdentityProvider(const FString& Text, EMonolithIdentityProvider& OutProvider);
/** 读取项目配置里当前启用的 provider。 */
MONOLITHINDEX_API EMonolithIdentityProvider GetConfiguredMonolithIdentityProvider();

/** 把执行模式枚举转成日志可读字符串。 */
MONOLITHINDEX_API FString LexToString(EMonolithExecutionMode Mode);

/** 把 identity 以稳定顺序写成字节数组。 */
MONOLITHINDEX_API void SerializeMonolithArtifactIdentity(const FMonolithArtifactIdentityV1& Identity, TArray<uint8>& OutBytes);
/** 返回新建好的 identity 字节数组。 */
MONOLITHINDEX_API TArray<uint8> SerializeMonolithArtifactIdentity(const FMonolithArtifactIdentityV1& Identity);
/** 对 identity 字节做哈希，得到缓存 key 会用到的稳定摘要。 */
MONOLITHINDEX_API FIoHash HashMonolithArtifactIdentity(const FMonolithArtifactIdentityV1& Identity);

/** 用包保存 hash 构建 identity。 */
MONOLITHINDEX_API bool BuildMonolithSavedHashIdentity(
	const FAssetData& AssetData,
	IAssetRegistry& AssetRegistry,
	FName IndexerId,
	uint32 IndexerVersion,
	uint8 ArtifactSchemaVersion,
	const TArray<FMonolithDependencyVersion>& DependencyVersions,
	FMonolithArtifactIdentityV1& OutIdentity);

/** 用 Asset Registry 快照信息构建 identity。 */
MONOLITHINDEX_API bool BuildMonolithArSnapshotIdentity(
	const FAssetData& AssetData,
	IAssetRegistry& AssetRegistry,
	FName IndexerId,
	uint32 IndexerVersion,
	uint8 ArtifactSchemaVersion,
	const TArray<FMonolithDependencyVersion>& DependencyVersions,
	FMonolithArtifactIdentityV1& OutIdentity);

/** 按当前配置自动选择 SavedHash 或 ARSnapshot 的实现。 */
MONOLITHINDEX_API bool BuildConfiguredMonolithArtifactIdentity(
	const FAssetData& AssetData,
	IAssetRegistry& AssetRegistry,
	FName IndexerId,
	uint32 IndexerVersion,
	uint8 ArtifactSchemaVersion,
	const TArray<FMonolithDependencyVersion>& DependencyVersions,
	FMonolithArtifactIdentityV1& OutIdentity);
