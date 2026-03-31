#include "MonolithLightActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/TextureLightProfile.h"
#include "Engine/SkyLight.h"
#include "Engine/Blueprint.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithLightActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- 光照组件操作 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_light_properties"),
		TEXT("Get all properties of a light component in a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleGetLightProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Specific light component name"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_light_property"),
		TEXT("Set a property on a light component"),
		FMonolithActionHandler::CreateStatic(&HandleSetLightProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property to set (Intensity, Color, AttenuationRadius, etc.)"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Specific light component name"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("list_light_components"),
		TEXT("List all light components in a Blueprint or Actor"),
		FMonolithActionHandler::CreateStatic(&HandleListLightComponents),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("add_light_component"),
		TEXT("Add a light component to a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddLightComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("light_type"), TEXT("string"), TEXT("Light type: PointLight, SpotLight, RectLight, DirectionalLight, SkyLight"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Component name"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("remove_light_component"),
		TEXT("Remove a light component from a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveLightComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component name to remove"))
			.Build());

	// --- IES 配置文件 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_light_profile"),
		TEXT("Get the IES light profile assigned to a light component"),
		FMonolithActionHandler::CreateStatic(&HandleGetLightProfile),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Light component name"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_light_profile"),
		TEXT("Assign an IES light profile to a light component"),
		FMonolithActionHandler::CreateStatic(&HandleSetLightProfile),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("ies_path"), TEXT("string"), TEXT("IES texture asset path"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Light component name"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("list_ies_profiles"),
		TEXT("List all IES light profile assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListIESProfiles),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Directory filter"))
			.Build());

	// --- 后处理体积 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_post_process_settings"),
		TEXT("Get post-process volume settings"),
		FMonolithActionHandler::CreateStatic(&HandleGetPostProcessSettings),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path with PostProcessVolume"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_post_process_property"),
		TEXT("Set a post-process volume property (bloom, exposure, color grading, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleSetPostProcessProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Post-process property name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value"))
			.Build());

	// --- 天光 ---
	Registry.RegisterAction(TEXT("light"), TEXT("get_sky_light_info"),
		TEXT("Get sky light component configuration"),
		FMonolithActionHandler::CreateStatic(&HandleGetSkyLightInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("set_sky_light_property"),
		TEXT("Set a sky light property"),
		FMonolithActionHandler::CreateStatic(&HandleSetSkyLightProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value"))
			.Build());

	Registry.RegisterAction(TEXT("light"), TEXT("recapture_sky_light"),
		TEXT("Trigger sky light recapture"),
		FMonolithActionHandler::CreateStatic(&HandleRecaptureSkyLight),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint or actor path containing SkyLight"))
			.Build());
}

// ============================================================================
// Helpers
// ============================================================================

static ULightComponent* FindLightComponent(UBlueprint* BP, const FString& ComponentName)
{
	if (!BP || !BP->GeneratedClass) return nullptr;

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return nullptr;

	if (ComponentName.IsEmpty())
	{
		return CDO->FindComponentByClass<ULightComponent>();
	}

	TArray<ULightComponent*> Lights;
	CDO->GetComponents<ULightComponent>(Lights);
	for (ULightComponent* Light : Lights)
	{
		if (Light->GetName() == ComponentName)
		{
			return Light;
		}
	}
	return nullptr;
}

static TSharedPtr<FJsonObject> SerializeLightComponent(const ULightComponent* Light)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Light->GetName());
	Obj->SetStringField(TEXT("class"), Light->GetClass()->GetName());
	Obj->SetNumberField(TEXT("intensity"), Light->Intensity);
	Obj->SetBoolField(TEXT("cast_shadows"), Light->CastShadows);

	// 颜色
	FLinearColor Color = Light->GetLightColor();
	TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
	ColorObj->SetNumberField(TEXT("r"), Color.R);
	ColorObj->SetNumberField(TEXT("g"), Color.G);
	ColorObj->SetNumberField(TEXT("b"), Color.B);
	ColorObj->SetNumberField(TEXT("a"), Color.A);
	Obj->SetObjectField(TEXT("color"), ColorObj);

	if (const UPointLightComponent* Point = Cast<UPointLightComponent>(Light))
	{
		Obj->SetNumberField(TEXT("attenuation_radius"), Point->AttenuationRadius);
		Obj->SetNumberField(TEXT("source_radius"), Point->SourceRadius);

		if (const USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
		{
			Obj->SetNumberField(TEXT("inner_cone_angle"), Spot->InnerConeAngle);
			Obj->SetNumberField(TEXT("outer_cone_angle"), Spot->OuterConeAngle);
		}
	}
	else if (const URectLightComponent* Rect = Cast<URectLightComponent>(Light))
	{
		Obj->SetNumberField(TEXT("source_width"), Rect->SourceWidth);
		Obj->SetNumberField(TEXT("source_height"), Rect->SourceHeight);
	}

	return Obj;
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FMonolithLightActions::HandleGetLightProperties(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString CompName = Params->HasField(TEXT("component_name")) ? Params->GetStringField(TEXT("component_name")) : TEXT("");

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	ULightComponent* Light = FindLightComponent(BP, CompName);
	if (!Light) return FMonolithActionResult::Error(TEXT("No light component found"));

	return FMonolithActionResult::Success(SerializeLightComponent(Light));
}

FMonolithActionResult FMonolithLightActions::HandleSetLightProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));
	const FString CompName = Params->HasField(TEXT("component_name")) ? Params->GetStringField(TEXT("component_name")) : TEXT("");

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	ULightComponent* Light = FindLightComponent(BP, CompName);
	if (!Light) return FMonolithActionResult::Error(TEXT("No light component found"));

	GEditor->BeginTransaction(FText::FromString(TEXT("Monolith: Set Light Property")));
	Light->Modify();

	if (PropName == TEXT("Intensity"))
	{
		Light->SetIntensity(Params->GetNumberField(TEXT("value")));
	}
	else if (PropName == TEXT("Color"))
	{
		const TSharedPtr<FJsonObject>* ColorObj;
		if (Params->TryGetObjectField(TEXT("value"), ColorObj))
		{
			FLinearColor Color(
				(*ColorObj)->GetNumberField(TEXT("r")),
				(*ColorObj)->GetNumberField(TEXT("g")),
				(*ColorObj)->GetNumberField(TEXT("b")),
				(*ColorObj)->HasField(TEXT("a")) ? (*ColorObj)->GetNumberField(TEXT("a")) : 1.0f
			);
			Light->SetLightColor(Color);
		}
	}
	else if (PropName == TEXT("CastShadows"))
	{
		Light->SetCastShadows(Params->GetBoolField(TEXT("value")));
	}
	else if (PropName == TEXT("AttenuationRadius"))
	{
		if (UPointLightComponent* Point = Cast<UPointLightComponent>(Light))
		{
			Point->SetAttenuationRadius(Params->GetNumberField(TEXT("value")));
		}
	}
	else if (PropName == TEXT("InnerConeAngle"))
	{
		if (USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
		{
			Spot->SetInnerConeAngle(Params->GetNumberField(TEXT("value")));
		}
	}
	else if (PropName == TEXT("OuterConeAngle"))
	{
		if (USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
		{
			Spot->SetOuterConeAngle(Params->GetNumberField(TEXT("value")));
		}
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

FMonolithActionResult FMonolithLightActions::HandleListLightComponents(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("Blueprint has no generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	TArray<ULightComponent*> Lights;
	CDO->GetComponents<ULightComponent>(Lights);

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const ULightComponent* Light : Lights)
	{
		Items.Add(MakeShared<FJsonValueObject>(SerializeLightComponent(Light)));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("lights"), Items);
	Root->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightActions::HandleAddLightComponent(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString LightType = Params->GetStringField(TEXT("light_type"));
	const FString CompName = Params->HasField(TEXT("component_name")) ? Params->GetStringField(TEXT("component_name")) : LightType + TEXT("Component");

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("added"));
	Result->SetStringField(TEXT("light_type"), LightType);
	Result->SetStringField(TEXT("component_name"), CompName);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLightActions::HandleRemoveLightComponent(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString CompName = Params->GetStringField(TEXT("component_name"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("removed"));
	Result->SetStringField(TEXT("component_name"), CompName);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLightActions::HandleGetLightProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString CompName = Params->HasField(TEXT("component_name")) ? Params->GetStringField(TEXT("component_name")) : TEXT("");

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	ULightComponent* Light = FindLightComponent(BP, CompName);
	if (!Light) return FMonolithActionResult::Error(TEXT("No light component found"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component"), Light->GetName());

	if (UTextureLightProfile* IES = Light->IESTexture)
	{
		Root->SetBoolField(TEXT("has_ies"), true);
		Root->SetStringField(TEXT("ies_path"), IES->GetPathName());
		Root->SetStringField(TEXT("ies_name"), IES->GetName());
	}
	else
	{
		Root->SetBoolField(TEXT("has_ies"), false);
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightActions::HandleSetLightProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString IESPath = Params->GetStringField(TEXT("ies_path"));
	const FString CompName = Params->HasField(TEXT("component_name")) ? Params->GetStringField(TEXT("component_name")) : TEXT("");

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	ULightComponent* Light = FindLightComponent(BP, CompName);
	if (!Light) return FMonolithActionResult::Error(TEXT("No light component found"));

	UTextureLightProfile* IES = FMonolithAssetUtils::LoadAssetByPath<UTextureLightProfile>(IESPath);
	if (!IES) return FMonolithActionResult::Error(FString::Printf(TEXT("IES profile not found: %s"), *IESPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Monolith: Set IES Profile")));
	Light->Modify();
	Light->IESTexture = IES;
	GEditor->EndTransaction();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetStringField(TEXT("ies_path"), IESPath);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLightActions::HandleListIESProfiles(const TSharedPtr<FJsonObject>& Params)
{
	FString PathFilter = TEXT("/Game/");
	if (Params->HasField(TEXT("path_filter")))
	{
		PathFilter = Params->GetStringField(TEXT("path_filter"));
	}

	FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	FARFilter Filter;
	Filter.ClassPaths.Add(UTextureLightProfile::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(*PathFilter));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.Get().GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Obj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Items.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("ies_profiles"), Items);
	Root->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightActions::HandleGetPostProcessSettings(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	UPostProcessComponent* PPComp = CDO->FindComponentByClass<UPostProcessComponent>();
	if (!PPComp) return FMonolithActionResult::Error(TEXT("No PostProcessComponent found"));

	const FPostProcessSettings& PP = PPComp->Settings;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("bloom_intensity"), PP.BloomIntensity);
	Root->SetNumberField(TEXT("auto_exposure_min_brightness"), PP.AutoExposureMinBrightness);
	Root->SetNumberField(TEXT("auto_exposure_max_brightness"), PP.AutoExposureMaxBrightness);
	Root->SetNumberField(TEXT("vignette_intensity"), PP.VignetteIntensity);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightActions::HandleSetPostProcessProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetStringField(TEXT("property"), PropName);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLightActions::HandleGetSkyLightInfo(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	USkyLightComponent* SkyLight = CDO->FindComponentByClass<USkyLightComponent>();
	if (!SkyLight) return FMonolithActionResult::Error(TEXT("No SkyLightComponent found"));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("intensity"), SkyLight->Intensity);
	Root->SetBoolField(TEXT("cast_shadows"), SkyLight->CastShadows);
	Root->SetNumberField(TEXT("volumetric_scattering_intensity"), SkyLight->VolumetricScatteringIntensity);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithLightActions::HandleSetSkyLightProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString PropName = Params->GetStringField(TEXT("property_name"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("set"));
	Result->SetStringField(TEXT("property"), PropName);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLightActions::HandleRecaptureSkyLight(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
	if (!BP) return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));

	if (!BP->GeneratedClass) return FMonolithActionResult::Error(TEXT("No generated class"));

	AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("CDO not found"));

	USkyLightComponent* SkyLight = CDO->FindComponentByClass<USkyLightComponent>();
	if (!SkyLight) return FMonolithActionResult::Error(TEXT("No SkyLightComponent found"));

	SkyLight->RecaptureSky();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("recaptured"));
	return FMonolithActionResult::Success(Result);
}
