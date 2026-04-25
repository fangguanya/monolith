#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "Misc/Paths.h"
#include "Serialization/JsonTypes.h"

/*
 * project.* 这组 action 的输入输出风格基本一致：
 * - 先从 JSON 里取参数；
 * - 再拿 MonolithIndex 子系统；
 * - 最后把结果包装成 FMonolithActionResult。
 *
 * 这些动作本身都很薄，如果每个文件各写一遍：
 * - 参数缺失报错；
 * - optional 字段默认值；
 * - GEditor -> GetEditorSubsystem；
 * 很容易长出重复样板。
 *
 * 这里把真正通用的那一层收口成一份小工具，
 * 让每个 action 文件只保留“自己的业务语义”。
 */
namespace MonolithProjectActionUtils
{
	/** 统一构造“缺少必填字符串参数”的错误。 */
	inline FMonolithActionResult MakeMissingStringParamError(const TCHAR* ParamName)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' parameter is required"), ParamName),
			-32602);
	}

	/** 统一构造“索引子系统当前不可用”的错误。 */
	inline FMonolithActionResult MakeSubsystemUnavailableError()
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}

	/** 读取 MonolithIndex 子系统。
	 * 这里保留成一个集中入口，避免每个 action 都重复写一遍 GEditor 判空。 */
	inline UMonolithIndexSubsystem* GetIndexSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	}

	/** 在不触碰 editor subsystem 的情况下解析 ProjectIndex.db 路径，供后台只读查询使用。 */
	inline FString ResolveIndexDatabasePath()
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		if (Settings && !Settings->DatabasePathOverride.Path.IsEmpty())
		{
			FString OverridePath = FPaths::ConvertRelativePathToFull(Settings->DatabasePathOverride.Path);
			if (FPaths::GetExtension(OverridePath).Equals(TEXT("db"), ESearchCase::IgnoreCase))
			{
				return OverridePath;
			}
			return OverridePath / TEXT("ProjectIndex.db");
		}

		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("ProjectIndex.db");
		}
		return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("ProjectIndex.db");
	}

	/** 在 commandlet / 无 GEditor 场景下打开一个独立查询连接。
	 * 编辑器进程内有 UMonolithIndexSubsystem 时不要再走这条路 —— 
	 * 用 RunReadDatabaseAction 复用子系统持有的主连接更稳。 */
	inline bool OpenQueryOnlyIndexDatabase(FMonolithIndexDatabase& Database, FString& OutError)
	{
		const FString DatabasePath = ResolveIndexDatabasePath();
		if (!FPaths::FileExists(DatabasePath))
		{
			OutError = FString::Printf(TEXT("Index database not found: %s"), *DatabasePath);
			return false;
		}
		if (!Database.OpenQueryOnly(DatabasePath))
		{
			OutError = FString::Printf(TEXT("Failed to open index database query-only: %s"), *DatabasePath);
			return false;
		}
		return true;
	}

	/** project.* 查询统一入口。
	 * 编辑器里走 UMonolithIndexSubsystem 持有的主连接 + 互斥锁，
	 * 没有 GEditor 时（比如 commandlet）退回去自己开一个一次性 query-only 连接。
	 *
	 * 这样所有 action 不再各自反复 sqlite3_open —— 这是“频繁 Failed to open index database”
	 * 这类错误最容易出现的地方。 */
	inline FMonolithActionResult RunReadDatabaseAction(
		TFunctionRef<FMonolithActionResult(FMonolithIndexDatabase&)> Func)
	{
		if (UMonolithIndexSubsystem* Subsystem = GetIndexSubsystem())
		{
			return Subsystem->RunReadDatabaseAction(Func);
		}

		// commandlet / 无编辑器场景 —— 自己开一个一次性查询连接。
		FMonolithIndexDatabase Database;
		FString Error;
		if (!OpenQueryOnlyIndexDatabase(Database, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		return Func(Database);
	}

	/** 读取可选字符串参数；缺失或类型不对时，返回调用方提供的默认值。 */
	inline FString GetOptionalStringParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* ParamName,
		const FString& DefaultValue = FString())
	{
		if (!Params.IsValid() || !Params->HasTypedField<EJson::String>(ParamName))
		{
			return DefaultValue;
		}

		return Params->GetStringField(ParamName);
	}

	/** 读取可选整数参数；缺失或类型不对时，返回调用方提供的默认值。 */
	inline int32 GetOptionalIntParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* ParamName,
		const int32 DefaultValue)
	{
		if (!Params.IsValid() || !Params->HasTypedField<EJson::Number>(ParamName))
		{
			return DefaultValue;
		}

		return Params->GetIntegerField(ParamName);
	}

	/** 读取必填字符串参数。
	 * 这里既要求字段存在，也要求值非空字符串。 */
	inline bool TryGetRequiredStringParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* ParamName,
		FString& OutValue)
	{
		OutValue = GetOptionalStringParam(Params, ParamName);
		return !OutValue.IsEmpty();
	}
}
