#include "Commandlets/MonolithIdentityPocCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MonolithArtifactTypes.h"
#include "MonolithIndexLog.h"

/*
 * 这是一个轻量 POC 工具，用来快速检查：
 * “同一批资产在当前 identity provider 下，会生成什么样的 identity hash？”
 *
 * 输出的 CSV 很适合拿去做人工 spot-check 或者简单 diff。
 */

UMonolithIdentityPocCommandlet::UMonolithIdentityPocCommandlet()
{
	LogToConsole = true;
	IsServer = false;
	IsClient = false;
	IsEditor = true;
	HelpDescription = TEXT("Monolith identity POC commandlet. 输出 package_path/provider/identity_hash/saved_hash CSV。");
}

int32 UMonolithIdentityPocCommandlet::Main(const FString& Params)
{
	// 默认只抽样 1000 个，避免一次把整个项目 CSV 打太大。
	int32 Limit = 1000;
	FParse::Value(*Params, TEXT("Limit="), Limit);
	Limit = FMath::Max(1, Limit);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	if (!AssetRegistry.IsSearchAllAssets())
	{
		AssetRegistry.SearchAllAssets(true);
	}
	AssetRegistry.WaitForCompletion();

	auto CollectAssets = [&AssetRegistry](const TCHAR* RootPath)
	{
		// 从指定根路径递归收集资产，并按包路径排序，保证输出稳定。
		TArray<FAssetData> OutAssets;
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(RootPath));
		Filter.bRecursivePaths = true;
		AssetRegistry.GetAssets(Filter, OutAssets);
		OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.LexicalLess(B.PackageName);
		});
		return OutAssets;
	};

	TArray<FAssetData> Assets = CollectAssets(TEXT("/Game/Characters"));
	if (Assets.Num() < Limit)
	{
		// 如果 Characters 不够，就退回整个 /Game 做补充。
		Assets = CollectAssets(TEXT("/Game"));
	}
	if (Assets.Num() > Limit)
	{
		Assets.SetNum(Limit);
	}

	TArray<FString> Lines;
	Lines.Reserve(Assets.Num() + 1);
	Lines.Add(TEXT("package_path,provider,identity_hash,saved_hash"));

	for (const FAssetData& AssetData : Assets)
	{
		// 这里只构建 identity，不构建 artifact，方便把问题范围压小。
		FMonolithArtifactIdentityV1 Identity;
		if (!BuildConfiguredMonolithArtifactIdentity(
			AssetData,
			AssetRegistry,
			FName(TEXT("IdentityPoc")),
			1,
			1,
			{},
			Identity))
		{
			continue;
		}

		const TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(AssetData.PackageName);
		const FString SavedHash = PackageData.IsSet() ? LexToString(PackageData->GetPackageSavedHash()) : FString();
		// 每行输出最关键的四列，方便 Excel / diff 工具直接使用。
		Lines.Add(FString::Printf(
			TEXT("%s,%s,%s,%s"),
			*AssetData.PackageName.ToString(),
			*LexToString(Identity.IdentityProvider),
			*LexToString(HashMonolithArtifactIdentity(Identity)),
			*SavedHash));
	}

	const FString OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithIndex"));
	IFileManager::Get().MakeDirectory(*OutputDirectory, true);
	const FString OutputPath = FPaths::Combine(
		OutputDirectory,
		FString::Printf(TEXT("identity-poc-%s.csv"), *LexToString(GetConfiguredMonolithIdentityProvider())));

	// 强制 UTF-8，无 BOM，保证后续脚本和 diff 工具更好处理。
	if (!FFileHelper::SaveStringArrayToFile(Lines, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("写入 identity CSV 失败: %s"), *OutputPath);
		return 1;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Identity POC CSV 已写入: %s (%d rows)"), *OutputPath, Assets.Num());
	return 0;
}
