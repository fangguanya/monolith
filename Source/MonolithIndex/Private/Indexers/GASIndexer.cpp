#include "Indexers/GASIndexer.h"

#include "Abilities/GameplayAbility.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AttributeSet.h"
#include "Engine/Blueprint.h"
#include "GameplayCueNotify_Actor.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Indexers/MonolithSimpleArtifactSerialization.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

/*
 * 这份实现只做一件事：
 * “把单个 GAS Blueprint 的关键玩法配置，整理成 1 条结构化 node。”
 *
 * 这轮继续往前收口后，GAS 不再分别维护：
 * - 直接写生产表的一套逻辑；
 * - 构建 artifact 的一套逻辑；
 * - 写 shadow 表的另一套逻辑。
 *
 * 现在统一改成：
 * 1. 先构建 1 条稳定 node；
 * 2. 再根据需要把这条 node 序列化成 artifact；
 * 3. 最后由统一 helper 回放到生产表或 shadow 表。
 *
 * 这样 full / incremental / live / shadow / warmup / DDC
 * 就都在复用同一份“GAS 资产到底长什么样”的真相来源。
 */

namespace GASIndexerInternal
{
	/** 这份 Blueprint 到底属于哪一类 GAS 资产。 */
	enum class EGASBlueprintKind : uint8
	{
		/** 不是 GAS Blueprint。 */
		None,
		/** GameplayAbility Blueprint。 */
		GameplayAbility,
		/** GameplayEffect Blueprint。 */
		GameplayEffect,
		/** AttributeSet Blueprint。 */
		AttributeSet,
		/** GameplayCue 静态通知 Blueprint。 */
		GameplayCueStatic,
		/** GameplayCue Actor 通知 Blueprint。 */
		GameplayCueActor,
	};

	/** 两个常见 Blueprint 父类标签名。 */
	static const FName ParentClassTagName(TEXT("ParentClass"));
	static const FName NativeParentClassTagName(TEXT("NativeParentClass"));

	/** 把 JSON 对象压成紧凑字符串，方便直接存进 node.properties。 */
	static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
	{
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	/** 默认 node 名更偏向“人类正在操作的那份资产名”，而不是生成类名。 */
	static FString ResolveNodeName(const FAssetData& AssetData, const UBlueprint* Blueprint)
	{
		if (!AssetData.AssetName.IsNone())
		{
			return AssetData.AssetName.ToString();
		}

		if (Blueprint)
		{
			return Blueprint->GetName();
		}

		return TEXT("UnnamedGASAsset");
	}

	/** 从 AssetData 里拿父类标签。
	 * Blueprint 资产有时写 `NativeParentClass`，有时写 `ParentClass`，这里统一兜底。 */
	static FString GetBlueprintParentClassTag(const FAssetData& AssetData)
	{
		FString ParentClassPath;
		if (AssetData.GetTagValue(NativeParentClassTagName, ParentClassPath) && !ParentClassPath.IsEmpty())
		{
			return ParentClassPath;
		}

		AssetData.GetTagValue(ParentClassTagName, ParentClassPath);
		return ParentClassPath;
	}

	/** 仅凭父类路径字符串判断 GAS 类型。
	 * 这里故意使用 `Contains(...)`，因为不同工程/蓝图层级下路径形式会有细微差异，
	 * 但类型名关键字保持稳定。 */
	static EGASBlueprintKind ResolveKindFromParentClassPath(const FString& ParentClassPath)
	{
		if (ParentClassPath.Contains(TEXT("GameplayAbility")))
		{
			return EGASBlueprintKind::GameplayAbility;
		}
		if (ParentClassPath.Contains(TEXT("GameplayEffect")))
		{
			return EGASBlueprintKind::GameplayEffect;
		}
		if (ParentClassPath.Contains(TEXT("AttributeSet")))
		{
			return EGASBlueprintKind::AttributeSet;
		}
		if (ParentClassPath.Contains(TEXT("GameplayCueNotify_Actor")))
		{
			return EGASBlueprintKind::GameplayCueActor;
		}
		if (ParentClassPath.Contains(TEXT("GameplayCueNotify_Static")) || ParentClassPath.Contains(TEXT("GameplayCueNotify")))
		{
			return EGASBlueprintKind::GameplayCueStatic;
		}

		return EGASBlueprintKind::None;
	}

	/** 如果 Blueprint 已经加载，就优先用 GeneratedClass 做更准确的类型判断。 */
	static EGASBlueprintKind ResolveKindFromLoadedBlueprint(const UBlueprint* Blueprint)
	{
		if (!Blueprint || !Blueprint->GeneratedClass)
		{
			return EGASBlueprintKind::None;
		}

		const UClass* GeneratedClass = Blueprint->GeneratedClass;
		if (GeneratedClass->IsChildOf(UGameplayAbility::StaticClass()))
		{
			return EGASBlueprintKind::GameplayAbility;
		}
		if (GeneratedClass->IsChildOf(UGameplayEffect::StaticClass()))
		{
			return EGASBlueprintKind::GameplayEffect;
		}
		if (GeneratedClass->IsChildOf(UAttributeSet::StaticClass()))
		{
			return EGASBlueprintKind::AttributeSet;
		}
		if (GeneratedClass->IsChildOf(AGameplayCueNotify_Actor::StaticClass()))
		{
			return EGASBlueprintKind::GameplayCueActor;
		}
		if (GeneratedClass->IsChildOf(UGameplayCueNotify_Static::StaticClass()))
		{
			return EGASBlueprintKind::GameplayCueStatic;
		}

		return EGASBlueprintKind::None;
	}

	/** 统一解析“这份资产是不是 GAS Blueprint、具体是哪种”。 */
	static EGASBlueprintKind ResolveBlueprintKind(const FAssetData& AssetData, const UBlueprint* Blueprint)
	{
		const EGASBlueprintKind LoadedKind = ResolveKindFromLoadedBlueprint(Blueprint);
		if (LoadedKind != EGASBlueprintKind::None)
		{
			return LoadedKind;
		}

		return ResolveKindFromParentClassPath(GetBlueprintParentClassTag(AssetData));
	}

	/** 把 GameplayTagContainer 展平成 JSON 字符串数组。 */
	static TArray<TSharedPtr<FJsonValue>> ExtractTagContainer(const UObject* Object, const TCHAR* PropertyName)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		if (!Object)
		{
			return Result;
		}

		const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		if (!StructProperty)
		{
			return Result;
		}

		const void* ValuePtr = StructProperty->ContainerPtrToValuePtr<void>(Object);
		const FGameplayTagContainer* TagContainer = static_cast<const FGameplayTagContainer*>(ValuePtr);
		if (!TagContainer)
		{
			return Result;
		}

		for (const FGameplayTag& Tag : *TagContainer)
		{
			Result.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		return Result;
	}

	/** 读取枚举属性并转成人类可读字符串。 */
	static bool TryReadEnumPropertyAsString(const UObject* Object, const TCHAR* PropertyName, FString& OutValue)
	{
		OutValue.Reset();
		if (!Object)
		{
			return false;
		}

		const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return false;
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const FNumericProperty* UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
			const void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(Object);
			const UEnum* EnumDefinition = EnumProperty->GetEnum();
			if (!UnderlyingProperty || !EnumDefinition || !ValuePtr)
			{
				return false;
			}

			OutValue = EnumDefinition->GetNameStringByValue(UnderlyingProperty->GetSignedIntPropertyValue(ValuePtr));
			return !OutValue.IsEmpty();
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (!ByteProperty->Enum)
			{
				return false;
			}

			OutValue = ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetPropertyValue_InContainer(Object));
			return !OutValue.IsEmpty();
		}

		return false;
	}

	/** 读取 float/double 数值属性。 */
	static bool TryReadNumericPropertyAsDouble(const UObject* Object, const TCHAR* PropertyName, double& OutValue)
	{
		OutValue = 0.0;
		if (!Object)
		{
			return false;
		}

		const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			OutValue = FloatProperty->GetPropertyValue_InContainer(Object);
			return true;
		}
		if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
		{
			OutValue = DoubleProperty->GetPropertyValue_InContainer(Object);
			return true;
		}

		return false;
	}

	/** 读取类引用属性，并导出成完整路径。 */
	static bool TryReadClassPropertyPath(const UObject* Object, const TCHAR* PropertyName, FString& OutPath)
	{
		OutPath.Reset();
		if (!Object)
		{
			return false;
		}

		const FClassProperty* ClassProperty = CastField<FClassProperty>(Object->GetClass()->FindPropertyByName(PropertyName));
		if (!ClassProperty)
		{
			return false;
		}

		const UClass* ReferencedClass = Cast<UClass>(ClassProperty->GetPropertyValue_InContainer(Object));
		if (!ReferencedClass)
		{
			return false;
		}

		OutPath = ReferencedClass->GetPathName();
		return !OutPath.IsEmpty();
	}

	/** 为 node 准备统一的“公共头部字段”。 */
	static void PopulateCommonNodeFields(
		const FAssetData& AssetData,
		const UBlueprint* Blueprint,
		TSharedPtr<FJsonObject>& Properties)
	{
		Properties->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Properties->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		if (Blueprint && Blueprint->GeneratedClass)
		{
			Properties->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass->GetPathName());
			Properties->SetStringField(TEXT("parent_class"), Blueprint->GeneratedClass->GetSuperClass()
				? Blueprint->GeneratedClass->GetSuperClass()->GetPathName()
				: FString(TEXT("None")));
		}
	}

	/** 把 JSON 属性对象包装成一条稳定 node 快照。
	 *
	 * 这里故意只负责“在内存里拼出节点”，不直接写数据库。
	 * 这样生产表、artifact 和 shadow 表三条路径就能共用同一份节点内容。
	 */
	static bool BuildNode(
		const FAssetData& AssetData,
		const UBlueprint* Blueprint,
		const FString& NodeType,
		const TSharedPtr<FJsonObject>& Properties,
		FIndexedNode& OutNode)
	{
		if (!Properties.IsValid())
		{
			return false;
		}

		OutNode = FIndexedNode();
		OutNode.NodeName = ResolveNodeName(AssetData, Blueprint);
		OutNode.NodeType = NodeType;
		OutNode.NodeClass = (Blueprint && Blueprint->GeneratedClass)
			? Blueprint->GeneratedClass->GetName()
			: NodeType;
		return SerializeJsonObject(Properties, OutNode.Properties);
	}

	/** 把单节点快照包装成 artifact。 */
	static void BuildNodeArtifact(
		const FIndexedNode& Node,
		FMonolithArtifact& OutArtifact,
		const FName IndexerId,
		const uint32 IndexerVersion,
		const uint8 ArtifactSchemaVersion,
		const EMonolithExecutionMode ExecutionMode,
		const FString& PackageName)
	{
		MonolithSimpleArtifactSerialization::FNodePayload Payload;
		Payload.Node = Node;

		OutArtifact = FMonolithArtifact();
		OutArtifact.ArtifactSchemaVersion = ArtifactSchemaVersion;
		OutArtifact.IndexerId = IndexerId;
		OutArtifact.IndexerVersion = IndexerVersion;
		OutArtifact.ExecutionMode = ExecutionMode;
		OutArtifact.PackageName = PackageName;
		MonolithSimpleArtifactSerialization::SerializeNodePayload(Payload, OutArtifact.Payload);
	}
}

bool FGASIndexer::MatchesAsset(const FAssetData& AssetData, const UObject* LoadedAsset) const
{
	if (!IMonolithIndexer::MatchesAsset(AssetData, LoadedAsset))
	{
		return false;
	}

	return GASIndexerInternal::ResolveBlueprintKind(AssetData, Cast<UBlueprint>(LoadedAsset))
		!= GASIndexerInternal::EGASBlueprintKind::None;
}

bool FGASIndexer::BuildArtifact(
	const FAssetData& AssetData,
	UObject* LoadedAsset,
	IAssetRegistry& AssetRegistry,
	FMonolithArtifact& OutArtifact)
{
	(void)AssetRegistry;

	FIndexedNode Node;
	if (!BuildNodeForAsset(AssetData, Cast<UBlueprint>(LoadedAsset), Node))
	{
		return false;
	}

	GASIndexerInternal::BuildNodeArtifact(
		Node,
		OutArtifact,
		GetIndexerId(),
		GetIndexerVersion(),
		GetArtifactSchemaVersion(),
		GetExecutionMode(),
		AssetData.PackageName.ToString());
	return OutArtifact.Payload.Num() > 0;
}

bool FGASIndexer::MaterializeArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB, int64 AssetId)
{
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodePayload(Payload, DB, AssetId);
}

bool FGASIndexer::MaterializeArtifactToShadow(
	const FMonolithArtifact& Artifact,
	FMonolithIndexDatabase& DB,
	int64 AssetId,
	const FString& CohortName)
{
	MonolithSimpleArtifactSerialization::FNodePayload Payload;
	if (!MonolithSimpleArtifactSerialization::DeserializeNodePayload(Artifact.Payload, Payload))
	{
		return false;
	}

	return MonolithSimpleArtifactSerialization::MaterializeNodePayloadToShadow(Payload, DB, AssetId, CohortName);
}

bool FGASIndexer::BuildNodeForAsset(const FAssetData& AssetData, UBlueprint* Blueprint, FIndexedNode& OutNode) const
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return false;
	}

	switch (GASIndexerInternal::ResolveBlueprintKind(AssetData, Blueprint))
	{
	case GASIndexerInternal::EGASBlueprintKind::GameplayAbility:
		return BuildGameplayAbilityNode(AssetData, Blueprint, OutNode);
	case GASIndexerInternal::EGASBlueprintKind::GameplayEffect:
		return BuildGameplayEffectNode(AssetData, Blueprint, OutNode);
	case GASIndexerInternal::EGASBlueprintKind::AttributeSet:
		return BuildAttributeSetNode(AssetData, Blueprint, OutNode);
	case GASIndexerInternal::EGASBlueprintKind::GameplayCueStatic:
	case GASIndexerInternal::EGASBlueprintKind::GameplayCueActor:
		return BuildGameplayCueNode(AssetData, Blueprint, OutNode);
	case GASIndexerInternal::EGASBlueprintKind::None:
	default:
		return false;
	}
}

bool FGASIndexer::BuildGameplayAbilityNode(
	const FAssetData& AssetData,
	UBlueprint* Blueprint,
	FIndexedNode& OutNode) const
{
	UGameplayAbility* Ability = Blueprint && Blueprint->GeneratedClass
		? Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject(false))
		: nullptr;
	if (!Ability)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	GASIndexerInternal::PopulateCommonNodeFields(AssetData, Blueprint, Properties);
	Properties->SetArrayField(TEXT("ability_tags"), GASIndexerInternal::ExtractTagContainer(Ability, TEXT("AbilityTags")));
	Properties->SetArrayField(TEXT("cancel_abilities_with_tag"), GASIndexerInternal::ExtractTagContainer(Ability, TEXT("CancelAbilitiesWithTag")));
	Properties->SetArrayField(TEXT("block_abilities_with_tag"), GASIndexerInternal::ExtractTagContainer(Ability, TEXT("BlockAbilitiesWithTag")));
	Properties->SetArrayField(TEXT("activation_required_tags"), GASIndexerInternal::ExtractTagContainer(Ability, TEXT("ActivationRequiredTags")));
	Properties->SetArrayField(TEXT("activation_blocked_tags"), GASIndexerInternal::ExtractTagContainer(Ability, TEXT("ActivationBlockedTags")));

	FString EnumValue;
	if (GASIndexerInternal::TryReadEnumPropertyAsString(Ability, TEXT("InstancingPolicy"), EnumValue))
	{
		Properties->SetStringField(TEXT("instancing_policy"), EnumValue);
	}
	if (GASIndexerInternal::TryReadEnumPropertyAsString(Ability, TEXT("NetExecutionPolicy"), EnumValue))
	{
		Properties->SetStringField(TEXT("net_execution_policy"), EnumValue);
	}
	if (GASIndexerInternal::TryReadEnumPropertyAsString(Ability, TEXT("NetSecurityPolicy"), EnumValue))
	{
		Properties->SetStringField(TEXT("net_security_policy"), EnumValue);
	}

	FString ClassPath;
	if (GASIndexerInternal::TryReadClassPropertyPath(Ability, TEXT("CostGameplayEffectClass"), ClassPath))
	{
		Properties->SetStringField(TEXT("cost_effect_class"), ClassPath);
	}
	if (GASIndexerInternal::TryReadClassPropertyPath(Ability, TEXT("CooldownGameplayEffectClass"), ClassPath))
	{
		Properties->SetStringField(TEXT("cooldown_effect_class"), ClassPath);
	}

	return GASIndexerInternal::BuildNode(AssetData, Blueprint, TEXT("GameplayAbility"), Properties, OutNode);
}

bool FGASIndexer::BuildGameplayEffectNode(
	const FAssetData& AssetData,
	UBlueprint* Blueprint,
	FIndexedNode& OutNode) const
{
	UGameplayEffect* Effect = Blueprint && Blueprint->GeneratedClass
		? Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject(false))
		: nullptr;
	if (!Effect)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	GASIndexerInternal::PopulateCommonNodeFields(AssetData, Blueprint, Properties);

	FString EnumValue;
	if (GASIndexerInternal::TryReadEnumPropertyAsString(Effect, TEXT("DurationPolicy"), EnumValue))
	{
		Properties->SetStringField(TEXT("duration_policy"), EnumValue);
	}
	if (GASIndexerInternal::TryReadEnumPropertyAsString(Effect, TEXT("StackingType"), EnumValue))
	{
		Properties->SetStringField(TEXT("stacking_type"), EnumValue);
	}

	double NumericValue = 0.0;
	if (GASIndexerInternal::TryReadNumericPropertyAsDouble(Effect, TEXT("Period"), NumericValue))
	{
		Properties->SetNumberField(TEXT("period"), NumericValue);
	}

	// Modifiers 是 GameplayEffect 最关键的结构化配置之一。
	// 这里把每个 modifier 折叠成一段小 JSON，查询侧可以直接读到“改哪个属性、怎么改”。
	if (const FArrayProperty* ModifiersProperty = CastField<FArrayProperty>(Effect->GetClass()->FindPropertyByName(TEXT("Modifiers"))))
	{
		FScriptArrayHelper ModifierArray(ModifiersProperty, ModifiersProperty->ContainerPtrToValuePtr<void>(Effect));
		TArray<TSharedPtr<FJsonValue>> Modifiers;
		for (int32 Index = 0; Index < ModifierArray.Num(); ++Index)
		{
			const FStructProperty* InnerStructProperty = CastField<FStructProperty>(ModifiersProperty->Inner);
			if (!InnerStructProperty)
			{
				continue;
			}

			void* ElementPtr = ModifierArray.GetRawPtr(Index);
			TSharedPtr<FJsonObject> ModifierObject = MakeShared<FJsonObject>();

			if (const FStructProperty* AttributeProperty = CastField<FStructProperty>(InnerStructProperty->Struct->FindPropertyByName(TEXT("Attribute"))))
			{
				FString AttributeExportText;
				AttributeProperty->ExportTextItem_Direct(
					AttributeExportText,
					AttributeProperty->ContainerPtrToValuePtr<void>(ElementPtr),
					nullptr,
					nullptr,
					PPF_None);
				if (!AttributeExportText.IsEmpty())
				{
					ModifierObject->SetStringField(TEXT("attribute"), AttributeExportText);
				}
			}

			FString ModifierOp;
			if (const FProperty* ModifierOpProperty = InnerStructProperty->Struct->FindPropertyByName(TEXT("ModifierOp")))
			{
				if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(ModifierOpProperty))
				{
					const FNumericProperty* UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
					const void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(ElementPtr);
					if (UnderlyingProperty && EnumProperty->GetEnum() && ValuePtr)
					{
						ModifierOp = EnumProperty->GetEnum()->GetNameStringByValue(UnderlyingProperty->GetSignedIntPropertyValue(ValuePtr));
					}
				}
				else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(ModifierOpProperty))
				{
					if (ByteProperty->Enum)
					{
						ModifierOp = ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetPropertyValue_InContainer(ElementPtr));
					}
				}
			}
			if (!ModifierOp.IsEmpty())
			{
				ModifierObject->SetStringField(TEXT("modifier_op"), ModifierOp);
			}

			if (const FStructProperty* MagnitudeProperty = CastField<FStructProperty>(InnerStructProperty->Struct->FindPropertyByName(TEXT("ModifierMagnitude"))))
			{
				const void* MagnitudePtr = MagnitudeProperty->ContainerPtrToValuePtr<void>(ElementPtr);
				if (const FProperty* MagnitudeTypeProperty = MagnitudeProperty->Struct->FindPropertyByName(TEXT("MagnitudeCalculationType")))
				{
					FString MagnitudeType;
					if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(MagnitudeTypeProperty))
					{
						const FNumericProperty* UnderlyingProperty = EnumProperty->GetUnderlyingProperty();
						const void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(MagnitudePtr);
						if (UnderlyingProperty && EnumProperty->GetEnum() && ValuePtr)
						{
							MagnitudeType = EnumProperty->GetEnum()->GetNameStringByValue(UnderlyingProperty->GetSignedIntPropertyValue(ValuePtr));
						}
					}
					else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(MagnitudeTypeProperty))
					{
						if (ByteProperty->Enum)
						{
							const uint8* ByteValue = ByteProperty->ContainerPtrToValuePtr<uint8>(MagnitudePtr);
							if (ByteValue)
							{
								MagnitudeType = ByteProperty->Enum->GetNameStringByValue(*ByteValue);
							}
						}
					}
					if (!MagnitudeType.IsEmpty())
					{
						ModifierObject->SetStringField(TEXT("magnitude_type"), MagnitudeType);
					}
				}
			}

			Modifiers.Add(MakeShared<FJsonValueObject>(ModifierObject));
		}

		if (Modifiers.Num() > 0)
		{
			Properties->SetArrayField(TEXT("modifiers"), Modifiers);
		}
	}

	if (const FArrayProperty* GameplayCuesProperty = CastField<FArrayProperty>(Effect->GetClass()->FindPropertyByName(TEXT("GameplayCues"))))
	{
		FScriptArrayHelper CueArray(GameplayCuesProperty, GameplayCuesProperty->ContainerPtrToValuePtr<void>(Effect));
		TArray<TSharedPtr<FJsonValue>> CueTags;
		for (int32 Index = 0; Index < CueArray.Num(); ++Index)
		{
			const FStructProperty* CueStructProperty = CastField<FStructProperty>(GameplayCuesProperty->Inner);
			if (!CueStructProperty)
			{
				continue;
			}

			void* ElementPtr = CueArray.GetRawPtr(Index);
			if (const FStructProperty* CueTagsProperty = CastField<FStructProperty>(CueStructProperty->Struct->FindPropertyByName(TEXT("GameplayCueTags"))))
			{
				const void* CueTagsPtr = CueTagsProperty->ContainerPtrToValuePtr<void>(ElementPtr);
				const FGameplayTagContainer* CueTagContainer = static_cast<const FGameplayTagContainer*>(CueTagsPtr);
				if (!CueTagContainer)
				{
					continue;
				}

				for (const FGameplayTag& CueTag : *CueTagContainer)
				{
					CueTags.Add(MakeShared<FJsonValueString>(CueTag.ToString()));
				}
			}
		}

		if (CueTags.Num() > 0)
		{
			Properties->SetArrayField(TEXT("gameplay_cues"), CueTags);
		}
	}

	TArray<UObject*> DefaultSubobjects;
	Effect->GetDefaultSubobjects(DefaultSubobjects);
	if (DefaultSubobjects.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ComponentClasses;
		for (UObject* Subobject : DefaultSubobjects)
		{
			if (Subobject)
			{
				ComponentClasses.Add(MakeShared<FJsonValueString>(Subobject->GetClass()->GetName()));
			}
		}
		if (ComponentClasses.Num() > 0)
		{
			Properties->SetArrayField(TEXT("components"), ComponentClasses);
		}
	}

	return GASIndexerInternal::BuildNode(AssetData, Blueprint, TEXT("GameplayEffect"), Properties, OutNode);
}

bool FGASIndexer::BuildAttributeSetNode(
	const FAssetData& AssetData,
	UBlueprint* Blueprint,
	FIndexedNode& OutNode) const
{
	UAttributeSet* AttributeSet = Blueprint && Blueprint->GeneratedClass
		? Cast<UAttributeSet>(Blueprint->GeneratedClass->GetDefaultObject(false))
		: nullptr;
	if (!AttributeSet || !Blueprint->GeneratedClass)
	{
		return false;
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	GASIndexerInternal::PopulateCommonNodeFields(AssetData, Blueprint, Properties);
	Properties->SetBoolField(TEXT("is_native"), false);

	TArray<TSharedPtr<FJsonValue>> Attributes;
	for (TFieldIterator<FProperty> PropertyIt(Blueprint->GeneratedClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); PropertyIt; ++PropertyIt)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(*PropertyIt);
		if (!StructProperty || !StructProperty->Struct || StructProperty->Struct->GetName() != TEXT("GameplayAttributeData"))
		{
			continue;
		}

		TSharedPtr<FJsonObject> AttributeObject = MakeShared<FJsonObject>();
		AttributeObject->SetStringField(TEXT("name"), PropertyIt->GetName());
		AttributeObject->SetStringField(TEXT("owning_class"), Blueprint->GeneratedClass->GetName());

		const void* ValuePtr = StructProperty->ContainerPtrToValuePtr<void>(AttributeSet);
		if (const FProperty* BaseValueProperty = StructProperty->Struct->FindPropertyByName(TEXT("BaseValue")))
		{
			if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(BaseValueProperty))
			{
				AttributeObject->SetNumberField(TEXT("base_value"), FloatProperty->GetPropertyValue_InContainer(ValuePtr));
			}
			else if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(BaseValueProperty))
			{
				AttributeObject->SetNumberField(TEXT("base_value"), DoubleProperty->GetPropertyValue_InContainer(ValuePtr));
			}
		}

		Attributes.Add(MakeShared<FJsonValueObject>(AttributeObject));
	}

	if (Attributes.Num() > 0)
	{
		Properties->SetArrayField(TEXT("attributes"), Attributes);
	}

	return GASIndexerInternal::BuildNode(AssetData, Blueprint, TEXT("AttributeSet"), Properties, OutNode);
}

bool FGASIndexer::BuildGameplayCueNode(
	const FAssetData& AssetData,
	UBlueprint* Blueprint,
	FIndexedNode& OutNode) const
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return false;
	}

	UObject* DefaultObject = Blueprint->GeneratedClass->GetDefaultObject(false);
	if (!DefaultObject)
	{
		return false;
	}

	const bool bIsActorCue = Blueprint->GeneratedClass->IsChildOf(AGameplayCueNotify_Actor::StaticClass());
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	GASIndexerInternal::PopulateCommonNodeFields(AssetData, Blueprint, Properties);
	Properties->SetStringField(TEXT("notify_type"), bIsActorCue ? TEXT("Actor") : TEXT("Static"));

	if (const FStructProperty* CueTagProperty = CastField<FStructProperty>(DefaultObject->GetClass()->FindPropertyByName(TEXT("GameplayCueTag"))))
	{
		const void* TagPtr = CueTagProperty->ContainerPtrToValuePtr<void>(DefaultObject);
		const FGameplayTag* CueTag = static_cast<const FGameplayTag*>(TagPtr);
		if (CueTag && CueTag->IsValid())
		{
			Properties->SetStringField(TEXT("gameplay_cue_tag"), CueTag->ToString());
		}
	}

	return GASIndexerInternal::BuildNode(AssetData, Blueprint, TEXT("GameplayCue"), Properties, OutNode);
}
