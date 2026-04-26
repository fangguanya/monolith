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
#include "RenderingThread.h"
#include "ShaderCompiler.h"
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
	 */
	static constexpr uint32 RenderRecipeVersion = 2;

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
	 *  这是 mesh / skel / material 三条管线共用的"最后一公里"。 */
	static bool RenderPrimitiveComponentToImage(
		UPrimitiveComponent* PrimitiveComp,
		const FVector& CameraLocation,
		const FRotator& CameraRotation,
		const int32 Resolution,
		FImage& OutImage)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderPrimitiveComponentToImage: 编辑器世界不可用"));
			return false;
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
		SCC->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
		SCC->ProjectionType = ECameraProjectionMode::Perspective;
		SCC->FOVAngle = CanonicalFovDegrees;

		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		SCC->ShowOnlyComponents.Add(PrimitiveComp);

		SCC->ShowFlags.SetAtmosphere(false);
		SCC->ShowFlags.SetFog(false);
		SCC->ShowFlags.SetVolumetricFog(false);
		SCC->ShowFlags.SetCloud(false);

		SCC->PostProcessSettings.bOverride_AutoExposureMethod = true;
		SCC->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
		SCC->PostProcessSettings.bOverride_AutoExposureBias = true;
		SCC->PostProcessSettings.AutoExposureBias = 0.0f;
		SCC->PostProcessBlendWeight = 1.0f;

		SCC->RegisterComponentWithWorld(World);
		SCC->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

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
			SCC->TextureTarget = nullptr;
			SCC->UnregisterComponent();
			return false;
		}

		TArray<FColor> Pixels;
		if (!RTResource->ReadPixels(Pixels) || Pixels.Num() != Resolution * Resolution)
		{
			UE_LOG(LogMonolithCapture, Error, TEXT("RenderPrimitiveComponentToImage: ReadPixels 失败"));
			SCC->TextureTarget = nullptr;
			SCC->UnregisterComponent();
			return false;
		}

		OutImage.Init(Resolution, Resolution, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FMemory::Memcpy(OutImage.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

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
			[World, PreviewMesh, Material]() -> UStaticMeshComponent*
			{
				UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				Comp->SetStaticMesh(PreviewMesh);
				Comp->SetMaterial(0, Material);
				Comp->CastShadow = false;
				Comp->bCastDynamicShadow = false;
				Comp->SetMobility(EComponentMobility::Movable);
				Comp->RegisterComponentWithWorld(World);
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

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				UE_LOG(LogMonolithCapture, Error, TEXT("RenderCanonical: 编辑器世界不可用"));
				return false;
			}

			// 视图集合空时退化为只渲染 Iso 一张，配合 semantic provider 默认行为。
			FCanonicalRenderRequest EffectiveRequest = Request;
			if (EffectiveRequest.Views.Num() == 0)
			{
				EffectiveRequest.Views.Add(ECanonicalView::Iso);
			}

			// 按运行时类型分发到对应管线。
			// 顺序：从最具体到最通用，避免父类 Cast 误命中。
			if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(EffectiveRequest.Asset))
			{
				return RenderMeshLikePipeline<UStaticMeshComponent>(
					World,
					[World, StaticMesh]() -> UStaticMeshComponent*
					{
						UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
						Comp->SetStaticMesh(StaticMesh);
						Comp->CastShadow = false;
						Comp->bCastDynamicShadow = false;
						Comp->SetMobility(EComponentMobility::Movable);
						Comp->RegisterComponentWithWorld(World);
						return Comp;
					},
					[StaticMesh](UStaticMeshComponent*) -> FBoxSphereBounds
					{
						return StaticMesh->GetBounds();
					},
					EffectiveRequest,
					OutResults);
			}

			if (USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(EffectiveRequest.Asset))
			{
				return RenderMeshLikePipeline<USkeletalMeshComponent>(
					World,
					[World, SkelMesh]() -> USkeletalMeshComponent*
					{
						USkeletalMeshComponent* Comp = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
						Comp->SetSkeletalMeshAsset(SkelMesh);
						Comp->CastShadow = false;
						Comp->bCastDynamicShadow = false;
						Comp->SetMobility(EComponentMobility::Movable);
						// 用 ref pose；不播任何动画，保证 cohort 内同资产跨次渲染 bit-identical。
						Comp->RegisterComponentWithWorld(World);
						return Comp;
					},
					[SkelMesh](USkeletalMeshComponent*) -> FBoxSphereBounds
					{
						return SkelMesh->GetBounds();
					},
					EffectiveRequest,
					OutResults);
			}

			if (UMaterialInterface* Material = Cast<UMaterialInterface>(EffectiveRequest.Asset))
			{
				return RenderMaterialPipeline(World, Material, EffectiveRequest, OutResults);
			}

			if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(EffectiveRequest.Asset))
			{
				return RenderWidgetPipeline(World, WidgetBP, EffectiveRequest, OutResults);
			}

			UE_LOG(LogMonolithCapture, Error,
				TEXT("RenderCanonical: 不支持的资产类型 %s（仅支持 StaticMesh / SkeletalMesh / MaterialInterface / WidgetBlueprint）"),
				*EffectiveRequest.Asset->GetClass()->GetName());
			return false;
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
