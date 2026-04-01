#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "Components/SceneCaptureComponent2D.h"

class FMonolithCaptureActions
{
public:
	static void RegisterActions();

	// 地图管理
	static FMonolithActionResult HandleGetCurrentMap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleOpenMap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSaveMap(const TSharedPtr<FJsonObject>& Params);

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

	// Actor 查找与聚焦
	static FMonolithActionResult HandleFindActorsByClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSelectAndFocus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFocusActor(const TSharedPtr<FJsonObject>& Params);

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

	static bool CaptureStaticMeshFrame(
		class UStaticMesh* Mesh,
		const FVector& CameraLocation, const FRotator& CameraRotation, float FOV,
		int32 ResX, int32 ResY, const FString& OutputPath,
		ESceneCaptureSource CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR);

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
