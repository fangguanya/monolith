#include "Indexers/LevelIndexer.h"
#include "MonolithIndexerShadowMode.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/StringConv.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Components/ActorComponent.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartition.h"

/*
 * 这份实现文件把 World 的 PersistentLevel 抽成一串 Actor 摘要。
 *
 * 这里不追求还原整个关卡对象图，
 * 而是抓最适合索引和比较的那部分：
 * - Actor 名
 * - Actor 类
 * - 标签
 * - Transform
 * - 组件摘要
 */

namespace LevelIndexerInternal
{
	static void WriteUInt32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	static bool ReadUInt32(const TArray<uint8>& Bytes, int32& Offset, uint32& OutValue)
	{
		if (Offset + 4 > Bytes.Num())
		{
			return false;
		}

		OutValue = static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4;
		return true;
	}

	static void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	static bool ReadString(const TArray<uint8>& Bytes, int32& Offset, FString& OutValue)
	{
		uint32 Length = 0;
		if (!ReadUInt32(Bytes, Offset, Length))
		{
			return false;
		}

		if (Length == 0)
		{
			OutValue.Reset();
			return true;
		}

		if (Offset + static_cast<int32>(Length) > Bytes.Num())
		{
			return false;
		}

		FUTF8ToTCHAR Convert(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData() + Offset), static_cast<int32>(Length));
		OutValue = FString(Convert.Length(), Convert.Get());
		Offset += static_cast<int32>(Length);
		return true;
	}
}

bool FLevelIndexer::BuildArtifact(const FAssetData& AssetData, UObject* LoadedAsset, IAssetRegistry& AssetRegistry, FMonolithArtifact& OutArtifact)
{
	TArray<FIndexedActor> Actors;
	if (!BuildActorPayload(LoadedAsset, Actors))
	{
		return false;
	}

	OutArtifact = FMonolithArtifact();
	OutArtifact.ArtifactSchemaVersion = GetArtifactSchemaVersion();
	OutArtifact.IndexerId = GetIndexerId();
	OutArtifact.IndexerVersion = GetIndexerVersion();
	OutArtifact.ExecutionMode = GetExecutionMode();
	OutArtifact.PackageName = AssetData.PackageName.ToString();
	SerializePayload(Actors, OutArtifact.Payload);
	return true;
}

bool FLevelIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	TArray<FIndexedActor> Actors;
	if (!DeserializePayload(Artifact.Payload, Actors))
	{
		return false;
	}

	return MaterializeActors(Actors, DB, AssetId);
}

bool FLevelIndexer::MaterializeArtifactToShadow(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName)
{
	TArray<FIndexedActor> Actors;
	if (!DeserializePayload(Artifact.Payload, Actors))
	{
		return false;
	}

	return MaterializeActorsToShadow(Actors, DB, AssetId, CohortName);
}

bool FLevelIndexer::BuildActorPayload(UObject* LoadedAsset, TArray<FIndexedActor>& OutActors) const
{
	OutActors.Reset();

	UWorld* World = Cast<UWorld>(LoadedAsset);
	if (!World || !World->PersistentLevel)
	{
		return false;
	}

	return BuildActorsInLevel(World->PersistentLevel, OutActors);
}

bool FLevelIndexer::BuildActorsInLevel(ULevel* Level, TArray<FIndexedActor>& OutActors) const
{
	if (!Level)
	{
		return false;
	}

	OutActors.Reset();
	for (AActor* Actor : Level->Actors)
	{
		// WorldSettings 是引擎默认管理对象，几乎总在，
		// 但对“关卡内容索引”帮助不大，所以这里主动跳过。
		if (!Actor) continue;
		if (Actor->IsA(AWorldSettings::StaticClass())) continue;

		FIndexedActor IndexedActor;
		IndexedActor.ActorName = Actor->GetName();
		IndexedActor.ActorClass = Actor->GetClass()->GetName();
		IndexedActor.ActorLabel = Actor->GetActorLabel();
		IndexedActor.Transform = SerializeTransform(Actor->GetActorTransform());
		IndexedActor.Components = SerializeComponents(Actor);
		OutActors.Add(MoveTemp(IndexedActor));
	}

	OutActors.Sort([](const FIndexedActor& A, const FIndexedActor& B)
	{
		// 明确排序后，artifact payload 和 shadow 比较结果才稳定。
		if (A.ActorName != B.ActorName)
		{
			return A.ActorName < B.ActorName;
		}
		if (A.ActorClass != B.ActorClass)
		{
			return A.ActorClass < B.ActorClass;
		}
		return A.ActorLabel < B.ActorLabel;
	});

	return true;
}

bool FLevelIndexer::MaterializeActors(const TArray<FIndexedActor>& Actors, FMonolithIndexDatabase& DB, int64 AssetId) const
{
	for (const FIndexedActor& Actor : Actors)
	{
		FIndexedActor IndexedActor = Actor;
		IndexedActor.AssetId = AssetId;
		if (DB.InsertActor(IndexedActor) <= 0)
		{
			return false;
		}
	}

	return true;
}

bool FLevelIndexer::MaterializeActorsToShadow(const TArray<FIndexedActor>& Actors, FMonolithIndexDatabase& DB, int64 AssetId, const FString& CohortName) const
{
	TArray<FMonolithShadowIndexedActor> ShadowActors;
	ShadowActors.Reserve(Actors.Num());
	for (const FIndexedActor& Actor : Actors)
	{
		// shadow 表里会额外带 row_hash，方便先做一级聚合 diff。
		FMonolithShadowIndexedActor ShadowActor;
		ShadowActor.Actor = Actor;
		ShadowActor.Actor.AssetId = AssetId;
		ShadowActor.RowHash = ComputeActorRowHash(ShadowActor.Actor);
		ShadowActors.Add(MoveTemp(ShadowActor));
	}

	return DB.ReplaceShadowActorsForAsset(CohortName, AssetId, ShadowActors);
}

void FLevelIndexer::SerializePayload(const TArray<FIndexedActor>& Actors, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	OutBytes.Add(1);
	LevelIndexerInternal::WriteUInt32(OutBytes, static_cast<uint32>(Actors.Num()));
	for (const FIndexedActor& Actor : Actors)
	{
		LevelIndexerInternal::WriteString(OutBytes, Actor.ActorName);
		LevelIndexerInternal::WriteString(OutBytes, Actor.ActorClass);
		LevelIndexerInternal::WriteString(OutBytes, Actor.ActorLabel);
		LevelIndexerInternal::WriteString(OutBytes, Actor.Transform);
		LevelIndexerInternal::WriteString(OutBytes, Actor.Components);
	}
}

bool FLevelIndexer::DeserializePayload(const TArray<uint8>& Bytes, TArray<FIndexedActor>& OutActors)
{
	OutActors.Reset();
	if (Bytes.Num() < 1 || Bytes[0] != 1)
	{
		return false;
	}

	int32 Offset = 1;
	uint32 ActorCount = 0;
	if (!LevelIndexerInternal::ReadUInt32(Bytes, Offset, ActorCount))
	{
		return false;
	}

	OutActors.Reserve(static_cast<int32>(ActorCount));
	for (uint32 ActorIndex = 0; ActorIndex < ActorCount; ++ActorIndex)
	{
		FIndexedActor Actor;
		if (!LevelIndexerInternal::ReadString(Bytes, Offset, Actor.ActorName)
			|| !LevelIndexerInternal::ReadString(Bytes, Offset, Actor.ActorClass)
			|| !LevelIndexerInternal::ReadString(Bytes, Offset, Actor.ActorLabel)
			|| !LevelIndexerInternal::ReadString(Bytes, Offset, Actor.Transform)
			|| !LevelIndexerInternal::ReadString(Bytes, Offset, Actor.Components))
		{
			return false;
		}

		OutActors.Add(MoveTemp(Actor));
	}

	return true;
}

FString FLevelIndexer::SerializeTransform(const FTransform& Transform) const
{
	auto Obj = MakeShared<FJsonObject>();

	const FVector& Loc = Transform.GetLocation();
	auto LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Obj->SetObjectField(TEXT("location"), LocObj);

	const FRotator Rot = Transform.GetRotation().Rotator();
	auto RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
	Obj->SetObjectField(TEXT("rotation"), RotObj);

	const FVector& Scale = Transform.GetScale3D();
	auto ScaleObj = MakeShared<FJsonObject>();
	ScaleObj->SetNumberField(TEXT("x"), Scale.X);
	ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
	ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
	Obj->SetObjectField(TEXT("scale"), ScaleObj);

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(Obj, *Writer, true);
	return Result;
}

FString FLevelIndexer::SerializeComponents(const AActor* Actor) const
{
	TArray<TSharedPtr<FJsonValue>> CompArray;

	TInlineComponentArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (const UActorComponent* Comp : Components)
	{
		if (!Comp) continue;

		auto CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Comp->GetName());
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

		CompArray.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(CompArray, *Writer);
	return Result;
}
