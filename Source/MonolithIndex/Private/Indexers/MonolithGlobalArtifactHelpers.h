#pragma once

#include "Algo/Unique.h"
#include "CoreMinimal.h"
#include "MonolithArtifactTypes.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexer.h"
#include "MonolithIndexLog.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Misc/SecureHash.h"

/*
 * 这份 helper 专门服务“全局型 artifact”。
 *
 * Config / Cpp 这类 indexer 的共同点是：
 * - 它们不是处理一个包，而是扫描一整批文件；
 * - 它们仍然需要稳定 identity，才能让 DDC 知道“这份全局快照有没有变”。
 *
 * 所以这里统一提供三件基础能力：
 * 1. 把磁盘文件转换成稳定的逻辑路径；
 * 2. 按稳定顺序收集输入文件；
 * 3. 把“逻辑路径 + 文件内容 hash”组合成 manifest 指纹，再拼出 identity。
 *
 * 这样 Config 和 Cpp 就不用各自复制一整套 manifest 规则。
 */
namespace MonolithGlobalArtifactHelpers
{
	/** 一份参与全局 artifact 的输入文件。 */
	struct FInputFile
	{
		/** 文件在磁盘上的真实绝对路径。 */
		FString AbsolutePath;
		/** 跨机器更稳定的逻辑路径。 */
		FString LogicalPath;
	};

	/** 往字节数组里写入一个 uint32。 */
	inline void WriteUInt32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	/** 以 UTF-8 + 长度前缀的形式写字符串。 */
	inline void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/** 把路径正规化，避免分隔符和相对路径噪声。 */
	inline FString NormalizeAbsolutePath(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Result);
		return Result;
	}

	/*
	 * 把磁盘绝对路径转换成“更适合进 identity”的逻辑路径。
	 *
	 * 规则故意不用“完整绝对路径”，因为那会把：
	 * - 机器盘符；
	 * - 安装目录差异；
	 * - 用户名目录
	 * 全都带进缓存 key。
	 *
	 * 这里统一变成：
	 * - `Project/...`
	 * - `Engine/...`
	 * - 其它极少数落不到这两类里的文件，才退回完整路径。
	 */
	inline FString MakeStableLogicalPath(const FString& AbsolutePath)
	{
		const FString NormalizedPath = NormalizeAbsolutePath(AbsolutePath);
		const FString ProjectRoot = NormalizeAbsolutePath(FPaths::ProjectDir());
		const FString EngineRoot = NormalizeAbsolutePath(FPaths::EngineDir());

		FString RelativePath = NormalizedPath;
		if (RelativePath.StartsWith(ProjectRoot))
		{
			FPaths::MakePathRelativeTo(RelativePath, *ProjectRoot);
			FPaths::NormalizeFilename(RelativePath);
			return FString::Printf(TEXT("Project/%s"), *RelativePath);
		}

		RelativePath = NormalizedPath;
		if (RelativePath.StartsWith(EngineRoot))
		{
			FPaths::MakePathRelativeTo(RelativePath, *EngineRoot);
			FPaths::NormalizeFilename(RelativePath);
			return FString::Printf(TEXT("Engine/%s"), *RelativePath);
		}

		return NormalizedPath;
	}

	/** 递归追加一批文件到输入列表。 */
	inline void AppendFilesRecursive(TArray<FInputFile>& OutFiles, const FString& Directory, const FString& Pattern)
	{
		if (!IFileManager::Get().DirectoryExists(*Directory))
		{
			return;
		}

		TArray<FString> FoundFiles;
		IFileManager::Get().FindFilesRecursive(FoundFiles, *Directory, *Pattern, true, false);
		for (const FString& FilePath : FoundFiles)
		{
			FInputFile InputFile;
			InputFile.AbsolutePath = NormalizeAbsolutePath(FilePath);
			InputFile.LogicalPath = MakeStableLogicalPath(InputFile.AbsolutePath);
			OutFiles.Add(MoveTemp(InputFile));
		}
	}

	/** 统一排序并按逻辑路径去重。 */
	inline void SortAndUniqueInputFiles(TArray<FInputFile>& InOutFiles)
	{
		InOutFiles.Sort([](const FInputFile& A, const FInputFile& B)
		{
			return A.LogicalPath == B.LogicalPath ? A.AbsolutePath < B.AbsolutePath : A.LogicalPath < B.LogicalPath;
		});

		InOutFiles.SetNum(Algo::UniqueBy(InOutFiles, [](const FInputFile& File)
		{
			return File.LogicalPath;
		}));
	}

	/*
	 * 把输入文件集合压成一个稳定 manifest 指纹。
	 *
	 * 规则是：
	 * - 先按逻辑路径排序；
	 * - 对每个文件计算内容 hash；
	 * - 再把“逻辑路径 + 内容 hash”整体再哈希一次。
	 *
	 * 这样只要任意一个输入文件改了内容或改了路径，最终指纹就会变化。
	 */
	inline bool BuildInputFileManifestFingerprint(const TArray<FInputFile>& InputFiles, FString& OutFingerprint)
	{
		TArray<uint8> ManifestBytes;
		WriteUInt32(ManifestBytes, static_cast<uint32>(InputFiles.Num()));

		static const uint8 EmptyByte = 0;
		for (const FInputFile& InputFile : InputFiles)
		{
			TArray<uint8> FileBytes;
			if (!FFileHelper::LoadFileToArray(FileBytes, *InputFile.AbsolutePath))
			{
				return false;
			}

			const void* DataPtr = FileBytes.Num() > 0
				? static_cast<const void*>(FileBytes.GetData())
				: static_cast<const void*>(&EmptyByte);
			const FSHAHash ContentHash = FSHA1::HashBuffer(DataPtr, FileBytes.Num());
			WriteString(ManifestBytes, InputFile.LogicalPath);
			WriteString(ManifestBytes, ContentHash.ToString());
		}

		OutFingerprint = LexToString(FIoHash::HashBuffer(ManifestBytes.GetData(), ManifestBytes.Num()));
		return true;
	}

	/** 用 manifest 指纹拼出全局 artifact identity。 */
	inline void BuildGlobalManifestIdentity(
		const IMonolithIndexer& Indexer,
		const FName LogicalPackageName,
		const FString& ManifestFingerprint,
		FMonolithArtifactIdentityV1& OutIdentity)
	{
		OutIdentity = FMonolithArtifactIdentityV1();
		OutIdentity.IndexerId = Indexer.GetIndexerId();
		OutIdentity.IndexerVersion = Indexer.GetIndexerVersion();
		OutIdentity.ArtifactSchemaVersion = Indexer.GetArtifactSchemaVersion();
		OutIdentity.PackageName = LogicalPackageName;
		OutIdentity.IdentityProvider = EMonolithIdentityProvider::ManifestV1;
		OutIdentity.PackageFingerprint = ManifestFingerprint;
		OutIdentity.EngineMajorVersion = static_cast<uint16>(FEngineVersion::Current().GetMajor());
		OutIdentity.EngineMinorVersion = static_cast<uint16>(FEngineVersion::Current().GetMinor());
		OutIdentity.DependencyVersions = Indexer.GetDependencyVersions();
	}

	/*
	 * Config / Cpp 这类 reducer 的 identity 规则本质上完全一样：
	 * - 先收集输入文件；
	 * - 再压成 manifest 指纹；
	 * - 最后把 manifest 指纹拼进全局 identity。
	 *
	 * 这里把这三步收口成一个 helper，避免每个 indexer 都手写一遍。
	 */
	template<typename CollectFilesFn>
	inline bool BuildInputFileManifestIdentity(
		const IMonolithIndexer& Indexer,
		const FName LogicalPackageName,
		CollectFilesFn&& CollectFiles,
		FMonolithArtifactIdentityV1& OutIdentity)
	{
		TArray<FInputFile> InputFiles;
		CollectFiles(InputFiles);

		FString ManifestFingerprint;
		if (!BuildInputFileManifestFingerprint(InputFiles, ManifestFingerprint))
		{
			return false;
		}

		BuildGlobalManifestIdentity(Indexer, LogicalPackageName, ManifestFingerprint, OutIdentity);
		return true;
	}

	/*
	 * 某些 reducer 没有“输入文件列表”这一层，而是直接从运行时状态生成 payload，
	 * 例如 GameplayTag 定义树。
	 *
	 * 这种场景最稳定的做法，就是把 payload 自己序列化后做一次 hash，
	 * 再把这个 hash 当成 manifest 指纹。
	 */
	template<typename PayloadType, typename BuildPayloadFn, typename SerializePayloadFn>
	inline bool BuildPayloadFingerprintIdentity(
		const IMonolithIndexer& Indexer,
		const FName LogicalPackageName,
		BuildPayloadFn&& BuildPayload,
		SerializePayloadFn&& SerializePayload,
		FMonolithArtifactIdentityV1& OutIdentity)
	{
		PayloadType Payload;
		if (!BuildPayload(Payload))
		{
			return false;
		}

		TArray<uint8> SerializedPayload;
		SerializePayload(Payload, SerializedPayload);
		static const uint8 EmptyByte = 0;
		const void* DataPtr = SerializedPayload.Num() > 0
			? static_cast<const void*>(SerializedPayload.GetData())
			: static_cast<const void*>(&EmptyByte);

		BuildGlobalManifestIdentity(
			Indexer,
			LogicalPackageName,
			LexToString(FIoHash::HashBuffer(DataPtr, SerializedPayload.Num())),
			OutIdentity);
		return true;
	}

	/*
	 * 全局 reducer 的 IndexGlobal 现在统一只有一种语义：
	 * - 先 build artifact；
	 * - 再 materialize artifact。
	 *
	 * 这样所有调用方都围绕 artifact 主链工作，不再保留“直接扫完就写 DB”的第二套实现。
	 */
	inline bool ExecuteIndexGlobalFromArtifact(IMonolithIndexer& Indexer, FMonolithIndexDatabase& DB)
	{
		FMonolithArtifact Artifact;
		if (!Indexer.BuildGlobalArtifact(Artifact))
		{
			return false;
		}

		return Indexer.MaterializeGlobalArtifact(Artifact, DB);
	}

	/*
	 * 大多数全局 reducer 在“把 payload 打成 artifact”这一步上的结构完全一致：
	 * - 构建 payload；
	 * - 填充 artifact 头；
	 * - 用 indexer 自己的序列化函数写 payload。
	 *
	 * 这里把重复样板代码统一收口，只把“如何构建 payload / 如何统计条目数 / 如何序列化”
	 * 留给具体 indexer 通过 lambda 提供。
	 */
	template<typename PayloadType, typename BuildPayloadFn, typename SerializePayloadFn, typename GetEntryCountFn>
	inline bool BuildGlobalPayloadArtifact(
		const IMonolithIndexer& Indexer,
		const FName LogicalPackageName,
		BuildPayloadFn&& BuildPayload,
		SerializePayloadFn&& SerializePayload,
		GetEntryCountFn&& GetEntryCount,
		FMonolithArtifact& OutArtifact)
	{
		PayloadType Payload;
		if (!BuildPayload(Payload))
		{
			return false;
		}

		OutArtifact = FMonolithArtifact();
		OutArtifact.ArtifactSchemaVersion = Indexer.GetArtifactSchemaVersion();
		OutArtifact.IndexerId = Indexer.GetIndexerId();
		OutArtifact.IndexerVersion = Indexer.GetIndexerVersion();
		OutArtifact.ExecutionMode = Indexer.GetExecutionMode();
		OutArtifact.PackageName = LogicalPackageName.ToString();
		SerializePayload(Payload, OutArtifact.Payload);
		return OutArtifact.Payload.Num() > 0 || GetEntryCount(Payload) == 0;
	}

	/*
	 * materialize 侧的重复结构也统一收口：
	 * - 先从 artifact 反序列化 payload；
	 * - 再交给具体 payload materializer 写 SQLite；
	 * - 成功后统一打印一条结构化日志。
	 */
	template<typename PayloadType, typename DeserializePayloadFn, typename MaterializePayloadFn, typename GetEntryCountFn>
	inline bool MaterializeGlobalPayloadArtifact(
		const FMonolithArtifact& Artifact,
		FMonolithIndexDatabase& DB,
		DeserializePayloadFn&& DeserializePayload,
		MaterializePayloadFn&& MaterializePayload,
		GetEntryCountFn&& GetEntryCount,
		const TCHAR* IndexerLabel,
		const TCHAR* EntryLabel)
	{
		PayloadType Payload;
		if (!DeserializePayload(Artifact.Payload, Payload))
		{
			return false;
		}

		const bool bSucceeded = MaterializePayload(Payload, DB);
		if (bSucceeded)
		{
			UE_LOG(
				LogMonolithIndex,
				Log,
				TEXT("%s: materialized %d %s from artifact"),
				IndexerLabel ? IndexerLabel : TEXT("GlobalIndexer"),
				GetEntryCount(Payload),
				EntryLabel ? EntryLabel : TEXT("entries"));
		}
		return bSucceeded;
	}
}
