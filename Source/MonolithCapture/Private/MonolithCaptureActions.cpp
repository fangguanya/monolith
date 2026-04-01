#include "MonolithCaptureActions.h"
#include "MonolithCaptureModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

// 编辑器
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "IAssetViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "UnrealClient.h"

// 截屏
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"

// 资产截屏
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraWorldManager.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/SkeletalMeshActor.h"
#include "Animation/AnimSequence.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"

// UMG Widget 截屏
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"
#include "Slate/WidgetRenderer.h"

// IO
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ===== 参数解析辅助 =====

void FMonolithCaptureActions::ParseCameraParams(
	const TSharedPtr<FJsonObject>& Params,
	FVector& OutLocation, FRotator& OutRotation, float& OutFOV)
{
	OutLocation = FVector(200.0f, 0.0f, 100.0f);
	OutRotation = FRotator(0.0f, 180.0f, 0.0f);
	OutFOV = 60.0f;

	if (!Params->HasField(TEXT("camera"))) return;

	const TSharedPtr<FJsonObject>* CameraObj = nullptr;
	TSharedPtr<FJsonObject> ParsedCamera;

	if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
	{
		FString CameraStr = Params->GetStringField(TEXT("camera"));
		if (!CameraStr.IsEmpty())
		{
			ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
			CameraObj = &ParsedCamera;
		}
	}

	if (CameraObj && (*CameraObj).IsValid())
	{
		if ((*CameraObj)->HasField(TEXT("location")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Loc = (*CameraObj)->GetArrayField(TEXT("location"));
			if (Loc.Num() >= 3)
				OutLocation = FVector(Loc[0]->AsNumber(), Loc[1]->AsNumber(), Loc[2]->AsNumber());
		}
		if ((*CameraObj)->HasField(TEXT("rotation")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Rot = (*CameraObj)->GetArrayField(TEXT("rotation"));
			if (Rot.Num() >= 3)
				OutRotation = FRotator(Rot[0]->AsNumber(), Rot[1]->AsNumber(), Rot[2]->AsNumber());
		}
		if ((*CameraObj)->HasField(TEXT("fov")))
		{
			OutFOV = (float)(*CameraObj)->GetNumberField(TEXT("fov"));
		}
	}
}

void FMonolithCaptureActions::ParseResolutionParams(
	const TSharedPtr<FJsonObject>& Params,
	int32& OutResX, int32& OutResY)
{
	OutResX = 512;
	OutResY = 512;

	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ResArray = Params->GetArrayField(TEXT("resolution"));
		if (ResArray.Num() >= 2)
		{
			OutResX = (int32)ResArray[0]->AsNumber();
			OutResY = (int32)ResArray[1]->AsNumber();
		}
	}
}

FString FMonolithCaptureActions::ResolveOutputPath(
	const TSharedPtr<FJsonObject>& Params,
	const FString& AssetPath, const FString& Suffix)
{
	if (Params->HasField(TEXT("output_path")))
	{
		FString OutputPath = Params->GetStringField(TEXT("output_path"));
		if (FPaths::IsRelative(OutputPath))
		{
			OutputPath = FPaths::ProjectDir() / OutputPath;
		}
		return OutputPath;
	}

	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	FString SafeName = FPaths::GetBaseFilename(AssetPath);
	return FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
		FString::Printf(TEXT("%s_%s%s.png"), *Timestamp, *SafeName, *Suffix);
}

FString FMonolithCaptureActions::ResolveArtifactRoot(const TSharedPtr<FJsonObject>& Params)
{
	if (Params.IsValid() && Params->HasField(TEXT("artifact_root")))
	{
		return Params->GetStringField(TEXT("artifact_root"));
	}

	const FString Capability = Params.IsValid() && Params->HasField(TEXT("capability"))
		? Params->GetStringField(TEXT("capability"))
		: TEXT("platform");
	if (Params.IsValid() && Params->HasField(TEXT("run_id")))
	{
		const FString RunId = Params->GetStringField(TEXT("run_id"));
		if (!RunId.IsEmpty())
		{
			return FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AI"), TEXT("runs"), RunId, Capability);
		}
	}

	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AI"), TEXT("screenshots"));
}

// ===== Actions 注册 =====

void FMonolithCaptureActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// --- 地图管理 ---
	Registry.RegisterAction(TEXT("capture"), TEXT("get_current_map"),
		TEXT("获取当前打开地图的名称和路径"),
		FMonolithActionHandler::CreateStatic(&HandleGetCurrentMap),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("capture"), TEXT("open_map"),
		TEXT("打开指定地图"),
		FMonolithActionHandler::CreateStatic(&HandleOpenMap),
		FParamSchemaBuilder()
			.Required(TEXT("map_path"), TEXT("string"), TEXT("地图资产路径，例如 /Game/Maps/DEMO1"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("save_map"),
		TEXT("保存当前打开的地图"),
		FMonolithActionHandler::CreateStatic(&HandleSaveMap),
		MakeShared<FJsonObject>());

	// --- 视口截屏 ---
	Registry.RegisterAction(TEXT("capture"), TEXT("capture_viewport"),
		TEXT("抓取当前主关卡编辑器视口截图"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureViewport),
		FParamSchemaBuilder()
			.Optional(TEXT("capability"), TEXT("string"), TEXT("artifact 路径分类使用的 capability 名称"), TEXT("platform"))
			.Optional(TEXT("run_id"), TEXT("string"), TEXT("artifact 归档使用的 run-id"))
			.Optional(TEXT("artifact_root"), TEXT("string"), TEXT("显式 artifact 输出目录"))
			.Optional(TEXT("camera_location"), TEXT("object"), TEXT("相机位置对象 {x,y,z}"))
			.Optional(TEXT("camera_rotation"), TEXT("object"), TEXT("相机朝向对象 {pitch,yaw,roll}"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("set_viewport_camera"),
		TEXT("设置当前编辑器主视口的相机位置和朝向"),
		FMonolithActionHandler::CreateStatic(&HandleSetViewportCamera),
		FParamSchemaBuilder()
			.Optional(TEXT("location"), TEXT("object"), TEXT("相机位置对象 {x,y,z}"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("相机朝向对象 {pitch,yaw,roll}"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("list_run_artifacts"),
		TEXT("列举指定 run-id 下的所有 artifact 文件"),
		FMonolithActionHandler::CreateStatic(&HandleListRunArtifacts),
		FParamSchemaBuilder()
			.Required(TEXT("run_id"), TEXT("string"), TEXT("要列举的 run-id"))
			.Build());

	// --- 资产预览截图 ---
	Registry.RegisterAction(TEXT("capture"), TEXT("capture_static_mesh"),
		TEXT("对 StaticMesh 资产进行离屏 3D 预览截图"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureStaticMesh),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StaticMesh 资产路径"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("输出 PNG 路径"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("capture_skeletal_mesh"),
		TEXT("对 SkeletalMesh 资产进行离屏预览截图，支持显示骨骼"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSkeletalMesh),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("SkeletalMesh 资产路径"))
			.Optional(TEXT("show_bones"), TEXT("bool"), TEXT("是否显示骨骼叠加"), TEXT("false"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("输出 PNG 路径"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("capture_animation"),
		TEXT("对 AnimSequence 进行多帧采样截图"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureAnimation),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("AnimSequence 资产路径"))
			.Optional(TEXT("timestamps"), TEXT("array"), TEXT("采样时间点数组（秒）"), TEXT("[0.0]"))
			.Optional(TEXT("show_bones"), TEXT("bool"), TEXT("是否显示骨骼叠加"), TEXT("false"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_dir"), TEXT("string"), TEXT("多帧输出目录"))
			.Optional(TEXT("filename_prefix"), TEXT("string"), TEXT("帧文件名前缀"), TEXT("frame"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("capture_niagara"),
		TEXT("对 Niagara 粒子系统进行预览截图"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureNiagara),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara 系统资产路径"))
			.Optional(TEXT("seek_time"), TEXT("number"), TEXT("粒子模拟推进时间（秒）"), TEXT("1.0"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("输出 PNG 路径"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("capture_material"),
		TEXT("对 Material 资产进行预览截图"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material 资产路径"))
			.Optional(TEXT("preview_mesh"), TEXT("string"), TEXT("预览 Mesh: plane, sphere, cube"), TEXT("sphere"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("输出 PNG 路径"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("capture_widget"),
		TEXT("对 WidgetBlueprint 进行设计器预览截图"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureWidget),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("WidgetBlueprint 资产路径"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[1920,1080]"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("输出 PNG 路径"))
			.Build());

	Registry.RegisterAction(TEXT("capture"), TEXT("capture_sequence_frames"),
		TEXT("对支持时间轴的资产进行多帧序列截图"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSequenceFrames),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("资产路径"))
			.Required(TEXT("asset_type"), TEXT("string"), TEXT("niagara | animation"))
			.Required(TEXT("timestamps"), TEXT("array"), TEXT("采样时间点数组（秒）"))
			.Optional(TEXT("show_bones"), TEXT("bool"), TEXT("动画截图时是否显示骨骼"), TEXT("false"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.Optional(TEXT("output_dir"), TEXT("string"), TEXT("输出目录"))
			.Optional(TEXT("filename_prefix"), TEXT("string"), TEXT("帧文件名前缀"), TEXT("frame"))
			.Build());
}

// ===== 截图辅助 =====

bool FMonolithCaptureActions::RenderAndSaveCapture(
	USceneCaptureComponent2D* CaptureComp,
	UTextureRenderTarget2D* RT,
	int32 ResX, int32 ResY,
	const FString& OutputPath)
{
	if (!CaptureComp || !RT) return false;

	CaptureComp->CaptureScene();

	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("RenderAndSaveCapture: Failed to get RT resource"));
		return false;
	}

	TArray<FColor> Pixels;
	bool bReadOk = RTResource->ReadPixels(Pixels);

	if (!bReadOk || Pixels.Num() == 0)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("RenderAndSaveCapture: ReadPixels failed"));
		return false;
	}

	FString Dir = FPaths::GetPath(OutputPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	FImage Image;
	Image.Init(ResX, ResY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

	return FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
}

// 在编辑器世界中使用 SCC2D + ShowOnlyList 截取指定组件（通用辅助函数）
// 双次捕获：第一次触发着色器编译，第二次使用已编译着色器产出最终图像。
// Component 必须已经通过 RegisterComponentWithWorld 注册到编辑器世界中。
static bool CaptureComponentInEditorWorld(
	UPrimitiveComponent* Component,
	const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
	int32 ResX, int32 ResY, const FString& OutputPath)
{
	if (!Component) return false;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureComponentInEditorWorld: 编辑器世界不可用"));
		return false;
	}

	// Tick 确保组件渲染状态初始化
	const float TickDelta = 1.0f / 30.0f;
	for (int32 i = 0; i < 5; i++)
	{
		World->Tick(LEVELTICK_TimeOnly, TickDelta);
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();
	}

	// 创建渲染目标
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor::Black;
	RT->UpdateResourceImmediate(true);

	// 创建 SCC2D，仅渲染目标组件（ShowOnlyList）
	USceneCaptureComponent2D* SCC = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	SCC->bTickInEditor = false;
	SCC->SetComponentTickEnabled(false);
	SCC->bCaptureEveryFrame = false;
	SCC->bCaptureOnMovement = false;
	SCC->TextureTarget = RT;
	SCC->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
	SCC->ProjectionType = ECameraProjectionMode::Perspective;
	SCC->FOVAngle = FOV;

	// 仅渲染目标组件
	SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	SCC->ShowOnlyComponents.Add(Component);

	// 隐藏大气/雾等环境效果，保留天空光照以提供环境光
	SCC->ShowFlags.SetAtmosphere(false);
	SCC->ShowFlags.SetFog(false);
	SCC->ShowFlags.SetVolumetricFog(false);
	SCC->ShowFlags.SetCloud(false);

	// 固定曝光
	SCC->PostProcessSettings.bOverride_AutoExposureMethod = true;
	SCC->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	SCC->PostProcessSettings.bOverride_AutoExposureBias = true;
	SCC->PostProcessSettings.AutoExposureBias = 0.0f;
	SCC->PostProcessBlendWeight = 1.0f;

	SCC->RegisterComponentWithWorld(World);
	SCC->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// 确保着色器就绪
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	FlushRenderingCommands();

	// 第一次截屏——触发残余着色器编译
	SCC->CaptureScene();
	FlushRenderingCommands();

	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	FlushRenderingCommands();

	// 第二次截屏——使用已编译着色器产出最终图像
	SCC->CaptureScene();
	FlushRenderingCommands();

	// 读取像素
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureComponentInEditorWorld: 无法获取 RenderTarget 资源"));
		SCC->TextureTarget = nullptr;
		SCC->UnregisterComponent();
		return false;
	}

	TArray<FColor> Pixels;
	if (!RTResource->ReadPixels(Pixels) || Pixels.Num() == 0)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureComponentInEditorWorld: ReadPixels 失败"));
		SCC->TextureTarget = nullptr;
		SCC->UnregisterComponent();
		return false;
	}

	FString Dir = FPaths::GetPath(OutputPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	FImage Image;
	Image.Init(ResX, ResY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

	bool bSaved = FImageUtils::SaveImageAutoFormat(*OutputPath, Image);

	SCC->TextureTarget = nullptr;
	SCC->UnregisterComponent();

	return bSaved;
}

bool FMonolithCaptureActions::CaptureNiagaraFrame(
	UNiagaraSystem* System, float SeekTime,
	const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
	int32 ResX, int32 ResY, const FString& OutputPath,
	ESceneCaptureSource CaptureSource)
{
	if (!System) return false;

	// 在编辑器世界中渲染 Niagara（GPU 粒子需要完整渲染管线支持）
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureNiagaraFrame: 编辑器世界不可用"));
		return false;
	}

	// 创建 Niagara 组件并注册到编辑器世界
	UNiagaraComponent* NiagaraComp = NewObject<UNiagaraComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	NiagaraComp->CastShadow = false;
	NiagaraComp->bCastDynamicShadow = false;
	NiagaraComp->SetAllowScalability(false);
	NiagaraComp->SetAsset(System);
	NiagaraComp->SetForceSolo(true);
	NiagaraComp->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
	NiagaraComp->SetCanRenderWhileSeeking(true);
	NiagaraComp->SetMaxSimTime(0.0f);
	NiagaraComp->RegisterComponentWithWorld(World);
	NiagaraComp->Activate(true);
	NiagaraComp->ResetSystem();

	// 推进粒子模拟：先通过 World Tick 初始化系统，再 Seek 到目标时间
	const float TickDelta = 1.0f / 30.0f;
	constexpr int32 InitFrames = 3;
	for (int32 i = 0; i < InitFrames; i++)
	{
		World->Tick(ELevelTick::LEVELTICK_TimeOnly, TickDelta);
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();
	}

	if (SeekTime > 0.0f)
	{
		NiagaraComp->SetSeekDelta(TickDelta);
		NiagaraComp->SeekToDesiredAge(SeekTime);
		NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);
		World->SendAllEndOfFrameUpdates();
		if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
		{
			WorldManager->FlushComputeAndDeferredQueues(true);
		}
	}

	// Warm-up：多帧 Tick 确保 GPU 粒子缓冲已填充
	constexpr int32 WarmUpFrames = 5;
	for (int32 i = 0; i < WarmUpFrames; i++)
	{
		World->Tick(ELevelTick::LEVELTICK_TimeOnly, TickDelta);
		NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);
		World->SendAllEndOfFrameUpdates();
		if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
		{
			WorldManager->FlushComputeAndDeferredQueues(true);
		}
		FlushRenderingCommands();
	}

	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	FlushRenderingCommands();

	UE_LOG(LogMonolithCapture, Log, TEXT("CaptureNiagaraFrame: IsActive=%d IsComplete=%d NumEmitters=%d"),
		NiagaraComp->IsActive(), NiagaraComp->IsComplete(),
		System->GetNumEmitters());

	// 离屏渲染——使用 ShowOnlyComponent 仅捕获 Niagara 效果
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f);
	RT->UpdateResourceImmediate(true);

	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;

	// GPU 粒子在 ShowOnlyList 模式下可能不渲染，使用 HiddenActors 隐藏场景 actor
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
	// 隐藏场景中已有的 actor，只保留 Niagara 组件可见
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		CaptureComp->HiddenActors.Add(*It);
	}

	// 隐藏大气/雾等环境效果，使粒子在黑色背景上可见
	CaptureComp->ShowFlags.SetAtmosphere(false);
	CaptureComp->ShowFlags.SetFog(false);
	CaptureComp->ShowFlags.SetVolumetricFog(false);
	CaptureComp->ShowFlags.SetSkyLighting(false);
	CaptureComp->ShowFlags.SetCloud(false);

	CaptureComp->PostProcessSettings.bOverride_AutoExposureMethod = true;
	CaptureComp->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	CaptureComp->PostProcessSettings.bOverride_AutoExposureBias = true;
	CaptureComp->PostProcessSettings.AutoExposureBias = 0.0f;
	CaptureComp->PostProcessBlendWeight = 1.0f;

	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// 双次捕获：第一次触发着色器编译，第二次产出最终图像
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	FlushRenderingCommands();

	CaptureComp->CaptureScene();
	FlushRenderingCommands();

	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	FlushRenderingCommands();

	CaptureComp->CaptureScene();
	FlushRenderingCommands();

	// 读取像素并保存
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	bool bSuccess = false;
	if (RTResource)
	{
		TArray<FColor> Pixels;
		if (RTResource->ReadPixels(Pixels) && Pixels.Num() > 0)
		{
			FString Dir = FPaths::GetPath(OutputPath);
			IFileManager::Get().MakeDirectory(*Dir, true);

			FImage Image;
			Image.Init(ResX, ResY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
			bSuccess = FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
		}
	}

	// 清理临时组件
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	NiagaraComp->Deactivate();
	NiagaraComp->UnregisterComponent();

	return bSuccess;
}

bool FMonolithCaptureActions::CaptureMaterialFrame(
	UMaterialInterface* Material, const FString& MeshType,
	const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
	int32 ResX, int32 ResY, const FString& OutputPath,
	ESceneCaptureSource CaptureSource)
{
	if (!Material) return false;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureMaterialFrame: 编辑器世界不可用"));
		return false;
	}

	// 加载预览形状
	UStaticMesh* PreviewMesh = nullptr;
	if (MeshType.Equals(TEXT("cube"), ESearchCase::IgnoreCase))
	{
		PreviewMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube"));
	}
	else if (MeshType.Equals(TEXT("plane"), ESearchCase::IgnoreCase))
	{
		PreviewMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane"));
	}
	else
	{
		PreviewMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere"));
	}
	if (!PreviewMesh)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureMaterialFrame: 无法加载预览形状"));
		return false;
	}

	// 确保材质着色器编译完成
	if (UMaterial* BaseMat = Material->GetMaterial())
	{
		BaseMat->EnsureIsComplete();
	}

	UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	MeshComp->SetStaticMesh(PreviewMesh);
	MeshComp->SetMaterial(0, const_cast<UMaterialInterface*>(Material));
	MeshComp->CastShadow = false;
	MeshComp->bCastDynamicShadow = false;
	MeshComp->SetMobility(EComponentMobility::Movable);
	MeshComp->RegisterComponentWithWorld(World);

	bool bSuccess = CaptureComponentInEditorWorld(
		MeshComp, CameraLocation, CameraRotation, FOV, ResX, ResY, OutputPath);

	MeshComp->UnregisterComponent();
	return bSuccess;
}

bool FMonolithCaptureActions::CaptureStaticMeshFrame(
	UStaticMesh* Mesh,
	const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
	int32 ResX, int32 ResY, const FString& OutputPath,
	ESceneCaptureSource CaptureSource)
{
	if (!Mesh) return false;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureStaticMeshFrame: 编辑器世界不可用"));
		return false;
	}

	UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	MeshComp->SetStaticMesh(Mesh);
	MeshComp->CastShadow = false;
	MeshComp->bCastDynamicShadow = false;
	MeshComp->SetMobility(EComponentMobility::Movable);
	MeshComp->RegisterComponentWithWorld(World);

	bool bSuccess = CaptureComponentInEditorWorld(
		MeshComp, CameraLocation, CameraRotation, FOV, ResX, ResY, OutputPath);

	MeshComp->UnregisterComponent();
	return bSuccess;
}

bool FMonolithCaptureActions::CaptureSkeletalMeshFrame(
	USkeletalMesh* Mesh, UAnimSequence* Anim, float Time, bool bShowBones,
	const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
	int32 ResX, int32 ResY, const FString& OutputPath,
	ESceneCaptureSource CaptureSource)
{
	if (!Mesh) return false;

	// 确保渲染资源和材质已就绪
	if (Mesh->GetResourceForRendering() == nullptr)
	{
		Mesh->InitResources();
	}
	FlushAsyncLoading();
	if (UPackage* MeshPackage = Mesh->GetOutermost())
	{
		MeshPackage->FullyLoad();
	}
	Mesh->ConditionalPostLoad();

	for (const FSkeletalMaterial& SkelMat : Mesh->GetMaterials())
	{
		if (UMaterialInterface* Mat = SkelMat.MaterialInterface)
		{
			if (UMaterial* BaseMat = Mat->GetMaterial())
			{
				BaseMat->EnsureIsComplete();
			}
		}
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogMonolithCapture, Error, TEXT("CaptureSkeletalMeshFrame: 编辑器世界不可用"));
		return false;
	}

	USkeletalMeshComponent* SkelComp = NewObject<USkeletalMeshComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	SkelComp->SetSkeletalMesh(Mesh);
	SkelComp->CastShadow = false;
	SkelComp->bCastDynamicShadow = false;
	SkelComp->SetMobility(EComponentMobility::Movable);
	SkelComp->bPauseAnims = false;
	SkelComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	SkelComp->RegisterComponentWithWorld(World);

	// 初始化骨骼动画系统后再设置动画和时间
	SkelComp->InitAnim(true);
	if (Anim)
	{
		SkelComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		SkelComp->PlayAnimation(Anim, false);
	}

	// 使用目标时间作为 Tick delta 推进动画到指定时间点
	SkelComp->TickComponent(FMath::Max(Time, 0.016f), ELevelTick::LEVELTICK_All, nullptr);
	// 确保骨骼变换更新到渲染线程
	SkelComp->RefreshBoneTransforms();
	SkelComp->MarkRenderStateDirty();
	SkelComp->MarkRenderDynamicDataDirty();

	bool bSuccess = CaptureComponentInEditorWorld(
		SkelComp, CameraLocation, CameraRotation, FOV, ResX, ResY, OutputPath);

	SkelComp->UnregisterComponent();
	return bSuccess;
}

// ===== 地图管理 =====

FMonolithActionResult FMonolithCaptureActions::HandleGetCurrentMap(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("map_name"), World->GetMapName());
	Result->SetStringField(TEXT("map_path"), World->GetOutermost()->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleOpenMap(const TSharedPtr<FJsonObject>& Params)
{
	const FString MapPath = Params.IsValid() ? Params->GetStringField(TEXT("map_path")) : FString();
	if (MapPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("缺少 map_path 参数"));
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	}

	FEditorFileUtils::LoadMap(MapPath, false, true);
	UWorld* NewWorld = GEditor->GetEditorWorldContext().World();
	if (NewWorld && NewWorld->GetOutermost()->GetName().Contains(FPaths::GetBaseFilename(MapPath)))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("map_path"), NewWorld->GetOutermost()->GetName());
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(FString::Printf(TEXT("打开地图失败: %s"), *MapPath));
}

FMonolithActionResult FMonolithCaptureActions::HandleSaveMap(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));
	}

	UPackage* MapPackage = World->GetOutermost();
	if (MapPackage && !MapPackage->GetName().StartsWith(TEXT("/Temp/")) && FEditorFileUtils::SaveMap(World, MapPackage->GetName()))
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("map_path"), MapPackage->GetName());
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(TEXT("保存地图失败"));
}

// ===== 视口截屏 =====

FMonolithActionResult FMonolithCaptureActions::HandleCaptureViewport(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	}

	FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<IAssetViewport> ActiveViewport = LevelEditor.GetFirstActiveViewport();
	if (!ActiveViewport.IsValid())
	{
		if (TSharedPtr<FTabManager> TabManager = LevelEditor.GetLevelEditorTabManager())
		{
			TabManager->TryInvokeTab(FTabId(TEXT("LevelEditorViewport")));
			for (int32 i = 0; i < 5; ++i)
			{
				FSlateApplication::Get().Tick();
			}
			ActiveViewport = LevelEditor.GetFirstActiveViewport();
		}
	}

	if (!ActiveViewport.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("没有可用的编辑器视口"));
	}

	FViewport* Viewport = ActiveViewport->GetActiveViewport();
	if (!Viewport)
	{
		return FMonolithActionResult::Error(TEXT("视口不可用"));
	}

	FLevelEditorViewportClient* CaptureViewportClient = nullptr;
	const TArray<FLevelEditorViewportClient*>& ViewportClients = GEditor->GetLevelViewportClients();
	for (FLevelEditorViewportClient* VC : ViewportClients)
	{
		if (VC && VC->Viewport == Viewport)
		{
			CaptureViewportClient = VC;
			break;
		}
	}

	if (!CaptureViewportClient)
	{
		for (FLevelEditorViewportClient* VC : ViewportClients)
		{
			if (!VC) continue;
			CaptureViewportClient = VC;
			if (VC->Viewport)
			{
				Viewport = VC->Viewport;
			}
			break;
		}
	}

	if (!CaptureViewportClient)
	{
		return FMonolithActionResult::Error(TEXT("没有可用的视口客户端"));
	}

	// 设置相机
	if (Params.IsValid())
	{
		if (const TSharedPtr<FJsonObject>* LocationObject = nullptr;
			Params->TryGetObjectField(TEXT("camera_location"), LocationObject))
		{
			FVector Location;
			Location.X = (*LocationObject)->GetNumberField(TEXT("x"));
			Location.Y = (*LocationObject)->GetNumberField(TEXT("y"));
			Location.Z = (*LocationObject)->GetNumberField(TEXT("z"));
			CaptureViewportClient->SetViewLocation(Location);
		}

		if (const TSharedPtr<FJsonObject>* RotationObject = nullptr;
			Params->TryGetObjectField(TEXT("camera_rotation"), RotationObject))
		{
			FRotator Rotation;
			Rotation.Pitch = (*RotationObject)->GetNumberField(TEXT("pitch"));
			Rotation.Yaw = (*RotationObject)->GetNumberField(TEXT("yaw"));
			Rotation.Roll = (*RotationObject)->GetNumberField(TEXT("roll"));
			CaptureViewportClient->SetViewRotation(Rotation);
		}

		if (CaptureViewportClient->Viewport)
		{
			Viewport = CaptureViewportClient->Viewport;
		}
	}

	auto IsViewportReady = [&Viewport]() -> bool
	{
		return Viewport && Viewport->GetSizeXY().X > 0 && Viewport->GetSizeXY().Y > 0;
	};

	for (int32 i = 0; i < 20 && !IsViewportReady(); ++i)
	{
		if (TSharedPtr<FTabManager> TabManager = LevelEditor.GetLevelEditorTabManager())
		{
			TabManager->TryInvokeTab(FTabId(TEXT("LevelEditorViewport")));
		}
		FSlateApplication::Get().Tick();
		CaptureViewportClient->Tick(0.033f);
		GEditor->RedrawAllViewports(true);
		FlushRenderingCommands();
		if (CaptureViewportClient->Viewport)
		{
			Viewport = CaptureViewportClient->Viewport;
		}
	}

	if (!IsViewportReady())
	{
		return FMonolithActionResult::Error(TEXT("视口尺寸为零"));
	}

	// 多帧 Tick 确保资源加载完成
	for (int32 i = 0; i < 8; ++i)
	{
		FSlateApplication::Get().Tick();
		CaptureViewportClient->Tick(0.033f);
		GEditor->RedrawAllViewports(true);
		FlushRenderingCommands();
	}

	TArray<FColor> Bitmap;
	const int32 Width = Viewport->GetSizeXY().X;
	const int32 Height = Viewport->GetSizeXY().Y;

	if (!Viewport->ReadPixels(Bitmap) || Bitmap.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("视口像素读取失败"));
	}

	for (FColor& Pixel : Bitmap)
	{
		Pixel.A = 255;
	}

	const FString ArtifactRoot = ResolveArtifactRoot(Params);
	const FString ScreenshotDir = FPaths::Combine(ArtifactRoot, TEXT("screenshots"));
	IFileManager::Get().MakeDirectory(*ScreenshotDir, true);

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString Filename = FString::Printf(TEXT("viewport_%s.png"), *Timestamp);
	const FString FullPath = FPaths::Combine(ScreenshotDir, Filename);

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!ImageWrapper.IsValid() || !ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
	{
		return FMonolithActionResult::Error(TEXT("截图编码失败"));
	}

	const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed();
	if (!FFileHelper::SaveArrayToFile(CompressedData, *FullPath))
	{
		return FMonolithActionResult::Error(TEXT("截图保存失败"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("path"), FullPath);
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (World)
	{
		Result->SetStringField(TEXT("map"), World->GetMapName());
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleSetViewportCamera(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	}

	const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
	if (Clients.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("没有可用的编辑器视口"));
	}

	FLevelEditorViewportClient* ViewportClient = Clients[0];
	if (Params.IsValid())
	{
		if (const TSharedPtr<FJsonObject>* LocationObject = nullptr;
			Params->TryGetObjectField(TEXT("location"), LocationObject))
		{
			FVector Location;
			Location.X = (*LocationObject)->GetNumberField(TEXT("x"));
			Location.Y = (*LocationObject)->GetNumberField(TEXT("y"));
			Location.Z = (*LocationObject)->GetNumberField(TEXT("z"));
			ViewportClient->SetViewLocation(Location);
		}

		if (const TSharedPtr<FJsonObject>* RotationObject = nullptr;
			Params->TryGetObjectField(TEXT("rotation"), RotationObject))
		{
			FRotator Rotation;
			Rotation.Pitch = (*RotationObject)->GetNumberField(TEXT("pitch"));
			Rotation.Yaw = (*RotationObject)->GetNumberField(TEXT("yaw"));
			Rotation.Roll = (*RotationObject)->GetNumberField(TEXT("roll"));
			ViewportClient->SetViewRotation(Rotation);
		}
	}

	ViewportClient->Tick(0.033f);
	if (ViewportClient->Viewport)
	{
		ViewportClient->Viewport->Invalidate();
		ViewportClient->Viewport->InvalidateDisplay();
		ViewportClient->Viewport->Draw();
		FlushRenderingCommands();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleListRunArtifacts(const TSharedPtr<FJsonObject>& Params)
{
	const FString RunId = Params.IsValid() ? Params->GetStringField(TEXT("run_id")) : FString();
	if (RunId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("缺少 run_id"));
	}

	const FString RunDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AI"), TEXT("runs"), RunId);
	TArray<FString> FoundFiles;
	IFileManager::Get().FindFilesRecursive(FoundFiles, *RunDir, TEXT("*.*"), true, false);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> FileArray;
	for (const FString& FilePath : FoundFiles)
	{
		TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
		FString RelPath = FilePath;
		FPaths::MakePathRelativeTo(RelPath, *RunDir);
		FileObj->SetStringField(TEXT("path"), RelPath);
		FileObj->SetStringField(TEXT("full_path"), FilePath);
		FileArray.Add(MakeShared<FJsonValueObject>(FileObj));
	}

	Result->SetArrayField(TEXT("files"), FileArray);
	Result->SetStringField(TEXT("run_id"), RunId);
	Result->SetNumberField(TEXT("count"), FoundFiles.Num());
	return FMonolithActionResult::Success(Result);
}

// ===== 资产预览截图 =====

FMonolithActionResult FMonolithCaptureActions::HandleCaptureStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load StaticMesh: %s"), *AssetPath));
	}

	FVector CameraLocation; FRotator CameraRotation; float FOV;
	ParseCameraParams(Params, CameraLocation, CameraRotation, FOV);

	// 根据 mesh bounds 自动计算默认相机距离
	if (!Params->HasField(TEXT("camera")))
	{
		FBoxSphereBounds Bounds = Mesh->GetBounds();
		float Radius = Bounds.SphereRadius;
		if (Radius < 1.0f) Radius = 100.0f;
		CameraLocation = Bounds.Origin + FVector(Radius * 2.0f, Radius * 0.5f, Radius * 0.8f);
		CameraRotation = (Bounds.Origin - CameraLocation).Rotation();
	}

	int32 ResX, ResY;
	ParseResolutionParams(Params, ResX, ResY);
	FString OutputPath = ResolveOutputPath(Params, AssetPath);

	check(IsInGameThread());
	double StartTime = FPlatformTime::Seconds();

	bool bSuccess = CaptureStaticMeshFrame(Mesh, CameraLocation, CameraRotation, FOV, ResX, ResY, OutputPath);

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("StaticMesh capture failed"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleCaptureSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
	if (!Mesh)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load SkeletalMesh: %s"), *AssetPath));
	}

	bool bShowBones = false;
	if (Params->HasField(TEXT("show_bones")))
	{
		bShowBones = Params->GetBoolField(TEXT("show_bones"));
	}

	FVector CameraLocation; FRotator CameraRotation; float FOV;
	ParseCameraParams(Params, CameraLocation, CameraRotation, FOV);

	if (!Params->HasField(TEXT("camera")))
	{
		FBoxSphereBounds Bounds = Mesh->GetBounds();
		float Radius = Bounds.SphereRadius;
		if (Radius < 1.0f) Radius = 100.0f;
		CameraLocation = Bounds.Origin + FVector(Radius * 2.0f, Radius * 0.5f, Radius * 0.8f);
		CameraRotation = (Bounds.Origin - CameraLocation).Rotation();
	}

	int32 ResX, ResY;
	ParseResolutionParams(Params, ResX, ResY);
	FString OutputPath = ResolveOutputPath(Params, AssetPath);

	check(IsInGameThread());
	double StartTime = FPlatformTime::Seconds();

	bool bSuccess = CaptureSkeletalMeshFrame(Mesh, nullptr, 0.0f, bShowBones,
		CameraLocation, CameraRotation, FOV, ResX, ResY, OutputPath);

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("SkeletalMesh capture failed"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetBoolField(TEXT("show_bones"), bShowBones);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleCaptureAnimation(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	UAnimSequence* Anim = LoadObject<UAnimSequence>(nullptr, *AssetPath);
	if (!Anim)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load AnimSequence: %s"), *AssetPath));
	}

	// 获取关联的 SkeletalMesh：优先使用用户指定的 skeletal_mesh_path
	USkeletalMesh* Mesh = nullptr;
	FString SkMeshPath = Params->GetStringField(TEXT("skeletal_mesh_path"));
	if (!SkMeshPath.IsEmpty())
	{
		Mesh = LoadObject<USkeletalMesh>(nullptr, *SkMeshPath);
		if (!Mesh)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load SkeletalMesh: %s"), *SkMeshPath));
		}
	}
	else
	{
		// 从动画的 Skeleton 自动查找
		USkeleton* Skeleton = Anim->GetSkeleton();
		if (Skeleton)
		{
			Mesh = Skeleton->GetAssetPreviewMesh(Anim);
			if (!Mesh)
			{
				Mesh = Skeleton->FindCompatibleMesh();
			}
		}
	}
	if (!Mesh)
	{
		return FMonolithActionResult::Error(
			TEXT("No SkeletalMesh found. Provide 'skeletal_mesh_path' parameter or use an AnimSequence with a skeleton reference."));
	}

	// 时间戳
	TArray<float> Timestamps;
	if (Params->HasField(TEXT("timestamps")))
	{
		const TArray<TSharedPtr<FJsonValue>>& TimestampArray = Params->GetArrayField(TEXT("timestamps"));
		for (const auto& Val : TimestampArray)
		{
			Timestamps.Add((float)Val->AsNumber());
		}
	}
	if (Timestamps.Num() == 0)
	{
		Timestamps.Add(0.0f);
	}
	Timestamps.Sort();

	bool bShowBones = false;
	if (Params->HasField(TEXT("show_bones")))
	{
		bShowBones = Params->GetBoolField(TEXT("show_bones"));
	}

	FVector CameraLocation; FRotator CameraRotation; float FOV;
	ParseCameraParams(Params, CameraLocation, CameraRotation, FOV);

	if (!Params->HasField(TEXT("camera")))
	{
		FBoxSphereBounds Bounds = Mesh->GetBounds();
		float Radius = Bounds.SphereRadius;
		if (Radius < 1.0f) Radius = 100.0f;
		CameraLocation = Bounds.Origin + FVector(Radius * 2.0f, Radius * 0.5f, Radius * 0.8f);
		CameraRotation = (Bounds.Origin - CameraLocation).Rotation();
	}

	int32 ResX, ResY;
	ParseResolutionParams(Params, ResX, ResY);

	FString FilenamePrefix = TEXT("frame");
	if (Params->HasField(TEXT("filename_prefix")))
	{
		FilenamePrefix = Params->GetStringField(TEXT("filename_prefix"));
	}

	FString OutputDir;
	if (Params->HasField(TEXT("output_dir")))
	{
		OutputDir = Params->GetStringField(TEXT("output_dir"));
		if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s"), *Timestamp, *SafeName);
	}

	check(IsInGameThread());
	double StartTime = FPlatformTime::Seconds();
	TArray<TSharedPtr<FJsonValue>> FrameResults;

	for (int32 i = 0; i < Timestamps.Num(); ++i)
	{
		float T = Timestamps[i];
		FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
			*FilenamePrefix, i, T);

		bool bOk = CaptureSkeletalMeshFrame(Mesh, Anim, T, bShowBones,
			CameraLocation, CameraRotation, FOV, ResX, ResY, FramePath);

		TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
		FrameObj->SetNumberField(TEXT("timestamp"), T);
		FrameObj->SetStringField(TEXT("file"), FramePath);
		FrameObj->SetBoolField(TEXT("success"), bOk);
		FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("frames"), FrameResults);
	Result->SetNumberField(TEXT("total_capture_time_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleCaptureNiagara(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
	if (!System)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath));
	}

	float SeekTime = 1.0f;
	if (Params->HasField(TEXT("seek_time")))
	{
		SeekTime = (float)Params->GetNumberField(TEXT("seek_time"));
	}

	FVector CameraLocation; FRotator CameraRotation; float FOV;
	ParseCameraParams(Params, CameraLocation, CameraRotation, FOV);

	int32 ResX, ResY;
	ParseResolutionParams(Params, ResX, ResY);
	FString OutputPath = ResolveOutputPath(Params, AssetPath);

	check(IsInGameThread());
	double StartTime = FPlatformTime::Seconds();

	bool bSuccess = CaptureNiagaraFrame(System, SeekTime, CameraLocation, CameraRotation,
		FOV, ResX, ResY, OutputPath);

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("Niagara capture failed"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetNumberField(TEXT("seek_time"), SeekTime);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleCaptureMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *AssetPath);
	if (!Material)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load material: %s"), *AssetPath));
	}

	FString PreviewMesh = TEXT("sphere");
	if (Params->HasField(TEXT("preview_mesh")))
	{
		PreviewMesh = Params->GetStringField(TEXT("preview_mesh"));
	}

	FVector CameraLocation; FRotator CameraRotation; float FOV;
	ParseCameraParams(Params, CameraLocation, CameraRotation, FOV);

	// 自动计算预览形状的相机位置（Engine BasicShapes 默认半径约 50）
	if (!Params->HasField(TEXT("camera")))
	{
		const float Radius = 50.0f;
		CameraLocation = FVector(Radius * 2.5f, Radius * 0.5f, Radius * 0.8f);
		CameraRotation = (-CameraLocation).Rotation();
	}

	int32 ResX, ResY;
	ParseResolutionParams(Params, ResX, ResY);
	FString OutputPath = ResolveOutputPath(Params, AssetPath);

	check(IsInGameThread());
	double StartTime = FPlatformTime::Seconds();

	bool bSuccess = CaptureMaterialFrame(Material, PreviewMesh, CameraLocation, CameraRotation,
		FOV, ResX, ResY, OutputPath);

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("Material capture failed"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCaptureActions::HandleCaptureWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	UWidgetBlueprint* WidgetBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
	if (!WidgetBP)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load WidgetBlueprint: %s"), *AssetPath));
	}

	int32 ResX = 1920, ResY = 1080;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ResArray = Params->GetArrayField(TEXT("resolution"));
		if (ResArray.Num() >= 2)
		{
			ResX = (int32)ResArray[0]->AsNumber();
			ResY = (int32)ResArray[1]->AsNumber();
		}
	}

	FString OutputPath = ResolveOutputPath(Params, AssetPath, TEXT("_widget"));

	check(IsInGameThread());
	double StartTime = FPlatformTime::Seconds();

	// 使用 WidgetBlueprint 的 GeneratedClass 创建 Widget 实例
	UClass* WidgetClass = WidgetBP->GeneratedClass;
	if (!WidgetClass)
	{
		return FMonolithActionResult::Error(TEXT("WidgetBlueprint has no GeneratedClass"));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	UUserWidget* Widget = CreateWidget<UUserWidget>(World, WidgetClass);
	if (!Widget)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create widget instance"));
	}

	// 获取 Slate widget
	TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
	if (!SlateWidget.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Failed to get Slate widget"));
	}

	// 创建 RenderTarget
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitCustomFormat(ResX, ResY, PF_B8G8R8A8, false);
	RT->ClearColor = FLinearColor(0.1f, 0.1f, 0.1f, 1.0f);
	RT->UpdateResourceImmediate(true);

	// 使用 FWidgetRenderer 渲染
	FWidgetRenderer WidgetRenderer(true);
	WidgetRenderer.DrawWidget(RT, SlateWidget.ToSharedRef(),
		FVector2D(ResX, ResY), 0.016f);

	FlushRenderingCommands();

	// 读取像素
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		return FMonolithActionResult::Error(TEXT("Failed to get RT resource"));
	}

	TArray<FColor> Pixels;
	bool bReadOk = RTResource->ReadPixels(Pixels);
	if (!bReadOk || Pixels.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("ReadPixels failed for widget capture"));
	}

	// 保存 PNG
	FString Dir = FPaths::GetPath(OutputPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	FImage Image;
	Image.Init(ResX, ResY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

	bool bSaved = FImageUtils::SaveImageAutoFormat(*OutputPath, Image);

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSaved)
	{
		return FMonolithActionResult::Error(TEXT("Widget capture save failed"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	Result->SetNumberField(TEXT("width"), ResX);
	Result->SetNumberField(TEXT("height"), ResY);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}

// ===== 通用多帧序列截图 =====

FMonolithActionResult FMonolithCaptureActions::HandleCaptureSequenceFrames(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AssetType = Params->GetStringField(TEXT("asset_type"));

	if (AssetPath.IsEmpty() || AssetType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and asset_type are required"));
	}

	if (!Params->HasField(TEXT("timestamps")))
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is required"));
	}

	TArray<float> Timestamps;
	const TArray<TSharedPtr<FJsonValue>>& TimestampArray = Params->GetArrayField(TEXT("timestamps"));
	for (const auto& Val : TimestampArray)
	{
		Timestamps.Add((float)Val->AsNumber());
	}
	Timestamps.Sort();

	if (Timestamps.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is empty"));
	}

	int32 ResX, ResY;
	ParseResolutionParams(Params, ResX, ResY);

	FVector CameraLocation; FRotator CameraRotation; float FOV;
	ParseCameraParams(Params, CameraLocation, CameraRotation, FOV);

	FString OutputDir;
	if (Params->HasField(TEXT("output_dir")))
	{
		OutputDir = Params->GetStringField(TEXT("output_dir"));
		if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
	}
	else
	{
		FString Ts = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s"), *Ts, *SafeName);
	}

	FString FilenamePrefix = TEXT("frame");
	if (Params->HasField(TEXT("filename_prefix")))
	{
		FilenamePrefix = Params->GetStringField(TEXT("filename_prefix"));
	}

	check(IsInGameThread());
	double StartTime = FPlatformTime::Seconds();
	TArray<TSharedPtr<FJsonValue>> FrameResults;

	if (AssetType.Equals(TEXT("niagara"), ESearchCase::IgnoreCase))
	{
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
		if (!System)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load: %s"), *AssetPath));
		}

		for (int32 i = 0; i < Timestamps.Num(); ++i)
		{
			float T = Timestamps[i];
			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, T);

			bool bOk = CaptureNiagaraFrame(System, T, CameraLocation, CameraRotation,
				FOV, ResX, ResY, FramePath);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), T);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
		}
	}
	else if (AssetType.Equals(TEXT("animation"), ESearchCase::IgnoreCase))
	{
		UAnimSequence* Anim = LoadObject<UAnimSequence>(nullptr, *AssetPath);
		if (!Anim)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load AnimSequence: %s"), *AssetPath));
		}

		USkeleton* Skeleton = Anim->GetSkeleton();
		if (!Skeleton)
		{
			return FMonolithActionResult::Error(TEXT("AnimSequence has no skeleton"));
		}

		USkeletalMesh* Mesh = Skeleton->GetAssetPreviewMesh(Anim);
		if (!Mesh) Mesh = Skeleton->FindCompatibleMesh();
		if (!Mesh)
		{
			return FMonolithActionResult::Error(TEXT("No compatible SkeletalMesh found"));
		}

		bool bShowBones = false;
		if (Params->HasField(TEXT("show_bones")))
		{
			bShowBones = Params->GetBoolField(TEXT("show_bones"));
		}

		// 根据 mesh bounds 自动计算默认相机
		if (!Params->HasField(TEXT("camera")))
		{
			FBoxSphereBounds Bounds = Mesh->GetBounds();
			float Radius = Bounds.SphereRadius;
			if (Radius < 1.0f) Radius = 100.0f;
			CameraLocation = Bounds.Origin + FVector(Radius * 2.0f, Radius * 0.5f, Radius * 0.8f);
			CameraRotation = (Bounds.Origin - CameraLocation).Rotation();
		}

		for (int32 i = 0; i < Timestamps.Num(); ++i)
		{
			float T = Timestamps[i];
			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, T);

			bool bOk = CaptureSkeletalMeshFrame(Mesh, Anim, T, bShowBones,
				CameraLocation, CameraRotation, FOV, ResX, ResY, FramePath);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), T);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
		}
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unsupported asset_type: %s (supported: niagara, animation)"), *AssetType));
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("frames"), FrameResults);
	Result->SetNumberField(TEXT("total_capture_time_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}
