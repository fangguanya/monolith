#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "Components/SceneCaptureComponent2D.h"

class FMonolithCaptureActions
{
public:
	static void RegisterActions();

	// 视口截屏
	static FMonolithActionResult HandleCaptureViewport(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetViewportCamera(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListRunArtifacts(const TSharedPtr<FJsonObject>& Params);

	// 资产预览截图
	static FMonolithActionResult HandleCaptureStaticMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureSkeletalMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureAnimation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureNiagara(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureMaterial(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureWidget(const TSharedPtr<FJsonObject>& Params);

	// 通用多帧
	static FMonolithActionResult HandleCaptureSequenceFrames(const TSharedPtr<FJsonObject>& Params);

	// 离屏场景截图
	static FMonolithActionResult HandleCaptureScene(const TSharedPtr<FJsonObject>& Params);

private:
	// 截图辅助
	static bool RenderAndSaveCapture(
		class USceneCaptureComponent2D* CaptureComp,
		class UTextureRenderTarget2D* RT,
		int32 ResX, int32 ResY, const FString& OutputPath);

	static bool CaptureNiagaraFrame(
		class UNiagaraSystem* System, float SeekTime,
		const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
		int32 ResX, int32 ResY, const FString& OutputPath,
		ESceneCaptureSource CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR);

	static bool CaptureMaterialFrame(
		class UMaterialInterface* Material, const FString& MeshType,
		const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
		int32 ResX, int32 ResY, const FString& OutputPath,
		ESceneCaptureSource CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR);

	/** 渲染单 mesh 的 canonical iso 视图并保存为 PNG。
	 *  相机参数 / FOV / 光照 / 曝光 由 IAssetCanonicalRenderer 统一托管，外部不可覆盖；
	 *  这是为了让 capture action 与 AssetVisual cohort 共享完全一致的 render recipe。 */
	static bool CaptureStaticMeshFrame(
		class UStaticMesh* Mesh,
		int32 Resolution,
		const FString& OutputPath);

	static bool CaptureSkeletalMeshFrame(
		class USkeletalMesh* Mesh, class UAnimSequence* Anim, float Time, bool bShowBones,
		const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
		int32 ResX, int32 ResY, const FString& OutputPath,
		ESceneCaptureSource CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR);

	// 参数解析辅助
	static void ParseCameraParams(const TSharedPtr<FJsonObject>& Params,
		FVector& OutLocation, FRotator& OutRotation, float& OutFOV);
	static void ParseResolutionParams(const TSharedPtr<FJsonObject>& Params,
		int32& OutResX, int32& OutResY);
	static FString ResolveOutputPath(const TSharedPtr<FJsonObject>& Params,
		const FString& AssetPath, const FString& Suffix = TEXT(""));
	static FString ResolveArtifactRoot(const TSharedPtr<FJsonObject>& Params);
};
