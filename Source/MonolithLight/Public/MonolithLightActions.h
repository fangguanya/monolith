#pragma once

#include "MonolithToolRegistry.h"

class FMonolithToolRegistry;

/**
 * 光照组件操作 — 点光源、聚光灯、矩形光、天光、后处理体积
 */
class FMonolithLightActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// 光照组件操作
	static FMonolithActionResult HandleGetLightProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetLightProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListLightComponents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddLightComponent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveLightComponent(const TSharedPtr<FJsonObject>& Params);

	// 光照配置文件
	static FMonolithActionResult HandleGetLightProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetLightProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListIESProfiles(const TSharedPtr<FJsonObject>& Params);

	// 后处理体积
	static FMonolithActionResult HandleGetPostProcessSettings(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetPostProcessProperty(const TSharedPtr<FJsonObject>& Params);

	// 天光
	static FMonolithActionResult HandleGetSkyLightInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetSkyLightProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRecaptureSkyLight(const TSharedPtr<FJsonObject>& Params);
};
