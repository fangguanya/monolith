#pragma once

#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexerShadowMode.h"

namespace MonolithSimpleArtifactSerialization
{
	/** 只包含一个 node 的轻量 artifact 载荷。 */
	struct FNodePayload
	{
		/** 资产展开后的节点快照。 */
		FIndexedNode Node;
	};

	/** 包含多个 node 的轻量载荷。 */
	struct FNodesPayload
	{
		/** 同一个资产展开出来的全部节点快照。 */
		TArray<FIndexedNode> Nodes;
	};

	/** 包含一个 node 和若干变量的载荷。 */
	struct FNodeVariablePayload
	{
		/** 主节点。 */
		FIndexedNode Node;
		/** 附带的变量列表。 */
		TArray<FIndexedVariable> Variables;
	};

	/** DataTable 的轻量载荷。 */
	struct FDataTablePayload
	{
		/** 全部行快照。 */
		TArray<FIndexedDataTableRow> Rows;
	};

	/** 单条 dependency artifact 记录。 */
	struct FDependencyPayloadEntry
	{
		/** 目标资产的包路径。 */
		FString TargetPackagePath;
		/** 依赖类型，例如 Hard / Soft。 */
		FString DependencyType;
	};

	/** dependency 的轻量载荷。 */
	struct FDependencyPayload
	{
		/** 全部依赖边快照。 */
		TArray<FDependencyPayloadEntry> Dependencies;
	};

	/** 单条 GameplayTag 引用 artifact 记录。 */
	struct FTagReferencePayloadEntry
	{
		/** 被引用的 tag 名。 */
		FString TagName;
		/** 这个 tag 是在哪个语境里出现的。 */
		FString Context;
	};

	/** GameplayTag 引用的轻量载荷。 */
	struct FTagReferencePayload
	{
		/** 全部 tag 引用快照。 */
		TArray<FTagReferencePayloadEntry> References;
	};

	/** 单条 Config artifact 记录。 */
	struct FConfigPayloadEntry
	{
		/** 配置文件逻辑路径。 */
		FString FilePath;
		/** INI section。 */
		FString Section;
		/** 键名。 */
		FString Key;
		/** 值文本。 */
		FString Value;
	};

	/** Config 全局 artifact 载荷。 */
	struct FConfigPayload
	{
		/** 整份 Config 扫描快照。 */
		TArray<FConfigPayloadEntry> Entries;
	};

	/** 单条 C++ 符号 artifact 记录。 */
	struct FCppSymbolPayloadEntry
	{
		/** 源文件逻辑路径。 */
		FString FilePath;
		/** 符号名。 */
		FString SymbolName;
		/** 符号类型。 */
		FString SymbolType;
		/** 声明文本。 */
		FString Signature;
		/** 行号。 */
		int32 LineNumber = 0;
		/** 父级符号。 */
		FString ParentSymbol;
	};

	/** C++ 符号全局 artifact 载荷。 */
	struct FCppSymbolPayload
	{
		/** 整份可查询符号快照。 */
		TArray<FCppSymbolPayloadEntry> Symbols;
	};

	/** 单条 GameplayTag 定义 artifact 记录。 */
	struct FGameplayTagDefinitionPayloadEntry
	{
		/** 完整 tag 名。 */
		FString TagName;
		/** 父 tag 名。 */
		FString ParentTag;
	};

	/** GameplayTag 定义树的全局 artifact 载荷。 */
	struct FGameplayTagDefinitionPayload
	{
		/** 全部定义快照。 */
		TArray<FGameplayTagDefinitionPayloadEntry> Definitions;
	};

	/** 一条“本地节点索引”意义下的连线。
	 *
	 * 这里不直接保存数据库 node id，
	 * 因为 artifact 在构建时数据库行还不存在。
	 *
	 * 所以它只记“第几个节点连向第几个节点”，
	 * 等真正 materialize 时，再换算成数据库里的真实行 id 或 shadow row hash。
	 */
	struct FGraphPayloadConnection
	{
		/** 起点节点在 Nodes 数组里的下标。 */
		int32 SourceNodeIndex = INDEX_NONE;
		/** 起点 pin 名。 */
		FString SourcePin;
		/** 终点节点在 Nodes 数组里的下标。 */
		int32 TargetNodeIndex = INDEX_NONE;
		/** 终点 pin 名。 */
		FString TargetPin;
		/** 这条边的语义类型，例如 Exec、ST_Transition。 */
		FString PinType;
	};

	/** 适合图结构资产的通用 artifact 载荷。
	 *
	 * 它把三种最常见的数据统一放在一起：
	 * - Nodes：图里的节点
	 * - Variables：附着在这个资产上的变量/键
	 * - Connections：节点之间的内部连线
	 *
	 * 这样 BehaviorTree / EQS / StateTree 这类资产就不需要各自维护一份几乎相同的回放逻辑。
	 */
	struct FGraphPayload
	{
		/** 节点快照。 */
		TArray<FIndexedNode> Nodes;
		/** 变量或键定义快照。 */
		TArray<FIndexedVariable> Variables;
		/** 节点之间的内部连线。 */
		TArray<FGraphPayloadConnection> Connections;
	};

	/** 往字节数组末尾写 1 个字节。 */
	inline void WriteUInt8(TArray<uint8>& Bytes, const uint8 Value)
	{
		Bytes.Add(Value);
	}

	/** 以固定 4 字节小端格式写一个 uint32。 */
	inline void WriteUInt32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	/** 先写长度，再把 UTF-8 字符串内容写进去。 */
	inline void WriteString(TArray<uint8>& Bytes, const FString& Value)
	{
		FTCHARToUTF8 Convert(*Value);
		WriteUInt32(Bytes, static_cast<uint32>(Convert.Length()));
		if (Convert.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Convert.Get()), Convert.Length());
		}
	}

	/** 从当前位置读一个字节，并推进偏移量。 */
	inline bool ReadUInt8(const TArray<uint8>& Bytes, int32& Offset, uint8& OutValue)
	{
		if (Offset + 1 > Bytes.Num())
		{
			return false;
		}

		OutValue = Bytes[Offset];
		++Offset;
		return true;
	}

	/** 从当前位置读一个 uint32，并推进偏移量。 */
	inline bool ReadUInt32(const TArray<uint8>& Bytes, int32& Offset, uint32& OutValue)
	{
		if (Offset + 4 > Bytes.Num())
		{
			return false;
		}

		OutValue =
			static_cast<uint32>(Bytes[Offset]) |
			(static_cast<uint32>(Bytes[Offset + 1]) << 8) |
			(static_cast<uint32>(Bytes[Offset + 2]) << 16) |
			(static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4;
		return true;
	}

	/** 先读长度，再按 UTF-8 把字符串读出来。 */
	inline bool ReadString(const TArray<uint8>& Bytes, int32& Offset, FString& OutValue)
	{
		uint32 Length = 0;
		if (!ReadUInt32(Bytes, Offset, Length))
		{
			return false;
		}

		if (Length > static_cast<uint32>(Bytes.Num() - Offset))
		{
			return false;
		}

		FUTF8ToTCHAR Convert(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset), static_cast<int32>(Length));
		OutValue = FString(Convert.Length(), Convert.Get());
		Offset += static_cast<int32>(Length);
		return true;
	}

	/** 把 node 的公共字段写入字节流。 */
	inline void SerializeNodeFields(const FIndexedNode& Node, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Node.NodeType);
		WriteString(Bytes, Node.NodeName);
		WriteString(Bytes, Node.NodeClass);
		WriteString(Bytes, Node.Properties);
		WriteUInt32(Bytes, static_cast<uint32>(Node.PosX));
		WriteUInt32(Bytes, static_cast<uint32>(Node.PosY));
	}

	/** 从字节流里读回 node 的公共字段。 */
	inline bool DeserializeNodeFields(const TArray<uint8>& Bytes, int32& Offset, FIndexedNode& OutNode)
	{
		uint32 PosX = 0;
		uint32 PosY = 0;
		return ReadString(Bytes, Offset, OutNode.NodeType)
			&& ReadString(Bytes, Offset, OutNode.NodeName)
			&& ReadString(Bytes, Offset, OutNode.NodeClass)
			&& ReadString(Bytes, Offset, OutNode.Properties)
			&& ReadUInt32(Bytes, Offset, PosX)
			&& ReadUInt32(Bytes, Offset, PosY)
			&& ([](FIndexedNode& Node, uint32 X, uint32 Y)
			{
				Node.PosX = static_cast<int32>(X);
				Node.PosY = static_cast<int32>(Y);
				return true;
			}(OutNode, PosX, PosY));
	}

	/** 把变量字段写入字节流。 */
	inline void SerializeVariableFields(const FIndexedVariable& Variable, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Variable.VarName);
		WriteString(Bytes, Variable.VarType);
		WriteString(Bytes, Variable.Category);
		WriteString(Bytes, Variable.DefaultValue);
		WriteUInt8(Bytes, Variable.bIsExposed ? 1u : 0u);
		WriteUInt8(Bytes, Variable.bIsReplicated ? 1u : 0u);
	}

	/** 从字节流读回变量字段。 */
	inline bool DeserializeVariableFields(const TArray<uint8>& Bytes, int32& Offset, FIndexedVariable& OutVariable)
	{
		uint8 bIsExposed = 0;
		uint8 bIsReplicated = 0;
		const bool bSuccess = ReadString(Bytes, Offset, OutVariable.VarName)
			&& ReadString(Bytes, Offset, OutVariable.VarType)
			&& ReadString(Bytes, Offset, OutVariable.Category)
			&& ReadString(Bytes, Offset, OutVariable.DefaultValue)
			&& ReadUInt8(Bytes, Offset, bIsExposed)
			&& ReadUInt8(Bytes, Offset, bIsReplicated);
		if (!bSuccess)
		{
			return false;
		}

		OutVariable.bIsExposed = bIsExposed != 0;
		OutVariable.bIsReplicated = bIsReplicated != 0;
		return true;
	}

	/** 把 DataTable 行字段写入字节流。 */
	inline void SerializeDataTableRowFields(const FIndexedDataTableRow& Row, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Row.RowName);
		WriteString(Bytes, Row.RowData);
	}

	/** 从字节流里读回 DataTable 行字段。 */
	inline bool DeserializeDataTableRowFields(const TArray<uint8>& Bytes, int32& Offset, FIndexedDataTableRow& OutRow)
	{
		return ReadString(Bytes, Offset, OutRow.RowName)
			&& ReadString(Bytes, Offset, OutRow.RowData);
	}

	/** 把 dependency 行字段写入字节流。 */
	inline void SerializeDependencyFields(const FDependencyPayloadEntry& Entry, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Entry.TargetPackagePath);
		WriteString(Bytes, Entry.DependencyType);
	}

	/** 从字节流里读回 dependency 行字段。 */
	inline bool DeserializeDependencyFields(const TArray<uint8>& Bytes, int32& Offset, FDependencyPayloadEntry& OutEntry)
	{
		return ReadString(Bytes, Offset, OutEntry.TargetPackagePath)
			&& ReadString(Bytes, Offset, OutEntry.DependencyType);
	}

	/** 把 GameplayTag 引用字段写入字节流。 */
	inline void SerializeTagReferenceFields(const FTagReferencePayloadEntry& Entry, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Entry.TagName);
		WriteString(Bytes, Entry.Context);
	}

	/** 从字节流里读回 GameplayTag 引用字段。 */
	inline bool DeserializeTagReferenceFields(const TArray<uint8>& Bytes, int32& Offset, FTagReferencePayloadEntry& OutEntry)
	{
		return ReadString(Bytes, Offset, OutEntry.TagName)
			&& ReadString(Bytes, Offset, OutEntry.Context);
	}

	/** 把 Config 行字段写入字节流。 */
	inline void SerializeConfigFields(const FConfigPayloadEntry& Entry, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Entry.FilePath);
		WriteString(Bytes, Entry.Section);
		WriteString(Bytes, Entry.Key);
		WriteString(Bytes, Entry.Value);
	}

	/** 从字节流里读回 Config 行字段。 */
	inline bool DeserializeConfigFields(const TArray<uint8>& Bytes, int32& Offset, FConfigPayloadEntry& OutEntry)
	{
		return ReadString(Bytes, Offset, OutEntry.FilePath)
			&& ReadString(Bytes, Offset, OutEntry.Section)
			&& ReadString(Bytes, Offset, OutEntry.Key)
			&& ReadString(Bytes, Offset, OutEntry.Value);
	}

	/** 把 C++ 符号字段写入字节流。 */
	inline void SerializeCppSymbolFields(const FCppSymbolPayloadEntry& Entry, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Entry.FilePath);
		WriteString(Bytes, Entry.SymbolName);
		WriteString(Bytes, Entry.SymbolType);
		WriteString(Bytes, Entry.Signature);
		WriteUInt32(Bytes, static_cast<uint32>(Entry.LineNumber));
		WriteString(Bytes, Entry.ParentSymbol);
	}

	/** 从字节流里读回 C++ 符号字段。 */
	inline bool DeserializeCppSymbolFields(const TArray<uint8>& Bytes, int32& Offset, FCppSymbolPayloadEntry& OutEntry)
	{
		uint32 LineNumber = 0;
		const bool bSuccess = ReadString(Bytes, Offset, OutEntry.FilePath)
			&& ReadString(Bytes, Offset, OutEntry.SymbolName)
			&& ReadString(Bytes, Offset, OutEntry.SymbolType)
			&& ReadString(Bytes, Offset, OutEntry.Signature)
			&& ReadUInt32(Bytes, Offset, LineNumber)
			&& ReadString(Bytes, Offset, OutEntry.ParentSymbol);
		if (!bSuccess)
		{
			return false;
		}

		OutEntry.LineNumber = static_cast<int32>(LineNumber);
		return true;
	}

	/** 把 GameplayTag 定义字段写入字节流。 */
	inline void SerializeGameplayTagDefinitionFields(const FGameplayTagDefinitionPayloadEntry& Entry, TArray<uint8>& Bytes)
	{
		WriteString(Bytes, Entry.TagName);
		WriteString(Bytes, Entry.ParentTag);
	}

	/** 从字节流里读回 GameplayTag 定义字段。 */
	inline bool DeserializeGameplayTagDefinitionFields(
		const TArray<uint8>& Bytes,
		int32& Offset,
		FGameplayTagDefinitionPayloadEntry& OutEntry)
	{
		return ReadString(Bytes, Offset, OutEntry.TagName)
			&& ReadString(Bytes, Offset, OutEntry.ParentTag);
	}

	/** 把图连接字段写入字节流。 */
	inline void SerializeGraphConnectionFields(const FGraphPayloadConnection& Connection, TArray<uint8>& Bytes)
	{
		WriteUInt32(Bytes, static_cast<uint32>(Connection.SourceNodeIndex));
		WriteString(Bytes, Connection.SourcePin);
		WriteUInt32(Bytes, static_cast<uint32>(Connection.TargetNodeIndex));
		WriteString(Bytes, Connection.TargetPin);
		WriteString(Bytes, Connection.PinType);
	}

	/** 从字节流读回图连接字段。 */
	inline bool DeserializeGraphConnectionFields(const TArray<uint8>& Bytes, int32& Offset, FGraphPayloadConnection& OutConnection)
	{
		uint32 SourceNodeIndex = 0;
		uint32 TargetNodeIndex = 0;
		const bool bSuccess = ReadUInt32(Bytes, Offset, SourceNodeIndex)
			&& ReadString(Bytes, Offset, OutConnection.SourcePin)
			&& ReadUInt32(Bytes, Offset, TargetNodeIndex)
			&& ReadString(Bytes, Offset, OutConnection.TargetPin)
			&& ReadString(Bytes, Offset, OutConnection.PinType);
		if (!bSuccess)
		{
			return false;
		}

		OutConnection.SourceNodeIndex = static_cast<int32>(SourceNodeIndex);
		OutConnection.TargetNodeIndex = static_cast<int32>(TargetNodeIndex);
		return true;
	}

	/** 序列化单 node 载荷，并写入版本号。 */
	inline void SerializeNodePayload(const FNodePayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		SerializeNodeFields(Payload.Node, OutBytes);
	}

	/** 反序列化单 node 载荷。 */
	inline bool DeserializeNodePayload(const TArray<uint8>& Bytes, FNodePayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		return ReadUInt8(Bytes, Offset, Version)
			&& Version == 1
			&& DeserializeNodeFields(Bytes, Offset, OutPayload.Node);
	}

	/** 序列化多 node 载荷。 */
	inline void SerializeNodesPayload(const FNodesPayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Nodes.Num()));
		for (const FIndexedNode& Node : Payload.Nodes)
		{
			SerializeNodeFields(Node, OutBytes);
		}
	}

	/** 反序列化多 node 载荷。 */
	inline bool DeserializeNodesPayload(const TArray<uint8>& Bytes, FNodesPayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 NodeCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}

		if (!ReadUInt32(Bytes, Offset, NodeCount))
		{
			return false;
		}

		OutPayload.Nodes.Reset();
		OutPayload.Nodes.Reserve(static_cast<int32>(NodeCount));
		for (uint32 Index = 0; Index < NodeCount; ++Index)
		{
			FIndexedNode Node;
			if (!DeserializeNodeFields(Bytes, Offset, Node))
			{
				return false;
			}

			OutPayload.Nodes.Add(MoveTemp(Node));
		}

		return true;
	}

	/** 序列化 node+variables 载荷。 */
	inline void SerializeNodeVariablePayload(const FNodeVariablePayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		SerializeNodeFields(Payload.Node, OutBytes);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Variables.Num()));
		for (const FIndexedVariable& Variable : Payload.Variables)
		{
			SerializeVariableFields(Variable, OutBytes);
		}
	}

	/** 反序列化 node+variables 载荷。 */
	inline bool DeserializeNodeVariablePayload(const TArray<uint8>& Bytes, FNodeVariablePayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 VariableCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}

		if (!DeserializeNodeFields(Bytes, Offset, OutPayload.Node) || !ReadUInt32(Bytes, Offset, VariableCount))
		{
			return false;
		}

		OutPayload.Variables.Reset();
		OutPayload.Variables.Reserve(static_cast<int32>(VariableCount));
		for (uint32 Index = 0; Index < VariableCount; ++Index)
		{
			FIndexedVariable Variable;
			if (!DeserializeVariableFields(Bytes, Offset, Variable))
			{
				return false;
			}
			OutPayload.Variables.Add(MoveTemp(Variable));
		}
		return true;
	}

	/** 序列化 DataTable 载荷。 */
	inline void SerializeDataTablePayload(const FDataTablePayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Rows.Num()));
		for (const FIndexedDataTableRow& Row : Payload.Rows)
		{
			SerializeDataTableRowFields(Row, OutBytes);
		}
	}

	/** 反序列化 DataTable 载荷。 */
	inline bool DeserializeDataTablePayload(const TArray<uint8>& Bytes, FDataTablePayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 RowCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}

		if (!ReadUInt32(Bytes, Offset, RowCount))
		{
			return false;
		}

		OutPayload.Rows.Reset();
		OutPayload.Rows.Reserve(static_cast<int32>(RowCount));
		for (uint32 Index = 0; Index < RowCount; ++Index)
		{
			FIndexedDataTableRow Row;
			if (!DeserializeDataTableRowFields(Bytes, Offset, Row))
			{
				return false;
			}

			OutPayload.Rows.Add(MoveTemp(Row));
		}

		return true;
	}

	/** 序列化 dependency 载荷。 */
	inline void SerializeDependencyPayload(const FDependencyPayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Dependencies.Num()));
		for (const FDependencyPayloadEntry& Entry : Payload.Dependencies)
		{
			SerializeDependencyFields(Entry, OutBytes);
		}
	}

	/** 反序列化 dependency 载荷。 */
	inline bool DeserializeDependencyPayload(const TArray<uint8>& Bytes, FDependencyPayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 DependencyCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}

		if (!ReadUInt32(Bytes, Offset, DependencyCount))
		{
			return false;
		}

		OutPayload.Dependencies.Reset();
		OutPayload.Dependencies.Reserve(static_cast<int32>(DependencyCount));
		for (uint32 Index = 0; Index < DependencyCount; ++Index)
		{
			FDependencyPayloadEntry Entry;
			if (!DeserializeDependencyFields(Bytes, Offset, Entry))
			{
				return false;
			}

			OutPayload.Dependencies.Add(MoveTemp(Entry));
		}

		return true;
	}

	/** 序列化 GameplayTag 引用载荷。 */
	inline void SerializeTagReferencePayload(const FTagReferencePayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.References.Num()));
		for (const FTagReferencePayloadEntry& Entry : Payload.References)
		{
			SerializeTagReferenceFields(Entry, OutBytes);
		}
	}

	/** 反序列化 GameplayTag 引用载荷。 */
	inline bool DeserializeTagReferencePayload(const TArray<uint8>& Bytes, FTagReferencePayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 ReferenceCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}

		if (!ReadUInt32(Bytes, Offset, ReferenceCount))
		{
			return false;
		}

		OutPayload.References.Reset();
		OutPayload.References.Reserve(static_cast<int32>(ReferenceCount));
		for (uint32 Index = 0; Index < ReferenceCount; ++Index)
		{
			FTagReferencePayloadEntry Entry;
			if (!DeserializeTagReferenceFields(Bytes, Offset, Entry))
			{
				return false;
			}

			OutPayload.References.Add(MoveTemp(Entry));
		}

		return true;
	}

	/** 序列化 Config 全局载荷。 */
	inline void SerializeConfigPayload(const FConfigPayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Entries.Num()));
		for (const FConfigPayloadEntry& Entry : Payload.Entries)
		{
			SerializeConfigFields(Entry, OutBytes);
		}
	}

	/** 反序列化 Config 全局载荷。 */
	inline bool DeserializeConfigPayload(const TArray<uint8>& Bytes, FConfigPayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 EntryCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}
		if (!ReadUInt32(Bytes, Offset, EntryCount))
		{
			return false;
		}

		OutPayload.Entries.Reset();
		OutPayload.Entries.Reserve(static_cast<int32>(EntryCount));
		for (uint32 Index = 0; Index < EntryCount; ++Index)
		{
			FConfigPayloadEntry Entry;
			if (!DeserializeConfigFields(Bytes, Offset, Entry))
			{
				return false;
			}
			OutPayload.Entries.Add(MoveTemp(Entry));
		}

		return true;
	}

	/** 序列化 C++ 符号全局载荷。 */
	inline void SerializeCppSymbolPayload(const FCppSymbolPayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Symbols.Num()));
		for (const FCppSymbolPayloadEntry& Entry : Payload.Symbols)
		{
			SerializeCppSymbolFields(Entry, OutBytes);
		}
	}

	/** 反序列化 C++ 符号全局载荷。 */
	inline bool DeserializeCppSymbolPayload(const TArray<uint8>& Bytes, FCppSymbolPayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 SymbolCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}
		if (!ReadUInt32(Bytes, Offset, SymbolCount))
		{
			return false;
		}

		OutPayload.Symbols.Reset();
		OutPayload.Symbols.Reserve(static_cast<int32>(SymbolCount));
		for (uint32 Index = 0; Index < SymbolCount; ++Index)
		{
			FCppSymbolPayloadEntry Entry;
			if (!DeserializeCppSymbolFields(Bytes, Offset, Entry))
			{
				return false;
			}
			OutPayload.Symbols.Add(MoveTemp(Entry));
		}

		return true;
	}

	/** 序列化 GameplayTag 定义全局载荷。 */
	inline void SerializeGameplayTagDefinitionPayload(const FGameplayTagDefinitionPayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);
		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Definitions.Num()));
		for (const FGameplayTagDefinitionPayloadEntry& Entry : Payload.Definitions)
		{
			SerializeGameplayTagDefinitionFields(Entry, OutBytes);
		}
	}

	/** 反序列化 GameplayTag 定义全局载荷。 */
	inline bool DeserializeGameplayTagDefinitionPayload(
		const TArray<uint8>& Bytes,
		FGameplayTagDefinitionPayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 DefinitionCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}
		if (!ReadUInt32(Bytes, Offset, DefinitionCount))
		{
			return false;
		}

		OutPayload.Definitions.Reset();
		OutPayload.Definitions.Reserve(static_cast<int32>(DefinitionCount));
		for (uint32 Index = 0; Index < DefinitionCount; ++Index)
		{
			FGameplayTagDefinitionPayloadEntry Entry;
			if (!DeserializeGameplayTagDefinitionFields(Bytes, Offset, Entry))
			{
				return false;
			}
			OutPayload.Definitions.Add(MoveTemp(Entry));
		}

		return true;
	}

	/** 序列化 graph 载荷。
	 *
	 * 写入顺序固定为：
	 * 1. version
	 * 2. nodes
	 * 3. variables
	 * 4. connections
	 *
	 * 以后如果 schema 升级，只需要 bump version 并在反序列化里分支处理。
	 */
	inline void SerializeGraphPayload(const FGraphPayload& Payload, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		WriteUInt8(OutBytes, 1);

		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Nodes.Num()));
		for (const FIndexedNode& Node : Payload.Nodes)
		{
			SerializeNodeFields(Node, OutBytes);
		}

		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Variables.Num()));
		for (const FIndexedVariable& Variable : Payload.Variables)
		{
			SerializeVariableFields(Variable, OutBytes);
		}

		WriteUInt32(OutBytes, static_cast<uint32>(Payload.Connections.Num()));
		for (const FGraphPayloadConnection& Connection : Payload.Connections)
		{
			SerializeGraphConnectionFields(Connection, OutBytes);
		}
	}

	/** 反序列化 graph 载荷。 */
	inline bool DeserializeGraphPayload(const TArray<uint8>& Bytes, FGraphPayload& OutPayload)
	{
		int32 Offset = 0;
		uint8 Version = 0;
		uint32 NodeCount = 0;
		uint32 VariableCount = 0;
		uint32 ConnectionCount = 0;
		if (!ReadUInt8(Bytes, Offset, Version) || Version != 1)
		{
			return false;
		}

		if (!ReadUInt32(Bytes, Offset, NodeCount))
		{
			return false;
		}

		OutPayload = FGraphPayload();
		OutPayload.Nodes.Reserve(static_cast<int32>(NodeCount));
		for (uint32 Index = 0; Index < NodeCount; ++Index)
		{
			FIndexedNode Node;
			if (!DeserializeNodeFields(Bytes, Offset, Node))
			{
				return false;
			}

			OutPayload.Nodes.Add(MoveTemp(Node));
		}

		if (!ReadUInt32(Bytes, Offset, VariableCount))
		{
			return false;
		}

		OutPayload.Variables.Reserve(static_cast<int32>(VariableCount));
		for (uint32 Index = 0; Index < VariableCount; ++Index)
		{
			FIndexedVariable Variable;
			if (!DeserializeVariableFields(Bytes, Offset, Variable))
			{
				return false;
			}

			OutPayload.Variables.Add(MoveTemp(Variable));
		}

		if (!ReadUInt32(Bytes, Offset, ConnectionCount))
		{
			return false;
		}

		OutPayload.Connections.Reserve(static_cast<int32>(ConnectionCount));
		for (uint32 Index = 0; Index < ConnectionCount; ++Index)
		{
			FGraphPayloadConnection Connection;
			if (!DeserializeGraphConnectionFields(Bytes, Offset, Connection))
			{
				return false;
			}

			OutPayload.Connections.Add(MoveTemp(Connection));
		}

		return true;
	}

	/** 把单 node 载荷写回正式生产表。 */
	inline bool MaterializeNodePayload(const FNodePayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		FIndexedNode Node = Payload.Node;
		Node.AssetId = AssetId;
		return DB.InsertNode(Node) > 0;
	}

	/** 把 node+variables 载荷写回正式生产表。 */
	inline bool MaterializeNodeVariablePayload(const FNodeVariablePayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		FIndexedNode Node = Payload.Node;
		Node.AssetId = AssetId;
		if (DB.InsertNode(Node) <= 0)
		{
			return false;
		}

		for (const FIndexedVariable& VariablePayload : Payload.Variables)
		{
			FIndexedVariable Variable = VariablePayload;
			Variable.AssetId = AssetId;
			if (DB.InsertVariable(Variable) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把多 node 载荷写回正式生产表。 */
	inline bool MaterializeNodesPayload(const FNodesPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		for (const FIndexedNode& NodePayload : Payload.Nodes)
		{
			FIndexedNode Node = NodePayload;
			Node.AssetId = AssetId;
			if (DB.InsertNode(Node) <= 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把 DataTable 载荷写回正式生产表。 */
	inline bool MaterializeDataTablePayload(const FDataTablePayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		for (const FIndexedDataTableRow& RowPayload : Payload.Rows)
		{
			FIndexedDataTableRow Row = RowPayload;
			Row.AssetId = AssetId;
			if (DB.InsertDataTableRow(Row) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 从完整 tag 名推导父 tag 名。 */
	inline FString DeriveParentGameplayTagName(const FString& TagName)
	{
		int32 LastSeparatorIndex = INDEX_NONE;
		if (TagName.FindLastChar(TEXT('.'), LastSeparatorIndex) && LastSeparatorIndex > 0)
		{
			return TagName.Left(LastSeparatorIndex);
		}

		return FString();
	}

	/** 把 dependency 载荷写回正式生产表。 */
	inline bool MaterializeDependencyPayload(const FDependencyPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		for (const FDependencyPayloadEntry& Entry : Payload.Dependencies)
		{
			const int64 TargetAssetId = DB.GetAssetId(Entry.TargetPackagePath);
			if (TargetAssetId <= 0)
			{
				// 目标包当前不在本地索引范围里时，生产表也不应该强行写入一条悬空边。
				continue;
			}

			FIndexedDependency Dependency;
			Dependency.SourceAssetId = AssetId;
			Dependency.TargetAssetId = TargetAssetId;
			Dependency.DependencyType = Entry.DependencyType;
			if (DB.InsertDependency(Dependency) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把 GameplayTag 引用载荷写回正式生产表。 */
	inline bool MaterializeTagReferencePayload(const FTagReferencePayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		for (const FTagReferencePayloadEntry& Entry : Payload.References)
		{
			const int64 TagId = DB.GetOrCreateTag(Entry.TagName, DeriveParentGameplayTagName(Entry.TagName));
			if (TagId < 0)
			{
				return false;
			}

			FIndexedTagReference Reference;
			Reference.TagId = TagId;
			Reference.AssetId = AssetId;
			Reference.Context = Entry.Context;
			if (DB.InsertTagReference(Reference) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把 Config 全局载荷写回正式生产表。 */
	inline bool MaterializeConfigPayload(const FConfigPayload& Payload, FMonolithIndexDatabase& DB)
	{
		if (!DB.ClearConfigIndex())
		{
			return false;
		}

		for (const FConfigPayloadEntry& EntryPayload : Payload.Entries)
		{
			FIndexedConfig Entry;
			Entry.FilePath = EntryPayload.FilePath;
			Entry.Section = EntryPayload.Section;
			Entry.Key = EntryPayload.Key;
			Entry.Value = EntryPayload.Value;
			if (DB.InsertConfig(Entry) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把 C++ 符号全局载荷写回正式生产表。 */
	inline bool MaterializeCppSymbolPayload(const FCppSymbolPayload& Payload, FMonolithIndexDatabase& DB)
	{
		if (!DB.ClearCppSymbolIndex())
		{
			return false;
		}

		for (const FCppSymbolPayloadEntry& EntryPayload : Payload.Symbols)
		{
			FIndexedCppSymbol Symbol;
			Symbol.FilePath = EntryPayload.FilePath;
			Symbol.SymbolName = EntryPayload.SymbolName;
			Symbol.SymbolType = EntryPayload.SymbolType;
			Symbol.Signature = EntryPayload.Signature;
			Symbol.LineNumber = EntryPayload.LineNumber;
			Symbol.ParentSymbol = EntryPayload.ParentSymbol;
			if (DB.InsertCppSymbol(Symbol) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把 GameplayTag 定义全局载荷写回正式生产表。
	 *
	 * 这里故意不用“先清 tags 再重写”的方式，
	 * 因为 tag_references 通过外键依赖 tags.id。
	 *
	 * 当前正式行为仍然是：
	 * - 有定义就补齐；
	 * - 已存在的定义尽量复用同一行；
	 * - 引用关系保持不动。
	 */
	inline bool MaterializeGameplayTagDefinitionPayload(
		const FGameplayTagDefinitionPayload& Payload,
		FMonolithIndexDatabase& DB)
	{
		for (const FGameplayTagDefinitionPayloadEntry& Entry : Payload.Definitions)
		{
			if (DB.GetOrCreateTag(Entry.TagName, Entry.ParentTag) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把 graph 载荷写回正式生产表。
	 *
	 * 做法分三步：
	 * 1. 先插 nodes，拿到真实 node id；
	 * 2. 再插 variables；
	 * 3. 最后把“本地节点下标连线”换算成“真实 node id 连线”。
	 *
	 * 这样 artifact 在缓存里保持轻量稳定，
	 * 真正落库时才和当前数据库行号发生关系。
	 */
	inline bool MaterializeGraphPayload(const FGraphPayload& Payload, FMonolithIndexDatabase& DB, const int64 AssetId)
	{
		TArray<int64> NodeIds;
		NodeIds.Reserve(Payload.Nodes.Num());
		for (const FIndexedNode& NodePayload : Payload.Nodes)
		{
			FIndexedNode Node = NodePayload;
			Node.AssetId = AssetId;
			const int64 NodeId = DB.InsertNode(Node);
			if (NodeId <= 0)
			{
				return false;
			}

			NodeIds.Add(NodeId);
		}

		for (const FIndexedVariable& VariablePayload : Payload.Variables)
		{
			FIndexedVariable Variable = VariablePayload;
			Variable.AssetId = AssetId;
			if (DB.InsertVariable(Variable) < 0)
			{
				return false;
			}
		}

		for (const FGraphPayloadConnection& ConnectionPayload : Payload.Connections)
		{
			if (!NodeIds.IsValidIndex(ConnectionPayload.SourceNodeIndex) || !NodeIds.IsValidIndex(ConnectionPayload.TargetNodeIndex))
			{
				return false;
			}

			FIndexedConnection Connection;
			Connection.SourceNodeId = NodeIds[ConnectionPayload.SourceNodeIndex];
			Connection.SourcePin = ConnectionPayload.SourcePin;
			Connection.TargetNodeId = NodeIds[ConnectionPayload.TargetNodeIndex];
			Connection.TargetPin = ConnectionPayload.TargetPin;
			Connection.PinType = ConnectionPayload.PinType;
			if (DB.InsertConnection(Connection) < 0)
			{
				return false;
			}
		}

		return true;
	}

	/** 把单 node 载荷写入 shadow 表，供 diff 阶段比较。 */
	inline bool MaterializeNodePayloadToShadow(
		const FNodePayload& Payload,
		FMonolithIndexDatabase& DB,
		const int64 AssetId,
		const FString& CohortName)
	{
		FMonolithShadowIndexedNode ShadowNode;
		ShadowNode.Node = Payload.Node;
		ShadowNode.Node.AssetId = AssetId;
		ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);

		TArray<FMonolithShadowIndexedNode> ShadowNodes;
		ShadowNodes.Add(MoveTemp(ShadowNode));
		return DB.ReplaceShadowNodesForAsset(CohortName, AssetId, ShadowNodes);
	}

	/** 把 node+variables 载荷写入 shadow 表。 */
	inline bool MaterializeNodeVariablePayloadToShadow(
		const FNodeVariablePayload& Payload,
		FMonolithIndexDatabase& DB,
		const int64 AssetId,
		const FString& CohortName)
	{
		FMonolithShadowIndexedNode ShadowNode;
		ShadowNode.Node = Payload.Node;
		ShadowNode.Node.AssetId = AssetId;
		ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);

		TArray<FMonolithShadowIndexedNode> ShadowNodes;
		ShadowNodes.Add(MoveTemp(ShadowNode));

		TArray<FMonolithShadowIndexedVariable> ShadowVariables;
		ShadowVariables.Reserve(Payload.Variables.Num());
		for (const FIndexedVariable& VariablePayload : Payload.Variables)
		{
			FMonolithShadowIndexedVariable ShadowVariable;
			ShadowVariable.Variable = VariablePayload;
			ShadowVariable.Variable.AssetId = AssetId;
			ShadowVariable.RowHash = ComputeVariableRowHash(ShadowVariable.Variable);
			ShadowVariables.Add(MoveTemp(ShadowVariable));
		}

		return DB.ReplaceShadowNodesForAsset(CohortName, AssetId, ShadowNodes)
			&& DB.ReplaceShadowVariablesForAsset(CohortName, AssetId, ShadowVariables);
	}

	/** 把多 node 载荷写入 shadow 表。 */
	inline bool MaterializeNodesPayloadToShadow(
		const FNodesPayload& Payload,
		FMonolithIndexDatabase& DB,
		const int64 AssetId,
		const FString& CohortName)
	{
		TArray<FMonolithShadowIndexedNode> ShadowNodes;
		ShadowNodes.Reserve(Payload.Nodes.Num());
		for (const FIndexedNode& NodePayload : Payload.Nodes)
		{
			FMonolithShadowIndexedNode ShadowNode;
			ShadowNode.Node = NodePayload;
			ShadowNode.Node.AssetId = AssetId;
			ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);
			ShadowNodes.Add(MoveTemp(ShadowNode));
		}

		return DB.ReplaceShadowNodesForAsset(CohortName, AssetId, ShadowNodes);
	}

	/** 把 DataTable 载荷写入 shadow 表。 */
	inline bool MaterializeDataTablePayloadToShadow(
		const FDataTablePayload& Payload,
		FMonolithIndexDatabase& DB,
		const int64 AssetId,
		const FString& CohortName)
	{
		TArray<FMonolithShadowIndexedDataTableRow> ShadowRows;
		ShadowRows.Reserve(Payload.Rows.Num());
		for (const FIndexedDataTableRow& RowPayload : Payload.Rows)
		{
			FMonolithShadowIndexedDataTableRow ShadowRow;
			ShadowRow.Row = RowPayload;
			ShadowRow.Row.AssetId = AssetId;
			ShadowRow.RowHash = ComputeDataTableRowHash(ShadowRow.Row);
			ShadowRows.Add(MoveTemp(ShadowRow));
		}

		return DB.ReplaceShadowDataTableRowsForAsset(CohortName, AssetId, ShadowRows);
	}

	/** 把 dependency 载荷写入 shadow 表。 */
	inline bool MaterializeDependencyPayloadToShadow(
		const FDependencyPayload& Payload,
		FMonolithIndexDatabase& DB,
		const int64 AssetId,
		const FString& CohortName)
	{
		TArray<FMonolithShadowIndexedDependency> ShadowDependencies;
		ShadowDependencies.Reserve(Payload.Dependencies.Num());
		for (const FDependencyPayloadEntry& Entry : Payload.Dependencies)
		{
			if (DB.GetAssetId(Entry.TargetPackagePath) <= 0)
			{
				// 影子表要和生产路径比较，所以也沿用“目标必须可解析”的规则。
				continue;
			}

			FMonolithShadowIndexedDependency ShadowDependency;
			ShadowDependency.TargetPackagePath = Entry.TargetPackagePath;
			ShadowDependency.DependencyType = Entry.DependencyType;
			ShadowDependency.RowHash = ComputeDependencyRowHash(
				ShadowDependency.TargetPackagePath,
				ShadowDependency.DependencyType);
			ShadowDependencies.Add(MoveTemp(ShadowDependency));
		}

		return DB.ReplaceShadowDependenciesForAsset(CohortName, AssetId, ShadowDependencies);
	}

	/** 把 GameplayTag 引用载荷写入 shadow 表。 */
	inline bool MaterializeTagReferencePayloadToShadow(
		const FTagReferencePayload& Payload,
		FMonolithIndexDatabase& DB,
		const int64 AssetId,
		const FString& CohortName)
	{
		TArray<FMonolithShadowIndexedTagReference> ShadowReferences;
		ShadowReferences.Reserve(Payload.References.Num());
		for (const FTagReferencePayloadEntry& Entry : Payload.References)
		{
			FMonolithShadowIndexedTagReference ShadowReference;
			ShadowReference.TagName = Entry.TagName;
			ShadowReference.Context = Entry.Context;
			ShadowReference.RowHash = ComputeTagReferenceRowHash(
				ShadowReference.TagName,
				ShadowReference.Context);
			ShadowReferences.Add(MoveTemp(ShadowReference));
		}

		return DB.ReplaceShadowTagReferencesForAsset(CohortName, AssetId, ShadowReferences);
	}

	/** 把 graph 载荷写入 shadow 表。 */
	inline bool MaterializeGraphPayloadToShadow(
		const FGraphPayload& Payload,
		FMonolithIndexDatabase& DB,
		const int64 AssetId,
		const FString& CohortName)
	{
		TArray<FMonolithShadowIndexedNode> ShadowNodes;
		TArray<uint64> NodeRowHashes;
		ShadowNodes.Reserve(Payload.Nodes.Num());
		NodeRowHashes.Reserve(Payload.Nodes.Num());
		for (const FIndexedNode& NodePayload : Payload.Nodes)
		{
			FMonolithShadowIndexedNode ShadowNode;
			ShadowNode.Node = NodePayload;
			ShadowNode.Node.AssetId = AssetId;
			ShadowNode.RowHash = ComputeNodeRowHash(ShadowNode.Node);
			NodeRowHashes.Add(ShadowNode.RowHash);
			ShadowNodes.Add(MoveTemp(ShadowNode));
		}

		TArray<FMonolithShadowIndexedVariable> ShadowVariables;
		ShadowVariables.Reserve(Payload.Variables.Num());
		for (const FIndexedVariable& VariablePayload : Payload.Variables)
		{
			FMonolithShadowIndexedVariable ShadowVariable;
			ShadowVariable.Variable = VariablePayload;
			ShadowVariable.Variable.AssetId = AssetId;
			ShadowVariable.RowHash = ComputeVariableRowHash(ShadowVariable.Variable);
			ShadowVariables.Add(MoveTemp(ShadowVariable));
		}

		TArray<FMonolithShadowIndexedConnection> ShadowConnections;
		ShadowConnections.Reserve(Payload.Connections.Num());
		for (const FGraphPayloadConnection& ConnectionPayload : Payload.Connections)
		{
			if (!NodeRowHashes.IsValidIndex(ConnectionPayload.SourceNodeIndex) || !NodeRowHashes.IsValidIndex(ConnectionPayload.TargetNodeIndex))
			{
				return false;
			}

			FMonolithShadowIndexedConnection ShadowConnection;
			ShadowConnection.SourceNodeRowHash = NodeRowHashes[ConnectionPayload.SourceNodeIndex];
			ShadowConnection.SourcePin = ConnectionPayload.SourcePin;
			ShadowConnection.TargetNodeRowHash = NodeRowHashes[ConnectionPayload.TargetNodeIndex];
			ShadowConnection.TargetPin = ConnectionPayload.TargetPin;
			ShadowConnection.PinType = ConnectionPayload.PinType;
			ShadowConnection.RowHash = ComputeConnectionRowHash(
				ShadowConnection.SourceNodeRowHash,
				ShadowConnection.SourcePin,
				ShadowConnection.TargetNodeRowHash,
				ShadowConnection.TargetPin,
				ShadowConnection.PinType);
			ShadowConnections.Add(MoveTemp(ShadowConnection));
		}

		return DB.ReplaceShadowNodesForAsset(CohortName, AssetId, ShadowNodes)
			&& DB.ReplaceShadowVariablesForAsset(CohortName, AssetId, ShadowVariables)
			&& DB.ReplaceShadowConnectionsForAsset(CohortName, AssetId, ShadowConnections);
	}
}
