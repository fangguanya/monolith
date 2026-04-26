#include "MonolithMeshSelectionActions.h"
#include "MonolithParamSchema.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "SceneView.h"
#include "UnrealClient.h"
#include "Editor/UnrealEdEngine.h"

/*
 * 实现要点：
 *  - 所有公开入口都做了 `IsInGameThread()` 断言，保证不会在 BackgroundCpuPool 误用。
 *  - selection action 走 GEditor->GetSelectedActors()；按选中顺序 + (Actor, Component) 复合键去重。
 *  - cursor action 走 LevelEditor 当前 viewport client 的 line trace；HISM/ISM 命中带 instance index。
 *  - 不触发任何资产 LoadObject；只读取已经在内存里的 Actor / Component / StaticMesh 引用。
 */
namespace MonolithMeshSelectionInternal
{
	/** 决定本次 action 应当作用于哪个世界。
	 *  默认 EditorWorld；显式 `world=pie` 时才用 PIE world。 */
	static UWorld* ResolveTargetWorld(const TSharedPtr<FJsonObject>& Params)
	{
		const bool bWantPie = Params.IsValid() && Params->HasTypedField<EJson::String>(TEXT("world"))
			&& Params->GetStringField(TEXT("world")).Equals(TEXT("pie"), ESearchCase::IgnoreCase);

		if (!GEditor)
		{
			return nullptr;
		}

		if (bWantPie)
		{
			// 拿第一个 PIE 世界；没有就退到 EditorWorld。
			for (const FWorldContext& Context : GEditor->GetWorldContexts())
			{
				if (Context.WorldType == EWorldType::PIE && Context.World())
				{
					return Context.World();
				}
			}
		}
		return GEditor->GetEditorWorldContext().World();
	}

	/** 把单个 mesh 组件命中描述成 JSON 对象（含资产路径 / actor / component / 资产类型 / 可选实例索引）。 */
	static TSharedPtr<FJsonObject> BuildHitJson(
		const AActor* Actor,
		const UPrimitiveComponent* Component,
		const FString& AssetPath,
		const FString& AssetTypeEnum,
		const int32 InstanceIndex = INDEX_NONE)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("asset_path"), AssetPath);
		Obj->SetStringField(TEXT("asset_type"), AssetTypeEnum);
		Obj->SetStringField(TEXT("actor"), Actor ? Actor->GetPathName() : FString());
		Obj->SetStringField(TEXT("component"), Component ? Component->GetName() : FString());
		if (InstanceIndex != INDEX_NONE)
		{
			Obj->SetNumberField(TEXT("instance_index"), InstanceIndex);
		}
		return Obj;
	}

	/** 从单个 component 提取底层 mesh 资产 + 类型；返回 false 表示该 component 不是 mesh component。 */
	static bool ResolveMeshFromComponent(
		const UPrimitiveComponent* Component,
		FString& OutAssetPath,
		FString& OutAssetTypeEnum)
	{
		if (const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Component))
		{
			if (UStaticMesh* SM = SMC->GetStaticMesh())
			{
				OutAssetPath = SM->GetPathName();
				OutAssetTypeEnum = TEXT("static_mesh");
				return true;
			}
		}
		else if (const USkeletalMeshComponent* SKMC = Cast<USkeletalMeshComponent>(Component))
		{
			if (USkeletalMesh* SK = SKMC->GetSkeletalMeshAsset())
			{
				OutAssetPath = SK->GetPathName();
				OutAssetTypeEnum = TEXT("skeletal_mesh");
				return true;
			}
		}
		return false;
	}

	/** 给 Actor 的所有 mesh 组件生成 hit 列表；按组件枚举顺序 + 复合键去重。 */
	static void CollectMeshHitsFromActor(
		const AActor* Actor,
		TSet<FString>& InOutSeenKeys,
		TArray<TSharedPtr<FJsonValue>>& OutHits)
	{
		if (!Actor)
		{
			return;
		}
		TInlineComponentArray<UPrimitiveComponent*> Components;
		Actor->GetComponents<UPrimitiveComponent>(Components);
		for (UPrimitiveComponent* Component : Components)
		{
			FString AssetPath;
			FString AssetTypeEnum;
			if (!ResolveMeshFromComponent(Component, AssetPath, AssetTypeEnum))
			{
				continue;
			}
			// (Actor, Component) 复合键去重，避免同一 actor 被选中两次时重复返回。
			const FString Key = Actor->GetPathName() + TEXT("|") + Component->GetName();
			bool bAlreadySeen = false;
			InOutSeenKeys.Add(Key, &bAlreadySeen);
			if (bAlreadySeen)
			{
				continue;
			}
			OutHits.Add(MakeShared<FJsonValueObject>(BuildHitJson(Actor, Component, AssetPath, AssetTypeEnum)));
		}
	}
}

void FMonolithMeshSelectionActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_selected_mesh_assets"),
		TEXT("Resolve mesh assets bound to currently selected Actors / Components in the editor (no embedding lookup)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSelectionActions::HandleGetSelectedMeshAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("world"), TEXT("string"), TEXT("editor | pie，默认 editor"), TEXT("editor"))
			.Build(),
		EMonolithActionExecutionPolicy::GameThread);

	Registry.RegisterAction(TEXT("mesh"), TEXT("query_mesh_under_cursor"),
		TEXT("Resolve mesh asset under given screen coordinates (or current cursor) in the active viewport"),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshSelectionActions::HandleQueryMeshUnderCursor),
		FParamSchemaBuilder()
			.Optional(TEXT("screen_x"), TEXT("number"), TEXT("屏幕 X（像素）；省略时使用当前鼠标位置"))
			.Optional(TEXT("screen_y"), TEXT("number"), TEXT("屏幕 Y（像素）；省略时使用当前鼠标位置"))
			.Optional(TEXT("world"), TEXT("string"), TEXT("editor | pie，默认 editor"), TEXT("editor"))
			.Build(),
		EMonolithActionExecutionPolicy::GameThread);
}

FMonolithActionResult FMonolithMeshSelectionActions::HandleGetSelectedMeshAssets(const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	const double StartSeconds = FPlatformTime::Seconds();

	using namespace MonolithMeshSelectionInternal;

	UWorld* World = ResolveTargetWorld(Params);
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("Editor world unavailable"));
	}
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor unavailable"));
	}

	USelection* SelectedActors = GEditor->GetSelectedActors();
	USelection* SelectedComponents = GEditor->GetSelectedComponents();

	TArray<TSharedPtr<FJsonValue>> HitsArray;
	TSet<FString> SeenKeys;

	// 优先按 selected components；selected actors 是其超集。
	// 若用户在 detail 面板选了具体 component，应当只返回那一个 component 命中那个 mesh。
	if (SelectedComponents && SelectedComponents->Num() > 0)
	{
		for (FSelectionIterator It(*SelectedComponents); It; ++It)
		{
			UPrimitiveComponent* Component = Cast<UPrimitiveComponent>(*It);
			if (!Component)
			{
				continue;
			}
			AActor* Owner = Component->GetOwner();
			FString AssetPath, AssetTypeEnum;
			if (!ResolveMeshFromComponent(Component, AssetPath, AssetTypeEnum))
			{
				continue;
			}
			const FString Key = (Owner ? Owner->GetPathName() : FString()) + TEXT("|") + Component->GetName();
			bool bAlreadySeen = false;
			SeenKeys.Add(Key, &bAlreadySeen);
			if (!bAlreadySeen)
			{
				HitsArray.Add(MakeShared<FJsonValueObject>(BuildHitJson(Owner, Component, AssetPath, AssetTypeEnum)));
			}
		}
	}
	else if (SelectedActors)
	{
		for (FSelectionIterator It(*SelectedActors); It; ++It)
		{
			const AActor* Actor = Cast<AActor>(*It);
			CollectMeshHitsFromActor(Actor, SeenKeys, HitsArray);
		}
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("hits"), HitsArray);
	Result->SetNumberField(TEXT("count"), HitsArray.Num());
	Result->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshSelectionActions::HandleQueryMeshUnderCursor(const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	const double StartSeconds = FPlatformTime::Seconds();

	using namespace MonolithMeshSelectionInternal;

	UWorld* World = ResolveTargetWorld(Params);
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("Editor world unavailable"));
	}
	if (!GCurrentLevelEditingViewportClient)
	{
		return FMonolithActionResult::Error(TEXT("Active viewport client unavailable"));
	}

	FViewport* Viewport = GCurrentLevelEditingViewportClient->Viewport;
	if (!Viewport)
	{
		return FMonolithActionResult::Error(TEXT("Active viewport unavailable"));
	}

	// 屏幕坐标：默认取当前鼠标；显式提供 screen_x / screen_y 时覆盖。
	int32 ScreenX = Viewport->GetMouseX();
	int32 ScreenY = Viewport->GetMouseY();
	if (Params.IsValid() && Params->HasTypedField<EJson::Number>(TEXT("screen_x")))
	{
		ScreenX = static_cast<int32>(Params->GetNumberField(TEXT("screen_x")));
	}
	if (Params.IsValid() && Params->HasTypedField<EJson::Number>(TEXT("screen_y")))
	{
		ScreenY = static_cast<int32>(Params->GetNumberField(TEXT("screen_y")));
	}

	// Deproject 到世界射线，再 line trace。
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		Viewport,
		World->Scene,
		GCurrentLevelEditingViewportClient->EngineShowFlags).SetRealtimeUpdate(false));
	FSceneView* SceneView = GCurrentLevelEditingViewportClient->CalcSceneView(&ViewFamily);
	if (!SceneView)
	{
		return FMonolithActionResult::Error(TEXT("Failed to construct scene view"));
	}

	FVector RayOrigin, RayDirection;
	SceneView->DeprojectFVector2D(FVector2D(ScreenX, ScreenY), RayOrigin, RayDirection);

	const FVector RayEnd = RayOrigin + RayDirection * 1.0e6;
	FHitResult HitResult;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MonolithMeshUnderCursor), /*bTraceComplex=*/true);

	const bool bHit = World->LineTraceSingleByChannel(
		HitResult,
		RayOrigin,
		RayEnd,
		ECC_Visibility,
		QueryParams);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> HitsArray;
	if (bHit)
	{
		UPrimitiveComponent* Component = HitResult.GetComponent();
		FString AssetPath, AssetTypeEnum;
		if (Component && ResolveMeshFromComponent(Component, AssetPath, AssetTypeEnum))
		{
			int32 InstanceIndex = INDEX_NONE;
			if (const UInstancedStaticMeshComponent* ISMC = Cast<UInstancedStaticMeshComponent>(Component))
			{
				// HitResult.Item 由 ISM/HISM 在 line trace 时填的实例索引。
				InstanceIndex = HitResult.Item;
				(void)ISMC;
			}
			HitsArray.Add(MakeShared<FJsonValueObject>(
				BuildHitJson(HitResult.GetActor(), Component, AssetPath, AssetTypeEnum, InstanceIndex)));
		}
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	Result->SetArrayField(TEXT("hits"), HitsArray);
	Result->SetNumberField(TEXT("count"), HitsArray.Num());
	Result->SetBoolField(TEXT("hit"), bHit);
	Result->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}
