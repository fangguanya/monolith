#pragma once

#include "Indexers/MonolithGlobalArtifactHelpers.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "MonolithIndexer.h"

/**
 * FConfigIndexer 负责把项目和引擎里的 `.ini` 配置文件拆成结构化表。
 *
 * 它不是“某一个资产”的附属信息，
 * 而是一次要扫描很多配置文件的全局型索引器，所以现在统一走 `IndexGlobal()`：
 * - 不再靠 magic sentinel class 名触发；
 * - 不再要求外层伪造一份假 `AssetData`。
 *
 * 这条链路的目标很简单：
 * “把 INI 里的 文件 / Section / Key / Value 四元组稳定落到数据库里。”
 */
class FConfigIndexer : public IMonolithIndexer
{
public:
	/** Config 不是按真实资产类分发，所以这里返回空列表。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {};
	}

	/** 真正的全局配置扫描入口。 */
	virtual bool IndexGlobal(FMonolithIndexDatabase& DB) override;
	/** 为整份 Config 快照构建全局 identity。 */
	virtual bool BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const override;
	/** 把整份 Config 快照打包成 artifact。 */
	virtual bool BuildGlobalArtifact(FMonolithArtifact& OutArtifact) override;
	/** 把 Config artifact 回放到正式 SQLite。 */
	virtual bool MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB) override;
	virtual FString GetName() const override { return TEXT("ConfigIndexer"); }
	/** 用稳定 cohort/id 名字，避免再依赖旧 sentinel 文本。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Config")); }
	/** Config 更接近“全局 reducer”型任务，而不是单资产加载。 */
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::GlobalReducer; }

private:
	/** 收集这次 Config 快照真正参与计算的输入文件。 */
	static void CollectConfigFiles(TArray<MonolithGlobalArtifactHelpers::FInputFile>& OutFiles);
	/** 把输入文件解析成统一 payload。 */
	static bool BuildPayload(MonolithSimpleArtifactSerialization::FConfigPayload& OutPayload);
	/** 解析单个 `.ini` 文件，并把结果追加到 payload。 */
	static bool ParseIniFileToPayload(
		const MonolithGlobalArtifactHelpers::FInputFile& InputFile,
		MonolithSimpleArtifactSerialization::FConfigPayload& OutPayload);
};
