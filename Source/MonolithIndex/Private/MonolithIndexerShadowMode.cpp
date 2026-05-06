#include "MonolithIndexerShadowMode.h"

#include "Containers/StringConv.h"
#include "Hash/xxhash.h"

/*
 * 这份实现文件专门处理 shadow mode 里最“数学化”的部分：
 * - 怎么把一行数据稳定地编码成字节；
 * - 怎么把这串字节算成 row_hash；
 * - 怎么根据聚合结果判断是不是需要 Level 2 下钻或直接回滚。
 *
 * 你可以把它想成“比较新旧索引结果时的统一尺子”。
 * 如果没有这把尺子，不同 cohort 会各自发明一套比较方法，最后很难排查问题。
 */

namespace MonolithIndexerShadowModeInternal
{
	/** 统一按小端写入 4 字节整数，保证不同机器上字节顺序一致。 */
	static void WriteUInt32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	/** 统一按小端写入 8 字节整数。 */
	static void WriteUInt64(TArray<uint8>& Bytes, const uint64 Value)
	{
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			Bytes.Add(static_cast<uint8>((Value >> (ByteIndex * 8)) & 0xff));
		}
	}

	/** 先写长度，再写 UTF-8 字节，避免中文和特殊字符被截断。 */
	static void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/*
	 * SQL 标识符里不能随便塞空格、逗号、破折号这些字符。
	 * 所以这里会把 cohort/base table 名清洗成只含字母数字和下划线的安全名字。
	 */
	static FString SanitizeSqlIdentifier(const FString& Value)
	{
		FString Result;
		Result.Reserve(Value.Len());

		bool bLastUnderscore = false;
		for (const TCHAR Character : Value)
		{
			if (FChar::IsAlnum(Character))
			{
				Result.AppendChar(Character);
				bLastUnderscore = false;
			}
			else if (!bLastUnderscore)
			{
				Result.AppendChar(TEXT('_'));
				bLastUnderscore = true;
			}
		}

		Result.TrimStartAndEndInline();
		while (Result.StartsWith(TEXT("_")))
		{
			Result.RightChopInline(1, EAllowShrinking::No);
		}
		while (Result.EndsWith(TEXT("_")))
		{
			Result.LeftChopInline(1, EAllowShrinking::No);
		}

		return Result.IsEmpty() ? FString(TEXT("default")) : Result;
	}
}

bool ParseShadowModeCohort(const FString& ConfigValue, FName& OutCohortName)
{
	// cohort 配置只允许一个名字，不支持在这里写列表。
	FString Trimmed = ConfigValue;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.IsEmpty())
	{
		return false;
	}

	if (Trimmed.Contains(TEXT(",")) || Trimmed.Contains(TEXT(";")) || Trimmed.Contains(TEXT("|")))
	{
		return false;
	}

	OutCohortName = FName(*Trimmed);
	return !OutCohortName.IsNone();
}

FString MakeShadowTableName(const FString& CohortName, const FString& BaseTableName)
{
	// 统一的 shadow 表命名规则，便于后面做 retention 和 promote/rollback。
	return FString::Printf(
		TEXT("shadow_%s_%s"),
		*MonolithIndexerShadowModeInternal::SanitizeSqlIdentifier(CohortName),
		*MonolithIndexerShadowModeInternal::SanitizeSqlIdentifier(BaseTableName));
}

FDateTime GetShadowRetentionDeadlineUtc(const FDateTime& NowUtc, const bool bRollbackRetained)
{
	// rollback 候选保留更久，因为它们通常更值得排查。
	return NowUtc + (bRollbackRetained ? FTimespan::FromDays(7.0) : FTimespan::FromHours(24.0));
}

FMonolithShadowDiffDecision EvaluateShadowDiff(
	const FMonolithShadowAggregate& Production,
	const FMonolithShadowAggregate& Shadow,
	const double RollbackThresholdRatio)
{
	FMonolithShadowDiffDecision Decision;
	// 分母至少为 1，避免两边都是空时除零。
	const uint64 Denominator = FMath::Max<uint64>(1, FMath::Max(Production.RowCount, Shadow.RowCount));
	// 至少不同多少行，这里取“行数差”和“哈希和差异”能证明的最小值。
	const uint64 MinimumDifferentRows = FMath::Max<uint64>(
		Production.RowCount > Shadow.RowCount ? Production.RowCount - Shadow.RowCount : Shadow.RowCount - Production.RowCount,
		Production.RowHashSum == Shadow.RowHashSum ? 0 : 1);

	Decision.AggregateDifferenceRatio = static_cast<double>(MinimumDifferentRows) / static_cast<double>(Denominator);
	Decision.bRequiresLevel2 = MinimumDifferentRows > 0;
	Decision.bShouldRollback = Decision.AggregateDifferenceRatio > RollbackThresholdRatio;
	return Decision;
}

bool IsDeterministicLevel2Sample(const FString& PrimaryKey)
{
	// 用主键哈希做固定 1% 抽样，保证同一行每次都会被抽中或不会被抽中。
	FTCHARToUTF8 Utf8(*PrimaryKey);
	const FXxHash64 Hash = FXxHash64::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
	return (Hash.Hash % 100ull) == 0ull;
}

FMonolithShadowLevel2DiffResult EvaluateShadowLevel2Diff(
	const TArray<FMonolithShadowLevel2Row>& ProductionRows,
	const TArray<FMonolithShadowLevel2Row>& ShadowRows)
{
	FMonolithShadowLevel2DiffResult Result;

	/*
	 * Level 2 的职责只有一件事：
	 * 对“已经被确定性采样命中”的主键做逐行对齐。
	 *
	 * 这里故意不用 row_hash 当 map key，
	 * 而是要求调用方先把“业务主键 + 重复序号”拼成唯一 PrimaryKey。
	 * 这样我们比较的是“同一逻辑行有没有变”，而不是“有没有一行长得像”。
	 */
	TMap<FString, const FMonolithShadowLevel2Row*> ShadowRowsByKey;
	ShadowRowsByKey.Reserve(ShadowRows.Num());
	for (const FMonolithShadowLevel2Row& ShadowRow : ShadowRows)
	{
		if (ShadowRow.PrimaryKey.IsEmpty())
		{
			continue;
		}

		if (IsDeterministicLevel2Sample(ShadowRow.PrimaryKey))
		{
			++Result.SampledShadowRows;
			ShadowRowsByKey.Add(ShadowRow.PrimaryKey, &ShadowRow);
		}
	}

	TSet<FString> MatchedShadowKeys;
	for (const FMonolithShadowLevel2Row& ProductionRow : ProductionRows)
	{
		if (ProductionRow.PrimaryKey.IsEmpty() || !IsDeterministicLevel2Sample(ProductionRow.PrimaryKey))
		{
			continue;
		}

		++Result.SampledProductionRows;

		const FMonolithShadowLevel2Row* const* ShadowRow = ShadowRowsByKey.Find(ProductionRow.PrimaryKey);
		if (!ShadowRow)
		{
			++Result.ProductionOnlyRows;
			continue;
		}

		MatchedShadowKeys.Add(ProductionRow.PrimaryKey);
		++Result.ComparedRows;
		if ((*ShadowRow)->RowHash != ProductionRow.RowHash)
		{
			++Result.MismatchedRows;
		}
	}

	for (const TPair<FString, const FMonolithShadowLevel2Row*>& Pair : ShadowRowsByKey)
	{
		if (!MatchedShadowKeys.Contains(Pair.Key))
		{
			++Result.ShadowOnlyRows;
		}
	}

	return Result;
}

uint64 ComputeNodeRowHash(const FIndexedNode& Node)
{
	// 这里只编码“业务可见字段”，不编码 asset_id / revision_id 这种会随环境变化的值。
	TArray<uint8> Bytes;
	Bytes.Reserve(128 + Node.NodeType.Len() + Node.NodeName.Len() + Node.NodeClass.Len() + Node.Properties.Len());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, Node.NodeType);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Node.NodeName);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Node.NodeClass);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Node.Properties);
	MonolithIndexerShadowModeInternal::WriteUInt64(Bytes, static_cast<uint64>(static_cast<int64>(Node.PosX)));
	MonolithIndexerShadowModeInternal::WriteUInt64(Bytes, static_cast<uint64>(static_cast<int64>(Node.PosY)));

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeVariableRowHash(const FIndexedVariable& Variable)
{
	// 变量行哈希关注的是名字、类型、默认值和暴露/复制语义。
	TArray<uint8> Bytes;
	Bytes.Reserve(96 + Variable.VarName.Len() + Variable.VarType.Len() + Variable.Category.Len() + Variable.DefaultValue.Len());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, Variable.VarName);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Variable.VarType);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Variable.Category);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Variable.DefaultValue);
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, Variable.bIsExposed ? 1u : 0u);
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, Variable.bIsReplicated ? 1u : 0u);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeActorRowHash(const FIndexedActor& Actor)
{
	// Level 的 actor diff 不能只看名字，还要看 class / label / transform / components。
	TArray<uint8> Bytes;
	Bytes.Reserve(128 + Actor.ActorName.Len() + Actor.ActorClass.Len() + Actor.ActorLabel.Len() + Actor.Transform.Len() + Actor.Components.Len());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, Actor.ActorName);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Actor.ActorClass);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Actor.ActorLabel);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Actor.Transform);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Actor.Components);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeDataTableRowHash(const FIndexedDataTableRow& Row)
{
	// DataTable 行最核心的就是“行名 + 行内容”。
	TArray<uint8> Bytes;
	Bytes.Reserve(64 + Row.RowName.Len() + Row.RowData.Len());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, Row.RowName);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Row.RowData);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeDependencyRowHash(const FString& TargetPackagePath, const FString& DependencyType)
{
	// dependency 的“业务含义”就是：
	// “我依赖哪个包，以及这是哪一种依赖”。
	// 所以这里故意不用本地 SQLite 的 asset_id，只用稳定的包路径文本。
	TArray<uint8> Bytes;
	Bytes.Reserve(48 + TargetPackagePath.Len() + DependencyType.Len());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, TargetPackagePath);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, DependencyType);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeTagReferenceRowHash(const FString& TagName, const FString& Context)
{
	// tag reference 也只关心“用了哪个 tag、在什么语境里用到它”。
	// tag_id 是本地数据库内部编号，不能拿来当稳定语义。
	TArray<uint8> Bytes;
	Bytes.Reserve(48 + TagName.Len() + Context.Len());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, TagName);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Context);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeParameterRowHash(const FIndexedParameter& Parameter)
{
	// 材质参数等参数类行需要把来源也编进去，不然不同来源可能撞成同一行。
	TArray<uint8> Bytes;
	Bytes.Reserve(96 + Parameter.ParamName.Len() + Parameter.ParamType.Len() + Parameter.ParamGroup.Len() + Parameter.DefaultValue.Len() + Parameter.Source.Len());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, Parameter.ParamName);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Parameter.ParamType);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Parameter.ParamGroup);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Parameter.DefaultValue);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Parameter.Source);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeMeshCatalogRowHash(const FIndexedMeshCatalogEntry& Entry)
{
	// mesh catalog 的意义就是“同一个网格的尺寸/类别/复杂度摘要”。
	// 所以这里同样只编码业务字段，不编码 asset_id / revision_id 这类环境字段。
	TArray<uint8> Bytes;
	Bytes.Reserve(192 + Entry.AssetPath.Len() + Entry.SizeClass.Len() + Entry.Category.Len());
	auto WriteDoubleBits = [&Bytes](const double Value)
	{
		uint64 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(double));
		MonolithIndexerShadowModeInternal::WriteUInt64(Bytes, Bits);
	};

	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.AssetPath);
	WriteDoubleBits(Entry.BoundsX);
	WriteDoubleBits(Entry.BoundsY);
	WriteDoubleBits(Entry.BoundsZ);
	WriteDoubleBits(Entry.BoundsMin);
	WriteDoubleBits(Entry.BoundsMid);
	WriteDoubleBits(Entry.BoundsMax);
	WriteDoubleBits(Entry.Volume);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.SizeClass);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.Category);
	MonolithIndexerShadowModeInternal::WriteUInt64(Bytes, static_cast<uint64>(static_cast<int64>(Entry.TriCount)));
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, Entry.bHasCollision ? 1u : 0u);
	MonolithIndexerShadowModeInternal::WriteUInt64(Bytes, static_cast<uint64>(static_cast<int64>(Entry.LodCount)));
	WriteDoubleBits(Entry.PivotOffsetZ);
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, Entry.bDegenerate ? 1u : 0u);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeAssetVisualRowHash(const FIndexedAssetVisualEntry& Entry)
{
	// AssetVisual 行哈希必须把 provider triple + embedding 字节都纳入，
	// 这样 provider 升级或视觉特征任何变化都能立刻在 shadow Level 1 diff 里被发现。
	// Multi-phase 后行身份 = (asset, phase_id, phase_t, phase_label) 全部参与，
	// 否则同 asset 不同 phase 的两行哈希会重复（XOR 后互相抵消）。
	TArray<uint8> Bytes;
	Bytes.Reserve(112 + Entry.AssetPath.Len() + Entry.ShardId.Len() + Entry.ProviderId.Len() + Entry.PreviewViewPath.Len() + Entry.PhaseLabel.Len() + Entry.EmbeddingBytes.Num());

	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.AssetPath);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.ShardId);
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, static_cast<uint32>(Entry.ShardPrefixDepth));
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.ProviderId);
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, Entry.ProviderVersion);
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, Entry.RenderRecipeVersion);
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, static_cast<uint32>(Entry.EmbeddingDim));
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, static_cast<uint32>(Entry.EmbeddingDtype));
	// embedding 字节流直接 append；不会与 dtype/dim 字段产生歧义解析。
	if (Entry.EmbeddingBytes.Num() > 0)
	{
		Bytes.Append(Entry.EmbeddingBytes.GetData(), Entry.EmbeddingBytes.Num());
	}
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.PreviewViewPath);

	// Phase 字段：PhaseT 用 IEEE-754 bit pattern 写入避免浮点比较歧义。
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, static_cast<uint32>(Entry.PhaseId));
	uint32 PhaseTBits = 0;
	FMemory::Memcpy(&PhaseTBits, &Entry.PhaseT, sizeof(uint32));
	MonolithIndexerShadowModeInternal::WriteUInt32(Bytes, PhaseTBits);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, Entry.PhaseLabel);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}

uint64 ComputeConnectionRowHash(
	const uint64 SourceNodeRowHash,
	const FString& SourcePin,
	const uint64 TargetNodeRowHash,
	const FString& TargetPin,
	const FString& PinType)
{
	// connection 不直接存整行节点，只存两端节点的稳定哈希 + pin 信息。
	TArray<uint8> Bytes;
	Bytes.Reserve(80 + SourcePin.Len() + TargetPin.Len() + PinType.Len());

	MonolithIndexerShadowModeInternal::WriteUInt64(Bytes, SourceNodeRowHash);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, SourcePin);
	MonolithIndexerShadowModeInternal::WriteUInt64(Bytes, TargetNodeRowHash);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, TargetPin);
	MonolithIndexerShadowModeInternal::WriteString(Bytes, PinType);

	return FXxHash64::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num())).Hash;
}
