#include "MonolithSettings.h"

#include "Interfaces/IPluginManager.h"

namespace MonolithSettingsInternal
{
	/** 把当前配置下允许参与索引的插件挂载点补进路径数组。 */
	static void AppendIndexedPluginMountPaths(TArray<FName>& InOutPaths)
	{
		const UMonolithSettings* Settings = UMonolithSettings::Get();
		if (!Settings || !Settings->bIndexMarketplacePlugins)
		{
			return;
		}

		const TArray<TSharedRef<IPlugin>> ContentPlugins = IPluginManager::Get().GetEnabledPluginsWithContent();
		for (const TSharedRef<IPlugin>& Plugin : ContentPlugins)
		{
			// 这里延续 MonolithIndex 的约定：
			// 只纳入项目/市场插件内容，不把 engine 自带插件内容默认塞进索引范围。
			if (Plugin->GetType() == EPluginType::Engine)
			{
				continue;
			}

			const FString MountPath = Plugin->GetMountedAssetPath();
			if (!MountPath.IsEmpty())
			{
				InOutPaths.AddUnique(FName(*MountPath));
			}
		}
	}
}

UMonolithSettings::UMonolithSettings()
{
}

const UMonolithSettings* UMonolithSettings::Get()
{
	return GetDefault<UMonolithSettings>();
}

TArray<FName> UMonolithSettings::GetIndexedContentPaths()
{
	TArray<FName> Paths;
	Paths.Add(FName(TEXT("/Game")));
	MonolithSettingsInternal::AppendIndexedPluginMountPaths(Paths);

	if (const UMonolithSettings* Settings = Get())
	{
		for (const FString& Path : Settings->AdditionalContentPaths)
		{
			if (!Path.IsEmpty())
			{
				Paths.AddUnique(FName(*Path));
			}
		}
	}

	return Paths;
}

bool UMonolithSettings::IsIndexedContentPath(const FString& PackagePath)
{
	TArray<FName> IndexedPaths = GetIndexedContentPaths();
	for (const FName& IndexedPath : IndexedPaths)
	{
		const FString Prefix = IndexedPath.ToString();
		if (PackagePath.Equals(Prefix, ESearchCase::CaseSensitive)
			|| PackagePath.StartsWith(Prefix + TEXT("/")))
		{
			return true;
		}
	}

	return false;
}
