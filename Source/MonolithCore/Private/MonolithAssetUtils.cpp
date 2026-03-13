#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"

FString FMonolithAssetUtils::ResolveAssetPath(const FString& InPath)
{
	FString Path = InPath;
	Path.TrimStartAndEndInline();

	// Normalize backslashes
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Handle /Content/ → /Game/
	if (Path.StartsWith(TEXT("/Content/")))
	{
		Path = TEXT("/Game/") + Path.Mid(9);
	}
	else if (!Path.StartsWith(TEXT("/")))
	{
		// Relative path — assume /Game/
		Path = TEXT("/Game/") + Path;
	}

	// Strip extension if present
	if (Path.EndsWith(TEXT(".uasset")) || Path.EndsWith(TEXT(".umap")))
	{
		Path = FPaths::GetBaseFilename(Path, false);
	}

	return Path;
}

UPackage* FMonolithAssetUtils::LoadPackageByPath(const FString& AssetPath)
{
	FString Resolved = ResolveAssetPath(AssetPath);
	UPackage* Package = LoadPackage(nullptr, *Resolved, LOAD_None);
	if (!Package)
	{
		UE_LOG(LogMonolith, Warning, TEXT("Failed to load package: %s"), *Resolved);
	}
	return Package;
}

UObject* FMonolithAssetUtils::LoadAssetByPath(const FString& AssetPath)
{
	FString Resolved = ResolveAssetPath(AssetPath);

	// === Asset Registry-first path ===
	// The Asset Registry reflects the editor's current ground truth.
	// StaticLoadObject can return stale RF_Standalone ghosts from prior MCP creates
	// if the disk file was deleted and recreated — the Asset Registry avoids this.
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Build PackageName.ObjectName for Asset Registry lookup
	FString PackageName = FPackageName::ObjectPathToPackageName(Resolved);
	FString ObjectName = FPackageName::ObjectPathToObjectName(Resolved);
	if (ObjectName.IsEmpty())
	{
		ObjectName = FPackageName::GetShortName(PackageName);
	}
	FString FullObjectPath = PackageName + TEXT(".") + ObjectName;

	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
	if (AssetData.IsValid())
	{
		UObject* Asset = AssetData.GetAsset();
		if (Asset)
		{
			return Asset;
		}
	}

	// === Fallback: StaticLoadObject ===
	// For assets not yet in the Asset Registry (e.g. just created this frame)
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Resolved);
	if (Asset)
	{
		return Asset;
	}

	Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *FullObjectPath);
	if (Asset)
	{
		return Asset;
	}

	// Load the package and search within it
	UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
	if (Package)
	{
		Asset = FindObject<UObject>(Package, *ObjectName);
		if (!Asset)
		{
			Asset = FindObject<UObject>(Package, *(ObjectName + TEXT("_C")));
		}
		if (!Asset)
		{
			ForEachObjectWithPackage(Package, [&Asset](UObject* Obj)
			{
				if (!Obj->IsA<UPackage>() && !Obj->HasAnyFlags(RF_Transient))
				{
					Asset = Obj;
					return false;
				}
				return true;
			}, false);
		}
	}

	if (!Asset)
	{
		UE_LOG(LogMonolith, Warning, TEXT("Failed to load asset: %s (tried: %s, %s)"), *AssetPath, *Resolved, *FullObjectPath);
	}
	return Asset;
}

bool FMonolithAssetUtils::AssetExists(const FString& AssetPath)
{
	FString Resolved = ResolveAssetPath(AssetPath);
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Resolved));
	return AssetData.IsValid();
}

TArray<FAssetData> FMonolithAssetUtils::GetAssetsByClass(const FTopLevelAssetPath& ClassPath, const FString& PackagePath)
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(ClassPath);
	if (!PackagePath.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PackagePath));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> Results;
	AssetRegistry.GetAssets(Filter, Results);
	return Results;
}

FString FMonolithAssetUtils::GetAssetName(const FString& AssetPath)
{
	return FPackageName::GetShortName(AssetPath);
}
