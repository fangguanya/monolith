#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithIndexDatabase.h"

/*
 * 这条测试只做一件事：
 * 确认 asset_index_metadata 这张“索引元数据表”能正确 round-trip。
 */

namespace MonolithIndexDatabaseMetadataTestInternal
{
	/** 使用临时数据库，避免污染真实索引库。 */
	static FString MakeTempDatabasePath()
	{
		return FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("MonolithIndexMetadata"), TEXT(".db"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexDatabaseMetadataRoundTripTest,
	"Monolith.Index.Database.AssetIndexMetadataRoundTrips",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexDatabaseMetadataRoundTripTest::RunTest(const FString& Parameters)
{
	// 这里把 metadata 的关键字段都写一遍，再读回来核对。
	const FString DbPath = MonolithIndexDatabaseMetadataTestInternal::MakeTempDatabasePath();
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*DbPath, false, true);
	};

	FMonolithIndexDatabase DB;
	TestTrue(TEXT("database should open"), DB.Open(DbPath));

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Test/SM_Metadata");
	Asset.AssetName = TEXT("SM_Metadata");
	Asset.AssetClass = TEXT("StaticMesh");
	const int64 AssetId = DB.InsertAsset(Asset);
	TestTrue(TEXT("asset should be inserted"), AssetId > 0);

	FMonolithAssetIndexMetadata WrittenMetadata;
	WrittenMetadata.AssetId = AssetId;
	WrittenMetadata.IndexerId = TEXT("GenericAsset");
	WrittenMetadata.IndexerVersion = 7;
	WrittenMetadata.ArtifactSchemaVersion = 3;
	WrittenMetadata.IdentityProvider = TEXT("SavedHash");
	WrittenMetadata.ExecutionMode = TEXT("PackageScopedLoad");
	WrittenMetadata.IdentityHash = TEXT("abc123");
	TestTrue(TEXT("metadata upsert should succeed"), DB.UpsertAssetIndexMetadata(WrittenMetadata));

	const TOptional<FMonolithAssetIndexMetadata> ReadMetadata = DB.GetAssetIndexMetadataByAssetId(AssetId);
	TestTrue(TEXT("metadata should be readable"), ReadMetadata.IsSet());
	if (!ReadMetadata.IsSet())
	{
		return false;
	}

	TestEqual(TEXT("indexer id should round-trip"), ReadMetadata->IndexerId, WrittenMetadata.IndexerId);
	TestEqual(TEXT("indexer version should round-trip"), ReadMetadata->IndexerVersion, WrittenMetadata.IndexerVersion);
	TestEqual(TEXT("artifact schema version should round-trip"), ReadMetadata->ArtifactSchemaVersion, WrittenMetadata.ArtifactSchemaVersion);
	TestEqual(TEXT("identity provider should round-trip"), ReadMetadata->IdentityProvider, WrittenMetadata.IdentityProvider);
	TestEqual(TEXT("execution mode should round-trip"), ReadMetadata->ExecutionMode, WrittenMetadata.ExecutionMode);
	TestEqual(TEXT("identity hash should round-trip"), ReadMetadata->IdentityHash, WrittenMetadata.IdentityHash);

	return true;
}
