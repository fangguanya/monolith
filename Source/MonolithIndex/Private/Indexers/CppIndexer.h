#pragma once

#include "Indexers/MonolithGlobalArtifactHelpers.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "MonolithIndexer.h"

/**
 * FCppIndexer 负责把项目和插件里的 C++ 头/源文件扫成“可查询的 UE 反射符号表”。
 *
 * 它同样不是单资产索引，而是一次扫描很多源码文件的全局型索引器：
 * - 不再靠旧的 `__CppSymbols__` sentinel class 名触发；
 * - 统一改走 `IndexGlobal()` 入口。
 *
 * 当前它关注的是最适合被策划和工具搜索的那几类 UE 宏符号：
 * - `UCLASS`
 * - `USTRUCT`
 * - `UENUM`
 * - `UFUNCTION`
 * - `UPROPERTY`
 * - 常见 Delegate 宏
 */
class FCppIndexer : public IMonolithIndexer
{
public:
	/** C++ 符号不是资产类分发，所以这里返回空列表。 */
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return {};
	}

	/** 真正的全局源码扫描入口。 */
	virtual bool IndexGlobal(FMonolithIndexDatabase& DB) override;
	/** 为整份 C++ 符号快照构建全局 identity。 */
	virtual bool BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const override;
	/** 把整份 C++ 符号快照打包成 artifact。 */
	virtual bool BuildGlobalArtifact(FMonolithArtifact& OutArtifact) override;
	/** 把 C++ 符号 artifact 回放到正式 SQLite。 */
	virtual bool MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB) override;
	virtual FString GetName() const override { return TEXT("CppIndexer"); }
	/** 稳定 cohort/id 名。 */
	virtual FName GetIndexerId() const override { return FName(TEXT("Cpp")); }
	/** 这类任务属于全局 reducer 语义。 */
	virtual EMonolithExecutionMode GetExecutionMode() const override { return EMonolithExecutionMode::GlobalReducer; }

private:
	/** 收集这次符号快照真正参与扫描的输入文件。 */
	static void CollectSourceFiles(TArray<MonolithGlobalArtifactHelpers::FInputFile>& OutFiles);
	/** 把输入文件解析成统一 payload。 */
	static bool BuildPayload(MonolithSimpleArtifactSerialization::FCppSymbolPayload& OutPayload);
	/** 解析单个源文件，并把识别到的符号追加到 payload。 */
	static bool ParseSourceFile(
		const MonolithGlobalArtifactHelpers::FInputFile& InputFile,
		MonolithSimpleArtifactSerialization::FCppSymbolPayload& OutPayload);
};
