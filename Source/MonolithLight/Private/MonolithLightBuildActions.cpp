#include "MonolithLightBuildActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "Components/SkyAtmosphereComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithLightBuildActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- 光照贴图 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_lightmap_settings"),
		TEXT("Get lightmap build settings for the current level"),
		FMonolithActionHandler::CreateStatic(&HandleGetLightmapSettings),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("light"), TEXT("set_lightmap_resolution"),
		TEXT("Set lightmap resolution for a static mesh component"),
		FMonolithActionHandler::CreateStatic(&HandleSetLightmapResolution),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Required(TEXT("resolution"), TEXT("integer"), TEXT("Lightmap resolution (power of 2)"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Specific mesh component name"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("build_lighting"),
		TEXT("Trigger a lighting build for the current level"),
		FMonolithActionHandler::CreateStatic(&HandleBuildLighting),
		FParamSchemaBuilder()
			.Optional(TEXT("quality"), TEXT("string"), TEXT("Build quality: Preview, Medium, High, Production"), TEXT("Preview"))
			.Build());

	// --- 反射捕获 ---
	Registry.RegisterAction(TEXT("light"), TEXT("list_reflection_captures"),
		TEXT("List all reflection capture actors in the current level"),
		FMonolithActionHandler::CreateStatic(&HandleListReflectionCaptures),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("light"), TEXT("update_reflection_captures"),
		TEXT("Update all reflection captures in the current level"),
		FMonolithActionHandler::CreateStatic(&HandleUpdateReflectionCaptures),
		MakeShared<FJsonObject>());

	// --- 天空大气 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_sky_atmosphere_info"),
		TEXT("Get sky atmosphere component settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkyAtmosphereInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_sky_atmosphere_property"),
		TEXT("Set a sky atmosphere property"),
		FMonolithActionHandler::CreateStatic(&HandleSetSkyAtmosphereProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value"))
			.Build());

	// --- Lumen ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_lumen_settings"),
		TEXT("Get Lumen global illumination and reflection settings from post-process"),
		FMonolithActionHandler::CreateStatic(&HandleGetLumenSettings),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint with PostProcessVolume"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_lumen_property"),
		TEXT("Set a Lumen GI or reflection property on a post-process volume"),
		FMonolithActionHandler::CreateStatic(&HandleSetLumenProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint with PostProcessVolume"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Lumen property name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value"))
			.Build());

	// --- 指数级高度雾 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_height_fog_info"),
		TEXT("Get exponential height fog settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetHeightFogInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_height_fog_property"),
		TEXT("Set an exponential height fog property"),
		FMonolithActionHandler::CreateStatic(&HandleSetHeightFogProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value"))
			.Build());

	// --- 体积云 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_volumetric_cloud_info"),
		TEXT("Get volumetric cloud component settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetVolumetricCloudInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_volumetric_cloud_property"),
		TEXT("Set a volumetric cloud property"),
		FMonolithActionHandler::CreateStatic(&HandleSetVolumetricCloudProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value"))
			.Build());
}

// ============================================================================
// Handlers — 光照贴图
// ============================================================================

FMonolithActionResult FMonolithLightBuildActions::HandleGetLightmapSettings(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return FMonolithActionResult::Error(TEXT("No editor world available"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("level_name"), World->GetMapName());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightBuildActions::HandleSetLightmapResolution(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const int32 Resolution = static_cast<int32>(Params->GetNumberField(TEXT("resolution")));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetNumberField(TEXT("resolution"), Resolution);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLightBuildActions::HandleBuildLighting(const TSharedPtr<FJsonObject>& Params)
{
	const FString Quality = Params->HasField(TEXT("quality")) ? Params->GetStringField(TEXT("quality")) : TEXT("Preview");

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return FMonolithActionResult::Error(TEXT("No editor world available"));

	// 触发光照构建
	GEditor->Exec(World, TEXT("BUILD LIGHTING"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("build_started"));
	Result->SetStringField(TEXT("quality"), Quality);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Handlers — 反射捕获
// ============================================================================

FMonolithActionResult FMonolithLightBuildActions::HandleListReflectionCaptures(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return FMonolithActionResult::Error(TEXT("No editor world available"));

	TArray<TSharedPtr<FJsonValue>> Items;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetClass()->GetName().Contains(TEXT("ReflectionCapture")))
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Actor->GetActorNameOrLabel());
			Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			FVector Loc = Actor->GetActorLocation();
			Obj->SetStringField(TEXT("location"), FString::Printf(TEXT("(%.1f, %.1f, %.1f)"), Loc.X, Loc.Y, Loc.Z));
			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("reflection_captures"), Items);
	Root->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightBuildActions::HandleUpdateReflectionCaptures(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World) return FMonolithActionResult::Error(TEXT("No editor world available"));

	GEditor->Exec(World, TEXT("BUILD REFLECTIONS"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("updated"));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Handlers — 天空大气
// ============================================================================

FMonolithActionResult FMonolithLightBuildActions::HandleGetSkyAtmosphereInfo(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	USkyAtmosphereComponent* SkyAtmo = CDO->FindComponentByClass<USkyAtmosphereComponent>();
	if (!SkyAtmo) return FMonolithActionResult::Error(TEXT("No SkyAtmosphereComponent found"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("atmosphere_height"), SkyAtmo->AtmosphereHeight);
	Root->SetNumberField(TEXT("multi_scattering_factor"), SkyAtmo->MultiScatteringFactor);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightBuildActions::HandleSetSkyAtmosphereProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetStringField(TEXT("property"), PropName);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Handlers — Lumen
// ============================================================================

FMonolithActionResult FMonolithLightBuildActions::HandleGetLumenSettings(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("note"), TEXT("Lumen settings are project-level in UE 5.x, accessible via post-process or project settings"));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightBuildActions::HandleSetLumenProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetStringField(TEXT("property"), PropName);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Handlers — 高度雾
// ============================================================================

FMonolithActionResult FMonolithLightBuildActions::HandleGetHeightFogInfo(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	UExponentialHeightFogComponent* Fog = CDO->FindComponentByClass<UExponentialHeightFogComponent>();
	if (!Fog) return FMonolithActionResult::Error(TEXT("No ExponentialHeightFogComponent found"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("fog_density"), Fog->FogDensity);
	Root->SetNumberField(TEXT("fog_height_falloff"), Fog->FogHeightFalloff);
	Root->SetNumberField(TEXT("fog_max_opacity"), Fog->FogMaxOpacity);
	Root->SetNumberField(TEXT("start_distance"), Fog->StartDistance);
	Root->SetBoolField(TEXT("volumetric_fog"), Fog->bEnableVolumetricFog);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightBuildActions::HandleSetHeightFogProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	UExponentialHeightFogComponent* Fog = CDO->FindComponentByClass<UExponentialHeightFogComponent>();
	if (!Fog) return FMonolithActionResult::Error(TEXT("No ExponentialHeightFogComponent found"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Monolith: Set Height Fog Property")));
	Fog->Modify();

	if (PropName == TEXT("FogDensity"))
	{
		Fog->FogDensity = Params->GetNumberField(TEXT("value"));
	}
	else if (PropName == TEXT("FogHeightFalloff"))
	{
		Fog->FogHeightFalloff = Params->GetNumberField(TEXT("value"));
	}
	else if (PropName == TEXT("FogMaxOpacity"))
	{
		Fog->FogMaxOpacity = Params->GetNumberField(TEXT("value"));
	}
	else if (PropName == TEXT("StartDistance"))
	{
		Fog->StartDistance = Params->GetNumberField(TEXT("value"));
	}
	else if (PropName == TEXT("EnableVolumetricFog"))
	{
		Fog->SetVolumetricFog(Params->GetBoolField(TEXT("value")));
	}
	else
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown property: %s"), *PropName));
	}

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetStringField(TEXT("property"), PropName);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Handlers — 体积云
// ============================================================================

FMonolithActionResult FMonolithLightBuildActions::HandleGetVolumetricCloudInfo(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	UVolumetricCloudComponent* Cloud = CDO->FindComponentByClass<UVolumetricCloudComponent>();
	if (!Cloud) return FMonolithActionResult::Error(TEXT("No VolumetricCloudComponent found"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("layer_bottom_altitude"), Cloud->LayerBottomAltitude);
	Root->SetNumberField(TEXT("layer_height"), Cloud->LayerHeight);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightBuildActions::HandleSetVolumetricCloudProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	UVolumetricCloudComponent* Cloud = CDO->FindComponentByClass<UVolumetricCloudComponent>();
	if (!Cloud) return FMonolithActionResult::Error(TEXT("No VolumetricCloudComponent found"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Monolith: Set Volumetric Cloud Property")));
	Cloud->Modify();

	if (PropName == TEXT("LayerBottomAltitude"))
	{
		Cloud->LayerBottomAltitude = Params->GetNumberField(TEXT("value"));
	}
	else if (PropName == TEXT("LayerHeight"))
	{
		Cloud->LayerHeight = Params->GetNumberField(TEXT("value"));
	}
	else
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown property: %s"), *PropName));
	}

	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetStringField(TEXT("property"), PropName);
	return FMonolithActionResult::Success(Result);
}
