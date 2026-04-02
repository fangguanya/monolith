#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBaseActions
{
public:
	static void RegisterActions();

	// 地图管理（从 MonolithCapture 迁移）
	static FMonolithActionResult HandleGetCurrentMap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleOpenMap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSaveMap(const TSharedPtr<FJsonObject>& Params);

	// Actor 查找与聚焦（从 MonolithCapture 迁移）
	static FMonolithActionResult HandleFindActorsByClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSelectAndFocus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFocusActor(const TSharedPtr<FJsonObject>& Params);

	// Actor 基础操作
	static FMonolithActionResult HandleGetActorTags(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActorTags(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddActorTag(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveActorTag(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetActorProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetActorTransform(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDestroyActor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSpawnActor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListActorComponents(const TSharedPtr<FJsonObject>& Params);

	// UObject 基础操作
	static FMonolithActionResult HandleLoadObject(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetObjectProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetObjectProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindAssets(const TSharedPtr<FJsonObject>& Params);

	// 组件管理
	static FMonolithActionResult HandleAddComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetComponentProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params);

	// Actor 编辑
	static FMonolithActionResult HandleRenameActor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDuplicateActor(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActorMobility(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetActorHidden(const TSharedPtr<FJsonObject>& Params);

	// Mesh & Material 配套
	static FMonolithActionResult HandleSetStaticMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSkeletalMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetComponentMaterial(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetComponentMaterials(const TSharedPtr<FJsonObject>& Params);

	// Animation & AI 配套
	static FMonolithActionResult HandleSetAnimClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetAIControllerClass(const TSharedPtr<FJsonObject>& Params);

	// Physics & Collision
	static FMonolithActionResult HandleSetCollisionPreset(const TSharedPtr<FJsonObject>& Params);

private:
	// 辅助：按名称/标签定位 Actor
	static AActor* FindActorByNameOrLabel(UWorld* World, const TSharedPtr<FJsonObject>& Params);
	// 辅助：按名称定位组件
	static UActorComponent* FindComponentByName(AActor* Actor, const FString& CompName);
};
