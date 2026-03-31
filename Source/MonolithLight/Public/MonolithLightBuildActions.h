#pragma once

#include "MonolithToolRegistry.h"

class FMonolithToolRegistry;

/**
 * 光照构建操作 — 光照贴图、反射捕获、天空大气、Lumen、高度雾
 */
class FMonolithLightBuildActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// 光照贴图构建
	static FMonolithActionResult HandleGetLightmapSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetLightmapResolution(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBuildLighting(const TSharedPtr<FJsonObject>& Params);

	// 反射捕获
	static FMonolithActionResult HandleListReflectionCaptures(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleUpdateReflectionCaptures(const TSharedPtr<FJsonObject>& Params);

	// 天空大气
	static FMonolithActionResult HandleGetSkyAtmosphereInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSkyAtmosphereProperty(const TSharedPtr<FJsonObject>& Params);

	// Lumen 设置
	static FMonolithActionResult HandleGetLumenSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetLumenProperty(const TSharedPtr<FJsonObject>& Params);

	// 指数级高度雾
	static FMonolithActionResult HandleGetHeightFogInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetHeightFogProperty(const TSharedPtr<FJsonObject>& Params);

	// 体积云
	static FMonolithActionResult HandleGetVolumetricCloudInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetVolumetricCloudProperty(const TSharedPtr<FJsonObject>& Params);
};
