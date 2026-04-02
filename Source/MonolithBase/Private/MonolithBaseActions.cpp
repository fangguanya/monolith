#include "MonolithBaseActions.h"
#include "MonolithBaseModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Animation/AnimInstance.h"
#include "ScopedTransaction.h"

// ============================================================================
// 辅助
// ============================================================================

AActor* FMonolithBaseActions::FindActorByNameOrLabel(UWorld* World, const TSharedPtr<FJsonObject>& Params)
{
	if (!World || !Params.IsValid()) return nullptr;

	const FString ActorName = Params->HasField(TEXT("actor_name"))
		? Params->GetStringField(TEXT("actor_name")) : FString();
	const FString ActorLabel = Params->HasField(TEXT("actor_label"))
		? Params->GetStringField(TEXT("actor_label")) : FString();
	const FString ClassName = Params->HasField(TEXT("class_name"))
		? Params->GetStringField(TEXT("class_name")) : FString();

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor)) continue;

		if (!ActorLabel.IsEmpty() && Actor->GetActorLabel() == ActorLabel) return Actor;
		if (!ActorName.IsEmpty() && Actor->GetName().Contains(ActorName)) return Actor;
		if (!ClassName.IsEmpty() && Actor->GetClass()->GetName().Contains(ClassName)) return Actor;
	}
	return nullptr;
}

UActorComponent* FMonolithBaseActions::FindComponentByName(AActor* Actor, const FString& CompName)
{
	if (!Actor) return nullptr;
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);
	for (UActorComponent* Comp : Components)
	{
		if (IsValid(Comp) && (Comp->GetName() == CompName || Comp->GetName().Contains(CompName)))
			return Comp;
	}
	return nullptr;
}

// 辅助：FProperty 值转 JSON
static TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Prop, const void* ValuePtr)
{
	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
	}
	if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		return MakeShared<FJsonValueNumber>(IntProp->GetPropertyValue(ValuePtr));
	}
	if (FInt64Property* Int64Prop = CastField<FInt64Property>(Prop))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(Int64Prop->GetPropertyValue(ValuePtr)));
	}
	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		return MakeShared<FJsonValueNumber>(FloatProp->GetPropertyValue(ValuePtr));
	}
	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		return MakeShared<FJsonValueNumber>(DoubleProp->GetPropertyValue(ValuePtr));
	}
	if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
	}
	if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
	}
	if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());
	}
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		FString EnumStr;
		Prop->ExportTextItem_Direct(EnumStr, ValuePtr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(EnumStr);
	}
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
		return MakeShared<FJsonValueString>(Obj ? Obj->GetPathName() : TEXT("null"));
	}
	if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		FString Path;
		Prop->ExportTextItem_Direct(Path, ValuePtr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Path);
	}
	// 其他类型：导出为文本
	FString ExportedText;
	Prop->ExportTextItem_Direct(ExportedText, ValuePtr, nullptr, nullptr, PPF_None);
	return MakeShared<FJsonValueString>(ExportedText);
}

// 辅助：JSON 值写入 FProperty
static bool JsonValueToProperty(FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal)
{
	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		BoolProp->SetPropertyValue(ValuePtr, JsonVal->AsBool());
		return true;
	}
	if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(JsonVal->AsNumber()));
		return true;
	}
	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(JsonVal->AsNumber()));
		return true;
	}
	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		DoubleProp->SetPropertyValue(ValuePtr, JsonVal->AsNumber());
		return true;
	}
	if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		StrProp->SetPropertyValue(ValuePtr, JsonVal->AsString());
		return true;
	}
	if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		NameProp->SetPropertyValue(ValuePtr, FName(*JsonVal->AsString()));
		return true;
	}
	if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
	{
		TextProp->SetPropertyValue(ValuePtr, FText::FromString(JsonVal->AsString()));
		return true;
	}
	// 通用：通过 ImportText 处理
	const FString TextValue = JsonVal->AsString();
	return Prop->ImportText_Direct(*TextValue, ValuePtr, nullptr, PPF_None) != nullptr;
}

// ============================================================================
// 注册
// ============================================================================

void FMonolithBaseActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// --- 地图管理（从 capture 迁移） ---
	Registry.RegisterAction(TEXT("base"), TEXT("get_current_map"),
		TEXT("获取当前打开地图的名称和路径"),
		FMonolithActionHandler::CreateStatic(&HandleGetCurrentMap),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("base"), TEXT("open_map"),
		TEXT("打开指定地图"),
		FMonolithActionHandler::CreateStatic(&HandleOpenMap),
		FParamSchemaBuilder()
			.Required(TEXT("map_path"), TEXT("string"), TEXT("地图资产路径，例如 /Game/Maps/DEMO1"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("save_map"),
		TEXT("保存当前打开的地图"),
		FMonolithActionHandler::CreateStatic(&HandleSaveMap),
		MakeShared<FJsonObject>());

	// --- Actor 查找与聚焦（从 capture 迁移） ---
	Registry.RegisterAction(TEXT("base"), TEXT("find_actors_by_class"),
		TEXT("按类名查找当前地图中的 Actor"),
		FMonolithActionHandler::CreateStatic(&HandleFindActorsByClass),
		FParamSchemaBuilder()
			.Optional(TEXT("class_name"), TEXT("string"), TEXT("类名模糊匹配过滤器"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("最大返回数量"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("select_and_focus"),
		TEXT("选中指定 Actor 并将视口聚焦到它"),
		FMonolithActionHandler::CreateStatic(&HandleSelectAndFocus),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Optional(TEXT("class_name"), TEXT("string"), TEXT("Actor 类名模糊匹配"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("focus_actor"),
		TEXT("计算相机位置并对准指定 Actor"),
		FMonolithActionHandler::CreateStatic(&HandleFocusActor),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Optional(TEXT("class_name"), TEXT("string"), TEXT("Actor 类名模糊匹配"))
			.Optional(TEXT("distance"), TEXT("number"), TEXT("观察距离，0 表示自动计算"), TEXT("0"))
			.Optional(TEXT("pitch"), TEXT("number"), TEXT("俯仰角度"), TEXT("-30"))
			.Optional(TEXT("yaw"), TEXT("number"), TEXT("偏航角度"), TEXT("45"))
			.Build());

	// --- Actor Tag 操作 ---
	Registry.RegisterAction(TEXT("base"), TEXT("get_actor_tags"),
		TEXT("获取 Actor 的所有 Tags"),
		FMonolithActionHandler::CreateStatic(&HandleGetActorTags),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_actor_tags"),
		TEXT("设置 Actor 的 Tags（替换全部）"),
		FMonolithActionHandler::CreateStatic(&HandleSetActorTags),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("tags"), TEXT("array"), TEXT("要设置的 Tag 字符串数组"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("add_actor_tag"),
		TEXT("给 Actor 添加一个 Tag（不重复）"),
		FMonolithActionHandler::CreateStatic(&HandleAddActorTag),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("tag"), TEXT("string"), TEXT("要添加的 Tag"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("remove_actor_tag"),
		TEXT("从 Actor 移除一个 Tag"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveActorTag),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("tag"), TEXT("string"), TEXT("要移除的 Tag"))
			.Build());

	// --- Actor 属性操作 ---
	Registry.RegisterAction(TEXT("base"), TEXT("get_actor_property"),
		TEXT("读取 Actor 的指定 UPROPERTY 属性值"),
		FMonolithActionHandler::CreateStatic(&HandleGetActorProperty),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("属性名"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_actor_property"),
		TEXT("修改 Actor 的指定 UPROPERTY 属性值"),
		FMonolithActionHandler::CreateStatic(&HandleSetActorProperty),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("属性名"))
			.Required(TEXT("value"), TEXT("any"), TEXT("属性值"))
			.Build());

	// --- Actor Transform ---
	Registry.RegisterAction(TEXT("base"), TEXT("get_actor_transform"),
		TEXT("获取 Actor 的位置、旋转和缩放"),
		FMonolithActionHandler::CreateStatic(&HandleGetActorTransform),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_actor_transform"),
		TEXT("设置 Actor 的位置、旋转和/或缩放"),
		FMonolithActionHandler::CreateStatic(&HandleSetActorTransform),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Optional(TEXT("location"), TEXT("object"), TEXT("{x,y,z} 位置"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("{pitch,yaw,roll} 旋转"))
			.Optional(TEXT("scale"), TEXT("object"), TEXT("{x,y,z} 缩放"))
			.Build());

	// --- Actor 生命周期 ---
	Registry.RegisterAction(TEXT("base"), TEXT("destroy_actor"),
		TEXT("销毁指定 Actor"),
		FMonolithActionHandler::CreateStatic(&HandleDestroyActor),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("confirm"), TEXT("boolean"), TEXT("必须为 true 才执行"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("spawn_actor"),
		TEXT("在场景中生成一个 Actor（通过类路径或蓝图路径）"),
		FMonolithActionHandler::CreateStatic(&HandleSpawnActor),
		FParamSchemaBuilder()
			.Required(TEXT("class_path"), TEXT("string"), TEXT("C++ 类路径或蓝图资产路径"))
			.Optional(TEXT("location"), TEXT("object"), TEXT("{x,y,z} 生成位置"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("{pitch,yaw,roll} 生成旋转"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("生成后设置的 Actor Label"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("list_actor_components"),
		TEXT("列出 Actor 的所有组件"),
		FMonolithActionHandler::CreateStatic(&HandleListActorComponents),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Build());

	// --- UObject 操作 ---
	Registry.RegisterAction(TEXT("base"), TEXT("load_object"),
		TEXT("按路径加载 UObject 并返回类名和关键属性"),
		FMonolithActionHandler::CreateStatic(&HandleLoadObject),
		FParamSchemaBuilder()
			.Required(TEXT("object_path"), TEXT("string"), TEXT("资产路径，如 /Game/Data/MyAsset.MyAsset"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("get_object_properties"),
		TEXT("读取已加载 UObject 的全部或指定属性"),
		FMonolithActionHandler::CreateStatic(&HandleGetObjectProperties),
		FParamSchemaBuilder()
			.Required(TEXT("object_path"), TEXT("string"), TEXT("资产路径"))
			.Optional(TEXT("properties"), TEXT("array"), TEXT("要读取的属性名列表，为空则返回全部可编辑属性"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_object_property"),
		TEXT("修改已加载 UObject 的指定属性"),
		FMonolithActionHandler::CreateStatic(&HandleSetObjectProperty),
		FParamSchemaBuilder()
			.Required(TEXT("object_path"), TEXT("string"), TEXT("资产路径"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("属性名"))
			.Required(TEXT("value"), TEXT("any"), TEXT("属性值"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("find_assets"),
		TEXT("按类型和名称在 Asset Registry 中搜索资产"),
		FMonolithActionHandler::CreateStatic(&HandleFindAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("class_name"), TEXT("string"), TEXT("资产类名过滤（如 StaticMesh, Blueprint, DataAsset）"))
			.Optional(TEXT("name_pattern"), TEXT("string"), TEXT("资产名称模糊匹配"))
			.Optional(TEXT("path_pattern"), TEXT("string"), TEXT("资产路径模糊匹配"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("最大返回数量"), TEXT("50"))
			.Build());

	// --- 组件管理 ---
	Registry.RegisterAction(TEXT("base"), TEXT("add_component"),
		TEXT("给 Actor 添加一个新组件（按组件类名）"),
		FMonolithActionHandler::CreateStatic(&HandleAddComponent),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("component_class"), TEXT("string"), TEXT("组件类名（如 StaticMeshComponent）或完整路径"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("新组件命名，默认使用类名"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("remove_component"),
		TEXT("从 Actor 上移除一个组件"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveComponent),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("要移除的组件名称"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("get_component_property"),
		TEXT("读取 Actor 上指定组件的 UPROPERTY 属性值"),
		FMonolithActionHandler::CreateStatic(&HandleGetComponentProperty),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("组件名称"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("属性名"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_component_property"),
		TEXT("修改 Actor 上指定组件的 UPROPERTY 属性值"),
		FMonolithActionHandler::CreateStatic(&HandleSetComponentProperty),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("组件名称"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("属性名"))
			.Required(TEXT("value"), TEXT("any"), TEXT("属性值"))
			.Build());

	// --- Actor 编辑 ---
	Registry.RegisterAction(TEXT("base"), TEXT("rename_actor"),
		TEXT("修改 Actor 的 Label（显示名称）"),
		FMonolithActionHandler::CreateStatic(&HandleRenameActor),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("new_label"), TEXT("string"), TEXT("新的 Actor Label"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("duplicate_actor"),
		TEXT("复制一个 Actor（使用源 Actor 作为模板）"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateActor),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("源 Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("源 Actor Label 精确匹配"))
			.Optional(TEXT("offset"), TEXT("object"), TEXT("{x,y,z} 相对源 Actor 的位置偏移"))
			.Optional(TEXT("new_label"), TEXT("string"), TEXT("新 Actor 的 Label"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_actor_mobility"),
		TEXT("设置 Actor 根组件的 Mobility（Static/Stationary/Movable）"),
		FMonolithActionHandler::CreateStatic(&HandleSetActorMobility),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("mobility"), TEXT("string"), TEXT("static / stationary / movable"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_actor_hidden"),
		TEXT("设置 Actor 的可见性（游戏中/编辑器中）"),
		FMonolithActionHandler::CreateStatic(&HandleSetActorHidden),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Optional(TEXT("hidden_in_game"), TEXT("boolean"), TEXT("是否在游戏中隐藏"))
			.Optional(TEXT("hidden_in_editor"), TEXT("boolean"), TEXT("是否在编辑器中隐藏"))
			.Build());

	// --- Mesh & Material 配套 ---
	Registry.RegisterAction(TEXT("base"), TEXT("set_static_mesh"),
		TEXT("给 Actor 的 StaticMeshComponent 设置 StaticMesh 资产"),
		FMonolithActionHandler::CreateStatic(&HandleSetStaticMesh),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("mesh_path"), TEXT("string"), TEXT("StaticMesh 资产路径"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("指定 StaticMeshComponent 名称，默认使用第一个"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_skeletal_mesh"),
		TEXT("给 Actor 的 SkeletalMeshComponent 设置 SkeletalMesh 资产"),
		FMonolithActionHandler::CreateStatic(&HandleSetSkeletalMesh),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("mesh_path"), TEXT("string"), TEXT("SkeletalMesh 资产路径"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("指定 SkeletalMeshComponent 名称，默认使用第一个"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_component_material"),
		TEXT("设置 PrimitiveComponent 上指定材质槽的材质"),
		FMonolithActionHandler::CreateStatic(&HandleSetComponentMaterial),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("material_path"), TEXT("string"), TEXT("MaterialInterface 资产路径"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("指定组件名称，默认使用第一个 PrimitiveComponent"))
			.Optional(TEXT("slot_index"), TEXT("integer"), TEXT("材质槽索引"), TEXT("0"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("get_component_materials"),
		TEXT("获取 PrimitiveComponent 上所有材质槽信息"),
		FMonolithActionHandler::CreateStatic(&HandleGetComponentMaterials),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("指定组件名称，默认使用第一个 PrimitiveComponent"))
			.Build());

	// --- Animation & AI 配套 ---
	Registry.RegisterAction(TEXT("base"), TEXT("set_anim_class"),
		TEXT("设置 SkeletalMeshComponent 的动画蓝图类（AnimInstance 子类）"),
		FMonolithActionHandler::CreateStatic(&HandleSetAnimClass),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("anim_class_path"), TEXT("string"), TEXT("AnimBlueprint 类路径（如 /Game/Anim/ABP_Hero.ABP_Hero_C）"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("指定 SkeletalMeshComponent 名称，默认使用第一个"))
			.Build());

	Registry.RegisterAction(TEXT("base"), TEXT("set_ai_controller_class"),
		TEXT("设置 Pawn 的 AIControllerClass"),
		FMonolithActionHandler::CreateStatic(&HandleSetAIControllerClass),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor（Pawn）名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("controller_class_path"), TEXT("string"), TEXT("AIController 类路径"))
			.Build());

	// --- Physics & Collision ---
	Registry.RegisterAction(TEXT("base"), TEXT("set_collision_preset"),
		TEXT("设置 PrimitiveComponent 的碰撞预设，可选同时开关物理模拟"),
		FMonolithActionHandler::CreateStatic(&HandleSetCollisionPreset),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Actor 名称模糊匹配"))
			.Optional(TEXT("actor_label"), TEXT("string"), TEXT("Actor Label 精确匹配"))
			.Required(TEXT("preset_name"), TEXT("string"), TEXT("碰撞预设名称（如 BlockAll, NoCollision, OverlapAll）"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("指定组件名称，默认使用第一个 PrimitiveComponent"))
			.Optional(TEXT("simulate_physics"), TEXT("boolean"), TEXT("是否开启物理模拟"))
			.Build());
}

// ============================================================================
// 地图管理（从 MonolithCapture 迁移）
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleGetCurrentMap(const TSharedPtr<FJsonObject>& Params)
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

FMonolithActionResult FMonolithBaseActions::HandleOpenMap(const TSharedPtr<FJsonObject>& Params)
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

FMonolithActionResult FMonolithBaseActions::HandleSaveMap(const TSharedPtr<FJsonObject>& Params)
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

// ============================================================================
// Actor 查找与聚焦（从 MonolithCapture 迁移）
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleFindActorsByClass(const TSharedPtr<FJsonObject>& Params)
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

	const FString ClassName = Params.IsValid() && Params->HasField(TEXT("class_name"))
		? Params->GetStringField(TEXT("class_name")) : FString();
	const int32 Limit = Params.IsValid() && Params->HasField(TEXT("limit"))
		? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	TArray<TSharedPtr<FJsonValue>> ActorArray;
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor)) continue;
		if (!ClassName.IsEmpty() && !Actor->GetClass()->GetName().Contains(ClassName)) continue;

		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Actor->GetName());
		ActorObj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

		const FVector Loc = Actor->GetActorLocation();
		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		ActorObj->SetObjectField(TEXT("location"), LocObj);

		// 返回 Tags
		TArray<TSharedPtr<FJsonValue>> TagArr;
		for (const FName& Tag : Actor->Tags)
		{
			TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		ActorObj->SetArrayField(TEXT("tags"), TagArr);

		FVector Origin, Extent;
		Actor->GetActorBounds(false, Origin, Extent);
		TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
		BoundsObj->SetNumberField(TEXT("extent_x"), Extent.X);
		BoundsObj->SetNumberField(TEXT("extent_y"), Extent.Y);
		BoundsObj->SetNumberField(TEXT("extent_z"), Extent.Z);
		ActorObj->SetObjectField(TEXT("bounds"), BoundsObj);

		ActorArray.Add(MakeShared<FJsonValueObject>(ActorObj));
		if (++Count >= Limit) break;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("actors"), ActorArray);
	Result->SetNumberField(TEXT("count"), Count);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSelectAndFocus(const TSharedPtr<FJsonObject>& Params)
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

	AActor* TargetActor = FindActorByNameOrLabel(World, Params);
	if (!TargetActor)
	{
		return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));
	}

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(TargetActor, true, true, true);
	GEditor->MoveViewportCamerasToActor(*TargetActor, true);
	GEditor->RedrawAllViewports(true);
	FlushRenderingCommands();

	FVector Origin, Extent;
	TargetActor->GetActorBounds(false, Origin, Extent);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), TargetActor->GetName());
	Result->SetStringField(TEXT("actor_label"), TargetActor->GetActorLabel());
	Result->SetStringField(TEXT("actor_class"), TargetActor->GetClass()->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleFocusActor(const TSharedPtr<FJsonObject>& Params)
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

	const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
	if (Clients.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("没有可用的编辑器视口"));
	}

	AActor* TargetActor = FindActorByNameOrLabel(World, Params);
	if (!TargetActor)
	{
		return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));
	}

	float Distance = Params.IsValid() && Params->HasField(TEXT("distance"))
		? static_cast<float>(Params->GetNumberField(TEXT("distance"))) : 0.0f;
	const float Pitch = Params.IsValid() && Params->HasField(TEXT("pitch"))
		? static_cast<float>(Params->GetNumberField(TEXT("pitch"))) : -30.0f;
	const float Yaw = Params.IsValid() && Params->HasField(TEXT("yaw"))
		? static_cast<float>(Params->GetNumberField(TEXT("yaw"))) : 45.0f;

	FVector Origin, Extent;
	TargetActor->GetActorBounds(false, Origin, Extent);
	const float MaxExtent = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
	if (Distance <= 0.0f)
	{
		Distance = FMath::Max(MaxExtent * 2.5f, 500.0f);
	}

	const FRotator LookRotation(Pitch, Yaw, 0.0f);
	const FVector CameraLocation = Origin + LookRotation.Vector() * -Distance;

	FLevelEditorViewportClient* ViewportClient = Clients[0];
	ViewportClient->SetViewLocation(CameraLocation);
	ViewportClient->SetViewRotation(LookRotation);
	ViewportClient->Tick(0.033f);
	if (ViewportClient->Viewport)
	{
		ViewportClient->Viewport->Invalidate();
		ViewportClient->Viewport->InvalidateDisplay();
	}
	GEditor->RedrawAllViewports(true);
	FlushRenderingCommands();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), TargetActor->GetName());
	TSharedPtr<FJsonObject> CameraObj = MakeShared<FJsonObject>();
	CameraObj->SetNumberField(TEXT("x"), CameraLocation.X);
	CameraObj->SetNumberField(TEXT("y"), CameraLocation.Y);
	CameraObj->SetNumberField(TEXT("z"), CameraLocation.Z);
	Result->SetObjectField(TEXT("camera_location"), CameraObj);
	Result->SetNumberField(TEXT("distance"), Distance);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Actor Tag 操作
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleGetActorTags(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	TArray<TSharedPtr<FJsonValue>> TagArr;
	for (const FName& Tag : Actor->Tags)
	{
		TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Result->SetArrayField(TEXT("tags"), TagArr);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetActorTags(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	if (!Params->HasField(TEXT("tags")))
		return FMonolithActionResult::Error(TEXT("缺少 tags 参数"));

	Actor->Modify();
	Actor->Tags.Empty();
	const TArray<TSharedPtr<FJsonValue>>& TagValues = Params->GetArrayField(TEXT("tags"));
	for (const auto& Val : TagValues)
	{
		Actor->Tags.AddUnique(FName(*Val->AsString()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetNumberField(TEXT("tag_count"), Actor->Tags.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleAddActorTag(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString TagStr = Params->GetStringField(TEXT("tag"));
	if (TagStr.IsEmpty()) return FMonolithActionResult::Error(TEXT("tag 不能为空"));

	Actor->Modify();
	const FName Tag(*TagStr);
	const bool bAdded = !Actor->Tags.Contains(Tag);
	Actor->Tags.AddUnique(Tag);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("added"), bAdded);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetNumberField(TEXT("tag_count"), Actor->Tags.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleRemoveActorTag(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString TagStr = Params->GetStringField(TEXT("tag"));
	if (TagStr.IsEmpty()) return FMonolithActionResult::Error(TEXT("tag 不能为空"));

	Actor->Modify();
	const int32 Removed = Actor->Tags.Remove(FName(*TagStr));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("removed"), Removed > 0);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetNumberField(TEXT("tag_count"), Actor->Tags.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Actor 属性操作
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleGetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString PropName = Params->GetStringField(TEXT("property_name"));
	FProperty* Prop = Actor->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 不存在"), *PropName));
	}

	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);
	TSharedPtr<FJsonValue> JsonVal = PropertyToJsonValue(Prop, ValuePtr);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("property_name"), PropName);
	Result->SetStringField(TEXT("property_type"), Prop->GetCPPType());
	Result->SetField(TEXT("value"), JsonVal);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetActorProperty(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString PropName = Params->GetStringField(TEXT("property_name"));
	FProperty* Prop = Actor->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 不存在"), *PropName));
	}

	Actor->Modify();
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);
	TSharedPtr<FJsonValue> JsonVal = Params->TryGetField(TEXT("value"));
	if (!JsonVal.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("缺少 value 参数"));
	}

	if (!JsonValueToProperty(Prop, ValuePtr, JsonVal))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 值设置失败"), *PropName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("property_name"), PropName);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Actor Transform
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleGetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FVector Loc = Actor->GetActorLocation();
	const FRotator Rot = Actor->GetActorRotation();
	const FVector Scale = Actor->GetActorScale3D();

	TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);

	TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	RotObj->SetNumberField(TEXT("roll"), Rot.Roll);

	TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
	ScaleObj->SetNumberField(TEXT("x"), Scale.X);
	ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
	ScaleObj->SetNumberField(TEXT("z"), Scale.Z);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
	Result->SetObjectField(TEXT("location"), LocObj);
	Result->SetObjectField(TEXT("rotation"), RotObj);
	Result->SetObjectField(TEXT("scale"), ScaleObj);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetActorTransform(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	Actor->Modify();

	if (Params->HasField(TEXT("location")))
	{
		const TSharedPtr<FJsonObject>& LocObj = Params->GetObjectField(TEXT("location"));
		Actor->SetActorLocation(FVector(
			LocObj->GetNumberField(TEXT("x")),
			LocObj->GetNumberField(TEXT("y")),
			LocObj->GetNumberField(TEXT("z"))));
	}

	if (Params->HasField(TEXT("rotation")))
	{
		const TSharedPtr<FJsonObject>& RotObj = Params->GetObjectField(TEXT("rotation"));
		Actor->SetActorRotation(FRotator(
			RotObj->GetNumberField(TEXT("pitch")),
			RotObj->GetNumberField(TEXT("yaw")),
			RotObj->GetNumberField(TEXT("roll"))));
	}

	if (Params->HasField(TEXT("scale")))
	{
		const TSharedPtr<FJsonObject>& ScaleObj = Params->GetObjectField(TEXT("scale"));
		Actor->SetActorScale3D(FVector(
			ScaleObj->GetNumberField(TEXT("x")),
			ScaleObj->GetNumberField(TEXT("y")),
			ScaleObj->GetNumberField(TEXT("z"))));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Actor 生命周期
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleDestroyActor(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	if (!Params->HasField(TEXT("confirm")) || !Params->GetBoolField(TEXT("confirm")))
	{
		return FMonolithActionResult::Error(TEXT("confirm 必须为 true"));
	}

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString ActorName = Actor->GetName();
	World->EditorDestroyActor(Actor, true);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("destroyed"), ActorName);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	const FString ClassPath = Params->GetStringField(TEXT("class_path"));
	if (ClassPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 class_path"));

	// 尝试加载蓝图
	UClass* ActorClass = nullptr;
	UObject* LoadedObj = StaticLoadObject(UObject::StaticClass(), nullptr, *ClassPath);
	if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
	{
		ActorClass = BP->GeneratedClass;
	}
	else if (UBlueprintGeneratedClass* BPG = Cast<UBlueprintGeneratedClass>(LoadedObj))
	{
		ActorClass = BPG;
	}
	else
	{
		ActorClass = FindObject<UClass>(nullptr, *ClassPath);
	}

	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("无法加载 Actor 类: %s"), *ClassPath));
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	if (Params->HasField(TEXT("location")))
	{
		const TSharedPtr<FJsonObject>& LocObj = Params->GetObjectField(TEXT("location"));
		Location = FVector(LocObj->GetNumberField(TEXT("x")), LocObj->GetNumberField(TEXT("y")), LocObj->GetNumberField(TEXT("z")));
	}
	if (Params->HasField(TEXT("rotation")))
	{
		const TSharedPtr<FJsonObject>& RotObj = Params->GetObjectField(TEXT("rotation"));
		Rotation = FRotator(RotObj->GetNumberField(TEXT("pitch")), RotObj->GetNumberField(TEXT("yaw")), RotObj->GetNumberField(TEXT("roll")));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* NewActor = World->SpawnActor<AActor>(ActorClass, Location, Rotation, SpawnParams);
	if (!NewActor)
	{
		return FMonolithActionResult::Error(TEXT("Actor 生成失败"));
	}

	if (Params->HasField(TEXT("label")))
	{
		NewActor->SetActorLabel(Params->GetStringField(TEXT("label")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), NewActor->GetName());
	Result->SetStringField(TEXT("actor_label"), NewActor->GetActorLabel());
	Result->SetStringField(TEXT("actor_class"), NewActor->GetClass()->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleListActorComponents(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	TArray<TSharedPtr<FJsonValue>> CompArray;
	for (UActorComponent* Comp : Components)
	{
		if (!IsValid(Comp)) continue;
		TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Comp->GetName());
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

		if (USceneComponent* SceneComp = Cast<USceneComponent>(Comp))
		{
			const FVector RelLoc = SceneComp->GetRelativeLocation();
			TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
			LocObj->SetNumberField(TEXT("x"), RelLoc.X);
			LocObj->SetNumberField(TEXT("y"), RelLoc.Y);
			LocObj->SetNumberField(TEXT("z"), RelLoc.Z);
			CompObj->SetObjectField(TEXT("relative_location"), LocObj);
		}

		CompArray.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetArrayField(TEXT("components"), CompArray);
	Result->SetNumberField(TEXT("count"), CompArray.Num());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// UObject 操作
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleLoadObject(const TSharedPtr<FJsonObject>& Params)
{
	const FString ObjPath = Params->GetStringField(TEXT("object_path"));
	if (ObjPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 object_path"));

	UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjPath);
	if (!Obj)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("加载失败: %s"), *ObjPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
	Result->SetStringField(TEXT("class_path"), Obj->GetClass()->GetPathName());
	Result->SetStringField(TEXT("outer"), Obj->GetOuter() ? Obj->GetOuter()->GetName() : TEXT("null"));

	// 列出可编辑属性名
	TArray<TSharedPtr<FJsonValue>> PropNames;
	for (TFieldIterator<FProperty> PropIt(Obj->GetClass()); PropIt; ++PropIt)
	{
		if (PropIt->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			PropNames.Add(MakeShared<FJsonValueString>(PropIt->GetName()));
		}
	}
	Result->SetArrayField(TEXT("editable_properties"), PropNames);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleGetObjectProperties(const TSharedPtr<FJsonObject>& Params)
{
	const FString ObjPath = Params->GetStringField(TEXT("object_path"));
	if (ObjPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 object_path"));

	UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjPath);
	if (!Obj) return FMonolithActionResult::Error(FString::Printf(TEXT("加载失败: %s"), *ObjPath));

	// 如果指定了属性列表则过滤
	TSet<FString> FilterProps;
	if (Params->HasField(TEXT("properties")))
	{
		for (const auto& Val : Params->GetArrayField(TEXT("properties")))
		{
			FilterProps.Add(Val->AsString());
		}
	}

	TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Obj->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!FilterProps.IsEmpty() && !FilterProps.Contains(Prop->GetName())) continue;
		if (FilterProps.IsEmpty() && !Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) continue;

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
		PropsObj->SetField(Prop->GetName(), PropertyToJsonValue(Prop, ValuePtr));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
	Result->SetObjectField(TEXT("properties"), PropsObj);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetObjectProperty(const TSharedPtr<FJsonObject>& Params)
{
	const FString ObjPath = Params->GetStringField(TEXT("object_path"));
	if (ObjPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 object_path"));

	UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjPath);
	if (!Obj) return FMonolithActionResult::Error(FString::Printf(TEXT("加载失败: %s"), *ObjPath));

	const FString PropName = Params->GetStringField(TEXT("property_name"));
	FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 不存在"), *PropName));

	Obj->Modify();
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
	TSharedPtr<FJsonValue> JsonVal = Params->TryGetField(TEXT("value"));
	if (!JsonVal.IsValid()) return FMonolithActionResult::Error(TEXT("缺少 value 参数"));

	if (!JsonValueToProperty(Prop, ValuePtr, JsonVal))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 值设置失败"), *PropName));
	}

	Obj->MarkPackageDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("property_name"), PropName);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleFindAssets(const TSharedPtr<FJsonObject>& Params)
{
	const FString ClassName = Params.IsValid() && Params->HasField(TEXT("class_name"))
		? Params->GetStringField(TEXT("class_name")) : FString();
	const FString NamePattern = Params.IsValid() && Params->HasField(TEXT("name_pattern"))
		? Params->GetStringField(TEXT("name_pattern")) : FString();
	const FString PathPattern = Params.IsValid() && Params->HasField(TEXT("path_pattern"))
		? Params->GetStringField(TEXT("path_pattern")) : FString();
	const int32 Limit = Params.IsValid() && Params->HasField(TEXT("limit"))
		? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FARFilter Filter;
	if (!PathPattern.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathPattern));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAllAssets(AssetList, true);

	TArray<TSharedPtr<FJsonValue>> ResultArray;
	int32 Count = 0;
	for (const FAssetData& Asset : AssetList)
	{
		if (Count >= Limit) break;

		if (!ClassName.IsEmpty())
		{
			const FString AssetClassName = Asset.AssetClassPath.GetAssetName().ToString();
			if (!AssetClassName.Contains(ClassName)) continue;
		}
		if (!NamePattern.IsEmpty() && !Asset.AssetName.ToString().Contains(NamePattern)) continue;
		if (!PathPattern.IsEmpty() && !Asset.GetObjectPathString().Contains(PathPattern)) continue;

		TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		AssetObj->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
		AssetObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		AssetObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());
		ResultArray.Add(MakeShared<FJsonValueObject>(AssetObj));
		Count++;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("assets"), ResultArray);
	Result->SetNumberField(TEXT("count"), Count);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 组件管理
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleAddComponent(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString ClassName = Params->GetStringField(TEXT("component_class"));
	if (ClassName.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 component_class"));

	const FString CompName = Params->HasField(TEXT("component_name"))
		? Params->GetStringField(TEXT("component_name")) : ClassName;

	// 查找组件类：先尝试完整路径，再尝试 /Script/Engine.ClassName
	UClass* CompClass = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassName);
	if (!CompClass)
	{
		const FString ScriptPath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
		CompClass = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ScriptPath);
	}
	if (!CompClass || !CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("组件类 '%s' 无效"), *ClassName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Add Component")));
	Actor->Modify();

	UActorComponent* NewComp = NewObject<UActorComponent>(Actor, CompClass, FName(*CompName));
	if (!NewComp)
	{
		return FMonolithActionResult::Error(TEXT("组件创建失败"));
	}

	if (USceneComponent* SceneComp = Cast<USceneComponent>(NewComp))
	{
		if (Actor->GetRootComponent())
		{
			SceneComp->SetupAttachment(Actor->GetRootComponent());
		}
		else
		{
			Actor->SetRootComponent(SceneComp);
		}
	}

	Actor->AddInstanceComponent(NewComp);
	NewComp->RegisterComponent();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("component_name"), NewComp->GetName());
	Result->SetStringField(TEXT("component_class"), NewComp->GetClass()->GetName());
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString CompName = Params->GetStringField(TEXT("component_name"));
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 component_name"));

	UActorComponent* Comp = FindComponentByName(Actor, CompName);
	if (!Comp) return FMonolithActionResult::Error(FString::Printf(TEXT("组件 '%s' 不存在"), *CompName));

	if (Cast<USceneComponent>(Comp) == Actor->GetRootComponent())
	{
		return FMonolithActionResult::Error(TEXT("不能删除根组件"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Remove Component")));
	Actor->Modify();
	const FString RemovedName = Comp->GetName();
	Actor->RemoveInstanceComponent(Comp);
	Comp->DestroyComponent();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("removed"), RemovedName);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleGetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString CompName = Params->GetStringField(TEXT("component_name"));
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 component_name"));

	UActorComponent* Comp = FindComponentByName(Actor, CompName);
	if (!Comp) return FMonolithActionResult::Error(FString::Printf(TEXT("组件 '%s' 不存在"), *CompName));

	const FString PropName = Params->GetStringField(TEXT("property_name"));
	FProperty* Prop = Comp->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 不存在于 %s"), *PropName, *Comp->GetClass()->GetName()));

	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
	TSharedPtr<FJsonValue> JsonVal = PropertyToJsonValue(Prop, ValuePtr);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("component_name"), Comp->GetName());
	Result->SetStringField(TEXT("property_name"), PropName);
	Result->SetStringField(TEXT("property_type"), Prop->GetCPPType());
	Result->SetField(TEXT("value"), JsonVal);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString CompName = Params->GetStringField(TEXT("component_name"));
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 component_name"));

	UActorComponent* Comp = FindComponentByName(Actor, CompName);
	if (!Comp) return FMonolithActionResult::Error(FString::Printf(TEXT("组件 '%s' 不存在"), *CompName));

	const FString PropName = Params->GetStringField(TEXT("property_name"));
	FProperty* Prop = Comp->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop) return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 不存在于 %s"), *PropName, *Comp->GetClass()->GetName()));

	TSharedPtr<FJsonValue> JsonVal = Params->TryGetField(TEXT("value"));
	if (!JsonVal.IsValid()) return FMonolithActionResult::Error(TEXT("缺少 value 参数"));

	Comp->Modify();
	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
	if (!JsonValueToProperty(Prop, ValuePtr, JsonVal))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("属性 '%s' 值设置失败"), *PropName));
	}
	Comp->MarkRenderStateDirty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("component_name"), Comp->GetName());
	Result->SetStringField(TEXT("property_name"), PropName);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Actor 编辑
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleRenameActor(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	const FString NewLabel = Params->GetStringField(TEXT("new_label"));
	if (NewLabel.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 new_label"));

	Actor->Modify();
	const FString OldLabel = Actor->GetActorLabel();
	Actor->SetActorLabel(NewLabel);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("old_label"), OldLabel);
	Result->SetStringField(TEXT("new_label"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleDuplicateActor(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* SourceActor = FindActorByNameOrLabel(World, Params);
	if (!SourceActor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	FScopedTransaction Transaction(FText::FromString(TEXT("Duplicate Actor")));

	FVector Location = SourceActor->GetActorLocation();
	FRotator Rotation = SourceActor->GetActorRotation();

	if (Params->HasField(TEXT("offset")))
	{
		const TSharedPtr<FJsonObject>& OffsetObj = Params->GetObjectField(TEXT("offset"));
		Location.X += OffsetObj->GetNumberField(TEXT("x"));
		Location.Y += OffsetObj->GetNumberField(TEXT("y"));
		Location.Z += OffsetObj->GetNumberField(TEXT("z"));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Template = SourceActor;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* NewActor = World->SpawnActor<AActor>(SourceActor->GetClass(), Location, Rotation, SpawnParams);

	if (!NewActor)
	{
		return FMonolithActionResult::Error(TEXT("复制 Actor 失败"));
	}

	if (Params->HasField(TEXT("new_label")))
	{
		NewActor->SetActorLabel(Params->GetStringField(TEXT("new_label")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("source_actor"), SourceActor->GetName());
	Result->SetStringField(TEXT("new_actor_name"), NewActor->GetName());
	Result->SetStringField(TEXT("new_actor_label"), NewActor->GetActorLabel());
	Result->SetStringField(TEXT("actor_class"), NewActor->GetClass()->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetActorMobility(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	USceneComponent* Root = Actor->GetRootComponent();
	if (!Root) return FMonolithActionResult::Error(TEXT("Actor 没有根组件"));

	const FString MobilityStr = Params->GetStringField(TEXT("mobility")).ToLower();
	EComponentMobility::Type Mobility;
	if (MobilityStr == TEXT("static")) Mobility = EComponentMobility::Static;
	else if (MobilityStr == TEXT("stationary")) Mobility = EComponentMobility::Stationary;
	else if (MobilityStr == TEXT("movable")) Mobility = EComponentMobility::Movable;
	else return FMonolithActionResult::Error(TEXT("mobility 必须为 static / stationary / movable"));

	Actor->Modify();
	Root->SetMobility(Mobility);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("mobility"), MobilityStr);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetActorHidden(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	Actor->Modify();

	if (Params->HasField(TEXT("hidden_in_game")))
	{
		Actor->SetActorHiddenInGame(Params->GetBoolField(TEXT("hidden_in_game")));
	}
	if (Params->HasField(TEXT("hidden_in_editor")))
	{
		Actor->SetIsTemporarilyHiddenInEditor(Params->GetBoolField(TEXT("hidden_in_editor")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetBoolField(TEXT("hidden_in_game"), Actor->IsHidden());
	Result->SetBoolField(TEXT("hidden_in_editor"), Actor->IsTemporarilyHiddenInEditor());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Mesh & Material 配套
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleSetStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	UStaticMeshComponent* SMComp = nullptr;
	if (Params->HasField(TEXT("component_name")))
	{
		UActorComponent* Comp = FindComponentByName(Actor, Params->GetStringField(TEXT("component_name")));
		SMComp = Cast<UStaticMeshComponent>(Comp);
	}
	else
	{
		SMComp = Actor->FindComponentByClass<UStaticMeshComponent>();
	}
	if (!SMComp) return FMonolithActionResult::Error(TEXT("未找到 StaticMeshComponent"));

	const FString MeshPath = Params->GetStringField(TEXT("mesh_path"));
	if (MeshPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 mesh_path"));

	UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshPath));
	if (!Mesh) return FMonolithActionResult::Error(FString::Printf(TEXT("加载 StaticMesh 失败: %s"), *MeshPath));

	Actor->Modify();
	SMComp->SetStaticMesh(Mesh);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("component_name"), SMComp->GetName());
	Result->SetStringField(TEXT("mesh_path"), Mesh->GetPathName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	USkeletalMeshComponent* SKComp = nullptr;
	if (Params->HasField(TEXT("component_name")))
	{
		UActorComponent* Comp = FindComponentByName(Actor, Params->GetStringField(TEXT("component_name")));
		SKComp = Cast<USkeletalMeshComponent>(Comp);
	}
	else
	{
		SKComp = Actor->FindComponentByClass<USkeletalMeshComponent>();
	}
	if (!SKComp) return FMonolithActionResult::Error(TEXT("未找到 SkeletalMeshComponent"));

	const FString MeshPath = Params->GetStringField(TEXT("mesh_path"));
	if (MeshPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 mesh_path"));

	USkeletalMesh* Mesh = Cast<USkeletalMesh>(StaticLoadObject(USkeletalMesh::StaticClass(), nullptr, *MeshPath));
	if (!Mesh) return FMonolithActionResult::Error(FString::Printf(TEXT("加载 SkeletalMesh 失败: %s"), *MeshPath));

	Actor->Modify();
	SKComp->SetSkeletalMeshAsset(Mesh);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("component_name"), SKComp->GetName());
	Result->SetStringField(TEXT("mesh_path"), Mesh->GetPathName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetComponentMaterial(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	UPrimitiveComponent* PrimComp = nullptr;
	if (Params->HasField(TEXT("component_name")))
	{
		UActorComponent* Comp = FindComponentByName(Actor, Params->GetStringField(TEXT("component_name")));
		PrimComp = Cast<UPrimitiveComponent>(Comp);
	}
	else
	{
		PrimComp = Actor->FindComponentByClass<UPrimitiveComponent>();
	}
	if (!PrimComp) return FMonolithActionResult::Error(TEXT("未找到 PrimitiveComponent"));

	const FString MaterialPath = Params->GetStringField(TEXT("material_path"));
	if (MaterialPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 material_path"));

	const int32 SlotIndex = Params->HasField(TEXT("slot_index"))
		? static_cast<int32>(Params->GetNumberField(TEXT("slot_index"))) : 0;

	UMaterialInterface* Material = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, *MaterialPath));
	if (!Material) return FMonolithActionResult::Error(FString::Printf(TEXT("加载材质失败: %s"), *MaterialPath));

	if (SlotIndex < 0 || SlotIndex >= PrimComp->GetNumMaterials())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("材质槽索引 %d 超出范围 [0, %d)"), SlotIndex, PrimComp->GetNumMaterials()));
	}

	Actor->Modify();
	PrimComp->SetMaterial(SlotIndex, Material);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("component_name"), PrimComp->GetName());
	Result->SetNumberField(TEXT("slot_index"), SlotIndex);
	Result->SetStringField(TEXT("material_path"), Material->GetPathName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleGetComponentMaterials(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	UPrimitiveComponent* PrimComp = nullptr;
	if (Params->HasField(TEXT("component_name")))
	{
		UActorComponent* Comp = FindComponentByName(Actor, Params->GetStringField(TEXT("component_name")));
		PrimComp = Cast<UPrimitiveComponent>(Comp);
	}
	else
	{
		PrimComp = Actor->FindComponentByClass<UPrimitiveComponent>();
	}
	if (!PrimComp) return FMonolithActionResult::Error(TEXT("未找到 PrimitiveComponent"));

	TArray<TSharedPtr<FJsonValue>> MaterialArray;
	for (int32 i = 0; i < PrimComp->GetNumMaterials(); ++i)
	{
		TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
		MatObj->SetNumberField(TEXT("slot_index"), i);
		UMaterialInterface* Mat = PrimComp->GetMaterial(i);
		MatObj->SetStringField(TEXT("material_name"), Mat ? Mat->GetName() : TEXT("null"));
		MatObj->SetStringField(TEXT("material_path"), Mat ? Mat->GetPathName() : TEXT("null"));
		MaterialArray.Add(MakeShared<FJsonValueObject>(MatObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("component_name"), PrimComp->GetName());
	Result->SetArrayField(TEXT("materials"), MaterialArray);
	Result->SetNumberField(TEXT("slot_count"), PrimComp->GetNumMaterials());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Animation & AI 配套
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleSetAnimClass(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	USkeletalMeshComponent* SKComp = nullptr;
	if (Params->HasField(TEXT("component_name")))
	{
		UActorComponent* Comp = FindComponentByName(Actor, Params->GetStringField(TEXT("component_name")));
		SKComp = Cast<USkeletalMeshComponent>(Comp);
	}
	else
	{
		SKComp = Actor->FindComponentByClass<USkeletalMeshComponent>();
	}
	if (!SKComp) return FMonolithActionResult::Error(TEXT("未找到 SkeletalMeshComponent"));

	const FString ClassPath = Params->GetStringField(TEXT("anim_class_path"));
	if (ClassPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 anim_class_path"));

	UClass* AnimClass = StaticLoadClass(UAnimInstance::StaticClass(), nullptr, *ClassPath);
	if (!AnimClass) return FMonolithActionResult::Error(FString::Printf(TEXT("加载动画蓝图类失败: %s"), *ClassPath));

	Actor->Modify();
	SKComp->SetAnimInstanceClass(AnimClass);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("component_name"), SKComp->GetName());
	Result->SetStringField(TEXT("anim_class"), AnimClass->GetName());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithBaseActions::HandleSetAIControllerClass(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	APawn* Pawn = Cast<APawn>(Actor);
	if (!Pawn) return FMonolithActionResult::Error(TEXT("目标 Actor 不是 Pawn"));

	const FString ClassPath = Params->GetStringField(TEXT("controller_class_path"));
	if (ClassPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 controller_class_path"));

	UClass* ControllerClass = StaticLoadClass(AController::StaticClass(), nullptr, *ClassPath);
	if (!ControllerClass) return FMonolithActionResult::Error(FString::Printf(TEXT("加载 AIController 类失败: %s"), *ClassPath));

	Pawn->Modify();
	Pawn->AIControllerClass = TSubclassOf<AController>(ControllerClass);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("controller_class"), ControllerClass->GetName());
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Physics & Collision
// ============================================================================

FMonolithActionResult FMonolithBaseActions::HandleSetCollisionPreset(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor 不可用"));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return FMonolithActionResult::Error(TEXT("当前没有打开的地图"));

	AActor* Actor = FindActorByNameOrLabel(World, Params);
	if (!Actor) return FMonolithActionResult::Error(TEXT("未找到匹配的 Actor"));

	UPrimitiveComponent* PrimComp = nullptr;
	if (Params->HasField(TEXT("component_name")))
	{
		UActorComponent* Comp = FindComponentByName(Actor, Params->GetStringField(TEXT("component_name")));
		PrimComp = Cast<UPrimitiveComponent>(Comp);
	}
	else
	{
		PrimComp = Actor->FindComponentByClass<UPrimitiveComponent>();
	}
	if (!PrimComp) return FMonolithActionResult::Error(TEXT("未找到 PrimitiveComponent"));

	const FString PresetName = Params->GetStringField(TEXT("preset_name"));
	if (PresetName.IsEmpty()) return FMonolithActionResult::Error(TEXT("缺少 preset_name"));

	Actor->Modify();
	PrimComp->SetCollisionProfileName(FName(*PresetName));

	if (Params->HasField(TEXT("simulate_physics")))
	{
		PrimComp->SetSimulatePhysics(Params->GetBoolField(TEXT("simulate_physics")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor_name"), Actor->GetName());
	Result->SetStringField(TEXT("component_name"), PrimComp->GetName());
	Result->SetStringField(TEXT("preset_name"), PresetName);
	return FMonolithActionResult::Success(Result);
}
