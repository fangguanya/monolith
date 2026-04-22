#pragma once

#include "Commandlets/Commandlet.h"
#include "MonolithIndexWarmupCommandlet.generated.h"

/*
 * 这个 commandlet 的目标很聚焦：
 * - 预热 artifact cache；
 * - 只写 DDC / 远端缓存；
 * - 不去写本地 SQLite 索引库。
 *
 * 它适合做后台 warmup、离线 release gate 观察，以及 rollout 之前的命中率采样。
 */
UCLASS()
class UMonolithIndexWarmupCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	/** 设置 commandlet 的基础运行属性和帮助文字。 */
	UMonolithIndexWarmupCommandlet();

	/** 解析命令行，执行 warmup，并把结果记入 history。 */
	virtual int32 Main(const FString& Params) override;
};
