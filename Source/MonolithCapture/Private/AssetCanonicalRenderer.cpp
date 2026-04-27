#include "MonolithCaptureUtils.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Math/Box.h"
#include "Math/Vector.h"
#include "Modules/ModuleManager.h"
#include "AssetCompilingManager.h"
#include "CanvasTypes.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ThumbnailRendering/ThumbnailRenderer.h"
#include "ShaderCompiler.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/WidgetRenderer.h"
#include "WidgetBlueprint.h"

#include "MonolithCaptureModule.h"

/*
 * 这个文件是 IAssetCanonicalRenderer 在 Monolith 进程内的唯一实现。
 *
 * v2 架构：
 *  接口接受 UObject* Asset，按运行时类型分发到 4 条具体渲染管线。
 *  全部 4 类输出统一为 BGRA8 sRGB FImage，下游 AssetVisual cohort 看不到差异。
 *
 *   UStaticMesh         → RenderMeshLikePipeline  （SCC2D + UStaticMeshComponent）
 *   USkeletalMesh       → RenderMeshLikePipeline  （SCC2D + USkeletalMeshComponent）
 *   UMaterialInterface  → RenderMaterialPipeline  （SCC2D + UStaticMeshComponent[球] + SetMaterial）
 *   UWidgetBlueprint    → RenderWidgetPipeline    （FWidgetRenderer 直接 rasterize 到 RT）
 *
 * 设计原则：
 * 1. 所有可能影响最终像素的参数都集中在 RenderRecipeVersion 里，
 *    bump 一次就让 AssetVisual cohort 整库 stale。
 * 2. 不允许再有第二份 SCC2D + RenderTarget + ShowOnlyList 的代码。
 * 3. 双次 CaptureScene + 强制 FinishAllCompilation 保证着色器编译完成，
 *    避免首次渲染拿到未编译的 fallback 材质。
 */
namespace MonolithCaptureRendererInternal
{
	using namespace MonolithCapture;

	/*
	 * 当前 render recipe 版本号。
	 *
	 * 这个数字会被 AssetVisual artifact Meta 携带；改动以下任意点都必须 bump：
	 * - 视图集合（ECanonicalView 增减）
	 * - 任意视图的相机距离公式 / FOV
	 * - 背景颜色 / 光照设置
	 * - 曝光 / Tone curve / showflag
	 * - silhouette 算法（亮度阈值 / 通道）
	 * - 任一资产类型的渲染管线细节（material 球体 / widget 背景色 / skel 默认 pose）
	 *
	 * v2 = mesh + skel + material + widget 全部接通；与 v1 单 mesh 不兼容，整库 stale。
	 * v3 = capture mode 由 SCS_FinalToneCurveHDR 切到 SCS_BaseColor，避免 commandlet 模式下
	 *      没光照导致整图全黑、Geometric embedding 全 0 的 bug。所有像素值都会变，整库必须 stale。
	 * v4 = SCS_BaseColor 在 commandlet world 下也是黑（GBuffer 没 prime）。改用
	 *      SCS_FinalColorLDR + ShowFlags.SetLighting(false) 走 unlit 路径直出 base color。
	 */
	static constexpr uint32 RenderRecipeVersion = 4;

	/** canonical 相机 FOV 固定为 35 度，给资产留足 framing 余量。 */
	static constexpr float CanonicalFovDegrees = 35.0f;

	/** 把资产包围盒装进画面的相机距离倍率：距离 = 球半径 × 倍率。 */
	static constexpr float CanonicalCameraDistanceMultiplier = 2.6f;

	/** silhouette 判定阈值：BGRA8 的 R+G+B 三通道之和 > 该值视为前景。 */
	static constexpr uint32 SilhouetteForegroundLumaThreshold = 24;

	/** Material 预览统一贴在球体上；写死路径让 render recipe 完全可复现。 */
	static constexpr const TCHAR* MaterialPreviewMeshPath = TEXT("/Engine/BasicShapes/Sphere");

	/** Widget 渲染时的中性背景色（深灰）；与 capture_widget action 历史行为一致。 */
	static const FLinearColor WidgetBackgroundColor = FLinearColor(0.1f, 0.1f, 0.1f, 1.0f);

	/** 给一个枚举视角，返回该视角下相对资产中心的相机偏移单位向量与上方向。 */
	static void GetViewBasis(const ECanonicalView View, FVector& OutDirection, FVector& OutUp)
	{
		switch (View)
		{
		case ECanonicalView::Front:
			OutDirection = FVector(1.0, 0.0, 0.0);
			OutUp = FVector(0.0, 0.0, 1.0);
			return;
		case ECanonicalView::Side:
			OutDirection = FVector(0.0, 1.0, 0.0);
			OutUp = FVector(0.0, 0.0, 1.0);
			return;
		case ECanonicalView::Back:
			OutDirection = FVector(-1.0, 0.0, 0.0);
			OutUp = FVector(0.0, 0.0, 1.0);
			return;
		case ECanonicalView::Top:
			OutDirection = FVector(0.0, 0.0, 1.0);
			OutUp = FVector(1.0, 0.0, 0.0);
			return;
		case ECanonicalView::Iso:
		default:
			OutDirection = FVector(1.0, 0.5, 0.8).GetSafeNormal();
			OutUp = FVector(0.0, 0.0, 1.0);
			return;
		}
	}

	/** 按资产包围盒和视角，算出固定可复现的相机位置和朝向。 */
	static void ComputeCameraTransform(
		const FBoxSphereBounds& Bounds,
		const ECanonicalView View,
		FVector& OutCameraLocation,
		FRotator& OutCameraRotation)
	{
		float Radius = static_cast<float>(Bounds.SphereRadius);
		if (Radius < 1.0f)
		{
			Radius = 100.0f;
		}

		FVector Direction;
		FVector Up;
		GetViewBasis(View, Direction, Up);
		(void)Up;

		const FVector Offset = Direction * (Radius * CanonicalCameraDistanceMultiplier);
		OutCameraLocation = Bounds.Origin + Offset;
		OutCameraRotation = (Bounds.Origin - OutCameraLocation).Rotation();
	}

	/** 按 BGRA8 像素亮度阈值生成单通道 silhouette 图（前景 255、背景 0）。 */
	static void BuildSilhouette(const FImage& ColorImage, FImage& OutSilhouette)
	{
		OutSilhouette.Init(ColorImage.SizeX, ColorImage.SizeY, ERawImageFormat::G8, EGammaSpace::Linear);

		const FColor* Src = reinterpret_cast<const FColor*>(ColorImage.RawData.GetData());
		uint8* Dst = OutSilhouette.RawData.GetData();
		const int32 PixelCount = ColorImage.SizeX * ColorImage.SizeY;
		for (int32 Index = 0; Index < PixelCount; ++Index)
		{
			const FColor& C = Src[Index];
			const uint32 Luma = static_cast<uint32>(C.R) + static_cast<uint32>(C.G) + static_cast<uint32>(C.B);
			Dst[Index] = (Luma > SilhouetteForegroundLumaThreshold) ? 0xFFu : 0x00u;
		}
	}

	/** 把任意 UPrimitiveComponent（实际只用于 Static / Skel / Material）通过 SCC2D 渲染到内存图像。
	 *  这是 mesh / skel / material 三条管线共用的"最后一公里"。
	 *
	 *  历史踩坑：原本 register component 到 GEditor->GetEditorWorldContext().World()，
	 *  在 commandlet 模式下 component->GetSceneProxy() 永远是 nil（已通过诊断证实），
	 *  捕获结果是全黑 RT（连 ClearColor 都没生效）。
	 *  正解：用 FPreviewScene —— 这是 UE 自己的 thumbnail / asset preview 用的 minimal scene，
	 *  自带 directional + sky light 和真正可工作的 FScene，commandlet 也支持。 */
	static bool RenderPrimitiveComponentToImage(
		UPrimitiveComponent* PrimitiveComp,
		const FVector& CameraLocation,
		const FRotator& CameraRotation,
		const int32 Resolution,
		FImage& OutImage)
	{
		FPreviewScene::ConstructionValues PSCV;
		PSCV.bAllowAudioPlayback = false;
		PSCV.bForceMipsResident = true;
		FPreviewScene PreviewScene(PSCV);
		UWorld* World = PreviewScene.GetWorld();
		if (!World)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderPrimitiveComponentToImage: PreviewScene world 创建失败"));
			return false;
		}

		// 关键：先等所有 asset compile（StaticMesh / Shader / Texture）完成。
		// UStaticMeshComponent::ShouldCreateRenderState() 在 StaticMesh->IsCompiling() 时返回 false，
		// 不等编译完直接 register 出来的 component 不会创建 scene proxy。
		FAssetCompilingManager::Get().FinishAllCompilation();

		// 显式 RegisterComponentWithWorld(PreviewScene 的 World)。FPreviewScene::AddComponent
		// 调的是 Comp->RegisterComponent()（无 World 参），靠 Comp->GetWorld() 推断；
		// 但我们的 Comp 创建于 TransientPackage 没 World，会注册失败。
		PrimitiveComp->RegisterComponentWithWorld(World);
		PreviewScene.AddComponent(PrimitiveComp, FTransform::Identity);

		// 关键：register 后 scene proxy 是 enqueue 给 render thread 创建的。
		// 不 flush 直接调 GetSceneProxy() 永远是 nullptr。
		// 先 MarkRenderStateDirty 触发 SendRenderState，然后 flush 让 render thread 完成 CreateSceneProxy。
		PrimitiveComp->MarkRenderStateDirty();
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();

		// 一次诊断：commandlet 下到底允不允许 render，以及 component 状态。
		static FThreadSafeCounter PreFlushDiagCounter;
		const int32 PreDiagIdx = PreFlushDiagCounter.Increment();
		if (PreDiagIdx <= 5)
		{
			UE_LOG(LogMonolithCapture, Log,
				TEXT("RenderPrimitiveComponentToImage pre-capture #%d: CanEverRender=%s IsAllowCommandletRendering=%s "
				     "Comp registered=%s ShouldRender=%s scene_proxy=%s World->Scene=%s"),
				PreDiagIdx,
				FApp::CanEverRender() ? TEXT("yes") : TEXT("NO"),
				IsAllowCommandletRendering() ? TEXT("yes") : TEXT("NO"),
				PrimitiveComp->IsRegistered() ? TEXT("yes") : TEXT("NO"),
				PrimitiveComp->ShouldRender() ? TEXT("yes") : TEXT("NO"),
				PrimitiveComp->GetSceneProxy() ? TEXT("yes") : TEXT("nil"),
				World->Scene ? TEXT("yes") : TEXT("NIL"));
		}

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		RT->InitAutoFormat(Resolution, Resolution);
		RT->ClearColor = FLinearColor::Black;
		RT->UpdateResourceImmediate(true);

		USceneCaptureComponent2D* SCC = NewObject<USceneCaptureComponent2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		SCC->bTickInEditor = false;
		SCC->SetComponentTickEnabled(false);
		SCC->bCaptureEveryFrame = false;
		SCC->bCaptureOnMovement = false;
		SCC->TextureTarget = RT;
		// FPreviewScene 自带 light，可以走完整 lit 路径。SCS_FinalColorLDR + 默认 lighting 即可。
		SCC->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		SCC->ProjectionType = ECameraProjectionMode::Perspective;
		SCC->FOVAngle = CanonicalFovDegrees;

		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		SCC->ShowOnlyComponents.Add(PrimitiveComp);

		// 关掉外部光环境，避免不同项目的天气 / 大气贡献污染 cohort hash。
		SCC->ShowFlags.SetAtmosphere(false);
		SCC->ShowFlags.SetFog(false);
		SCC->ShowFlags.SetVolumetricFog(false);
		SCC->ShowFlags.SetCloud(false);

		SCC->PostProcessSettings.bOverride_AutoExposureMethod = true;
		SCC->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
		SCC->PostProcessSettings.bOverride_AutoExposureBias = true;
		SCC->PostProcessSettings.AutoExposureBias = 0.0f;
		SCC->PostProcessBlendWeight = 1.0f;

		PreviewScene.AddComponent(SCC, FTransform(CameraRotation, CameraLocation));

		// 双次 CaptureScene + FinishAllCompilation 保证 shader 编译完成，避免拿到 fallback 材质。
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		FlushRenderingCommands();
		SCC->CaptureScene();
		FlushRenderingCommands();
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		FlushRenderingCommands();
		SCC->CaptureScene();
		FlushRenderingCommands();

		FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
		if (!RTResource)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderPrimitiveComponentToImage: RenderTarget 资源为空"));
			return false;
		}

		TArray<FColor> Pixels;
		if (!RTResource->ReadPixels(Pixels) || Pixels.Num() != Resolution * Resolution)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderPrimitiveComponentToImage: ReadPixels 失败"));
			return false;
		}

		OutImage.Init(Resolution, Resolution, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FMemory::Memcpy(OutImage.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

		// 诊断日志：dump 中心 pixel 值 + 非零 pixel 数量。仅前 20 次 capture 打日志。
		static FThreadSafeCounter DiagCounter;
		const int32 DiagIdx = DiagCounter.Increment();
		if (DiagIdx <= 20)
		{
			const int32 Center = (Resolution / 2) * Resolution + (Resolution / 2);
			const FColor C = Pixels[Center];
			int32 NonZeroCount = 0;
			for (const FColor& P : Pixels)
			{
				if (P.R != 0 || P.G != 0 || P.B != 0) ++NonZeroCount;
			}
			UE_LOG(LogMonolithCapture, Log,
				TEXT("RenderPrimitiveComponentToImage diag #%d: center BGRA=(%u,%u,%u,%u) non_zero_pixels=%d / %d  scene_proxy=%s"),
				DiagIdx, C.B, C.G, C.R, C.A, NonZeroCount, Pixels.Num(),
				PrimitiveComp->GetSceneProxy() ? TEXT("yes") : TEXT("nil"));
		}

		SCC->TextureTarget = nullptr;
		SCC->UnregisterComponent();
		return true;
	}

	/** Tick 编辑器世界几帧让组件渲染状态稳定下来；mesh / skel / material 都需要。 */
	static void TickEditorWorldForRenderState(UWorld* World)
	{
		if (!World) return;
		const float TickDelta = 1.0f / 30.0f;
		for (int32 Tick = 0; Tick < 5; ++Tick)
		{
			World->Tick(LEVELTICK_TimeOnly, TickDelta);
			World->SendAllEndOfFrameUpdates();
			FlushRenderingCommands();
		}
	}

	// ---- 4 条管线 ----

	/** 静态 / 骨骼 mesh 共用的"挂组件 + 渲染多视图"流程。
	 *  通过 CreateAndAttachComponent / GetBounds 抽出资产类型差异，主体 framing/相机/SCC2D 完全共享。 */
	template <typename TComponent>
	static bool RenderMeshLikePipeline(
		UWorld* World,
		const TFunctionRef<TComponent*()>& CreateAndAttachComponent,
		const TFunctionRef<FBoxSphereBounds(TComponent*)>& GetBounds,
		const FCanonicalRenderRequest& Request,
		TArray<FCanonicalRenderResult>& OutResults)
	{
		TComponent* Comp = CreateAndAttachComponent();
		if (!Comp)
		{
			return false;
		}
		TickEditorWorldForRenderState(World);

		const FBoxSphereBounds Bounds = GetBounds(Comp);

		bool bAnyFailure = false;
		OutResults.Reserve(Request.Views.Num());
		for (const ECanonicalView View : Request.Views)
		{
			FVector CameraLocation;
			FRotator CameraRotation;
			ComputeCameraTransform(Bounds, View, CameraLocation, CameraRotation);

			FCanonicalRenderResult Result;
			Result.View = View;
			if (!RenderPrimitiveComponentToImage(Comp, CameraLocation, CameraRotation, Request.Resolution, Result.ColorImage))
			{
				bAnyFailure = true;
				break;
			}
			if (Request.bComputeSilhouette)
			{
				BuildSilhouette(Result.ColorImage, Result.SilhouetteImage);
			}
			OutResults.Add(MoveTemp(Result));
		}

		Comp->UnregisterComponent();
		return !bAnyFailure;
	}

	/** Material 管线：把 material 应用到固定球体上，再走 mesh-like 流程。
	 *  统一用球体保证 cohort 内同 material 跨次渲染 bit-identical。 */
	static bool RenderMaterialPipeline(
		UWorld* World,
		UMaterialInterface* Material,
		const FCanonicalRenderRequest& Request,
		TArray<FCanonicalRenderResult>& OutResults)
	{
		UStaticMesh* PreviewMesh = LoadObject<UStaticMesh>(nullptr, MaterialPreviewMeshPath);
		if (!PreviewMesh)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderMaterialPipeline: 加载预览球体失败 (%s)"), MaterialPreviewMeshPath);
			return false;
		}

		// 强制材质着色器编译完成；不然首次渲染会拿到 fallback 颜色。
		if (UMaterial* BaseMat = Material->GetMaterial())
		{
			BaseMat->EnsureIsComplete();
		}

		return RenderMeshLikePipeline<UStaticMeshComponent>(
			World,
			[PreviewMesh, Material]() -> UStaticMeshComponent*
			{
				// 不预 register；让 Render 内部 PreviewScene.AddComponent 唯一注册。
				UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				Comp->SetStaticMesh(PreviewMesh);
				Comp->SetMaterial(0, Material);
				Comp->CastShadow = false;
				Comp->bCastDynamicShadow = false;
				Comp->SetMobility(EComponentMobility::Movable);
				return Comp;
			},
			[](UStaticMeshComponent* Comp) -> FBoxSphereBounds
			{
				// 用预览球体自身的 bounds，所有 material 因此共享同一 framing。
				return Comp->GetStaticMesh()
					? Comp->GetStaticMesh()->GetBounds()
					: FBoxSphereBounds(FVector::ZeroVector, FVector(50.f), 50.f);
			},
			Request,
			OutResults);
	}

	/** WidgetBlueprint 管线：用 FWidgetRenderer 直接 rasterize 到 RT。
	 *  Widget 是 2D 资产，没有 3D bounds 概念，只支持 Iso 一张视图。 */
	static bool RenderWidgetPipeline(
		UWorld* World,
		UWidgetBlueprint* WidgetBP,
		const FCanonicalRenderRequest& Request,
		TArray<FCanonicalRenderResult>& OutResults)
	{
		UClass* WidgetClass = WidgetBP->GeneratedClass;
		if (!WidgetClass)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderWidgetPipeline: WidgetBlueprint 没有 GeneratedClass"));
			return false;
		}

		UUserWidget* Widget = CreateWidget<UUserWidget>(World, WidgetClass);
		if (!Widget)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderWidgetPipeline: CreateWidget 失败"));
			return false;
		}

		const TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
		if (!SlateWidget.IsValid())
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderWidgetPipeline: TakeWidget 失败"));
			return false;
		}

		// Widget 渲染按目标 Resolution 直接 rasterize（正方形）；
		// 设计稿原比例信息我们不需要保留，AssetVisual cohort 只关心固定尺寸视觉特征。
		const int32 R = Request.Resolution;

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		RT->InitCustomFormat(R, R, PF_B8G8R8A8, false);
		RT->ClearColor = WidgetBackgroundColor;
		RT->UpdateResourceImmediate(true);

		// FWidgetRenderer 第二参数 bUseGammaCorrection=true：让最终图像在 sRGB 空间。
		FWidgetRenderer WidgetRenderer(true);
		WidgetRenderer.DrawWidget(RT, SlateWidget.ToSharedRef(), FVector2D(R, R), 0.016f);
		FlushRenderingCommands();

		FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
		if (!RTResource)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderWidgetPipeline: RenderTarget 资源为空"));
			return false;
		}

		TArray<FColor> Pixels;
		if (!RTResource->ReadPixels(Pixels) || Pixels.Num() != R * R)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderWidgetPipeline: ReadPixels 失败"));
			return false;
		}

		FCanonicalRenderResult Result;
		// Widget 没有 3D 视角概念，但下游 cohort 用 ECanonicalView::Iso 标记，
		// 行为上等价于"该资产唯一可用的 canonical view"。
		Result.View = ECanonicalView::Iso;
		Result.ColorImage.Init(R, R, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FMemory::Memcpy(Result.ColorImage.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
		if (Request.bComputeSilhouette)
		{
			// Widget 背景固定深灰；silhouette 把"明显比背景亮"的像素当前景，与 mesh 路径阈值一致。
			BuildSilhouette(Result.ColorImage, Result.SilhouetteImage);
		}
		OutResults.Reset();
		OutResults.Add(MoveTemp(Result));
		return true;
	}

	/*
	 * 默认 helper 实现：实现接口的全部 3 个方法。
	 *
	 * 它故意做成无状态：所有渲染参数都在 Request 里，每次调用都从零开始。
	 * 这样调用方就不必担心"helper 是否被别人改坏了"。
	 */
	class FAssetCanonicalRenderer final : public IAssetCanonicalRenderer
	{
	public:
		virtual bool RenderCanonical(
			const FCanonicalRenderRequest& Request,
			TArray<FCanonicalRenderResult>& OutResults) override
		{
			OutResults.Reset();

			if (!Request.Asset)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("RenderCanonical: Asset 为空"));
				return false;
			}
			if (Request.Resolution <= 0 || Request.Resolution > 4096)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("RenderCanonical: Resolution=%d 越界"), Request.Resolution);
				return false;
			}

			// v11 实现策略：放弃 4 类 SCC2D pipeline，直接用 UE 自己的 UThumbnailManager。
			// SCC2D 在 commandlet 模式下注册的 component 永远拿不到 scene proxy（确认过 v6..v10 各种 fix 都不奏效）。
			// UThumbnailManager + UThumbnailRenderer 是 UE 自己 cook / project browser 用的标准 thumbnail 路径，
			// 已知在 commandlet（含 ResavePackages cook 阶段）正常工作，并对每种 asset 类自动选合适的 renderer
			// （UStaticMeshThumbnailRenderer / UMaterialThumbnailRenderer / USkeletalMeshThumbnailRenderer / UWidgetBlueprintThumbnailRenderer）。
			//
			// 这等于让 UE 替我们处理"4 类资产分发 + scene proxy 创建 + lighting + framing"。
			// 代价：framing 是 thumbnail manager 默认的（不是我们之前自定义的 35° FOV / 2.6× 距离），
			// 跨 cohort 一致性靠它本身的稳定性即可。

			if (!GIsRHIInitialized)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("RenderCanonical: RHI 未初始化"));
				return false;
			}

			// 用 UThumbnailManager::Get() 静态访问，绕开 GUnrealEd（commandlet 模式 GUnrealEd 是 nil）。
			UThumbnailManager* TM = UThumbnailManager::TryGet();
			if (!TM)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("RenderCanonical: UThumbnailManager 未就绪"));
				return false;
			}

			FThumbnailRenderingInfo* RenderInfo = TM->GetRenderingInfo(Request.Asset);
			if (!RenderInfo || !RenderInfo->Renderer)
			{
				UE_LOG(LogMonolithCapture, Verbose,
					TEXT("RenderCanonical: 资产 %s 无注册 thumbnail renderer，跳过"),
					*Request.Asset->GetPathName());
				return false;
			}

			// 等所有 asset 编译完成（mesh / shader / texture）。这是 ThumbnailManager 自己也用的 prep 路径。
			FAssetCompilingManager::Get().FinishAllCompilation();

			// 创建临时 RT；用 manager 自己 RTPool 也行但每次新建更隔离。
			UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
			RT->InitCustomFormat(Request.Resolution, Request.Resolution, PF_B8G8R8A8, /*bForceLinearGamma=*/false);
			RT->ClearColor = FLinearColor::Black;
			RT->TargetGamma = 2.2f;
			RT->UpdateResourceImmediate(true);

			FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
			if (!RTResource)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("RenderCanonical: RT 资源为空"));
				return false;
			}

			// 与 ObjectTools::RenderThumbnail（UE 自己的 thumbnail save 路径）行为完全一致：
			FCanvas Canvas(RTResource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
			Canvas.Clear(FLinearColor::Black);

			// 抑制 thumbnail renderer 内部可能弹出的 message dialog（commandlet 模式致命）。
			TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
			RenderInfo->Renderer->Draw(
				Request.Asset, 0, 0, Request.Resolution, Request.Resolution,
				RTResource, &Canvas, /*bAdditionalViewFamily=*/false);
			Canvas.Flush_GameThread();
			FlushRenderingCommands();

			TArray<FColor> Pixels;
			if (!RTResource->ReadPixels(Pixels) || Pixels.Num() != Request.Resolution * Request.Resolution)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("RenderCanonical: ReadPixels 失败"));
				return false;
			}

			// 诊断日志：前 5 次打中心 pixel 和非零数。
			static FThreadSafeCounter ThumbnailDiagCounter;
			const int32 DiagIdx = ThumbnailDiagCounter.Increment();
			if (DiagIdx <= 5)
			{
				const int32 Center = (Request.Resolution / 2) * Request.Resolution + (Request.Resolution / 2);
				const FColor C = Pixels[Center];
				int32 NonZero = 0;
				for (const FColor& P : Pixels)
				{
					if (P.R != 0 || P.G != 0 || P.B != 0) ++NonZero;
				}
				UE_LOG(LogMonolithCapture, Log,
					TEXT("RenderCanonical thumbnail diag #%d: asset=%s class=%s center BGRA=(%u,%u,%u,%u) non_zero=%d / %d"),
					DiagIdx, *Request.Asset->GetName(), *Request.Asset->GetClass()->GetName(),
					C.B, C.G, C.R, C.A, NonZero, Pixels.Num());
			}

			// 装配 OutResult；只产 Iso 一张视图（thumbnail manager 不支持 5 视图，warmup 流程 spec 也只要 Iso）。
			FCanonicalRenderResult Result;
			Result.View = ECanonicalView::Iso;
			Result.ColorImage.Init(Request.Resolution, Request.Resolution, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FMemory::Memcpy(Result.ColorImage.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			if (Request.bComputeSilhouette)
			{
				BuildSilhouette(Result.ColorImage, Result.SilhouetteImage);
			}
			OutResults.Add(MoveTemp(Result));
			return true;
		}

		virtual bool SaveImageAsPng(const FImage& Image, const FString& OutputPath) override
		{
			if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.RawData.Num() == 0)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("SaveImageAsPng: 输入图像为空"));
				return false;
			}

			const FString Dir = FPaths::GetPath(OutputPath);
			IFileManager::Get().MakeDirectory(*Dir, true);
			return FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
		}

		virtual uint32 GetRenderRecipeVersion() const override
		{
			return RenderRecipeVersion;
		}
	};

	/** 全局单例：FAssetCanonicalRenderer 是无状态的，单例完全安全。 */
	static FAssetCanonicalRenderer& Get()
	{
		static FAssetCanonicalRenderer Instance;
		return Instance;
	}
}

namespace MonolithCapture
{
	IAssetCanonicalRenderer& GetAssetCanonicalRenderer()
	{
		return MonolithCaptureRendererInternal::Get();
	}
}
