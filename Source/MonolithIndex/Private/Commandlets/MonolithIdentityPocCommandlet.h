#pragma once

#include "Commandlets/Commandlet.h"
#include "MonolithIdentityPocCommandlet.generated.h"

/*
 * 这个 commandlet 是一个“身份证生成实验台”。
 *
 * 它不会真正跑完整索引，
 * 而是抽样一批资产，输出：
 * - package path
 * - 当前 provider
 * - identity hash
 * - saved hash
 *
 * 这样方便快速验证不同 identity provider 的行为是否稳定。
 */
UCLASS()
class UMonolithIdentityPocCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	/** 设置 commandlet 的基础运行属性和帮助文字。 */
	UMonolithIdentityPocCommandlet();

	/** 抽样资产并输出 identity 对照 CSV。 */
	virtual int32 Main(const FString& Params) override;
};
