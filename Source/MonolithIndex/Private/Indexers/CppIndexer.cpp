#include "Indexers/CppIndexer.h"

#include "MonolithIndexLog.h"

#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"

namespace CppIndexerInternal
{
	/** 这份全局 artifact 在 identity 里对应的逻辑“包名”。 */
	static const FName GlobalPackageName(TEXT("/Monolith/Global/Cpp"));

	/** 统一创建一条符号 payload。 */
	static MonolithSimpleArtifactSerialization::FCppSymbolPayloadEntry MakeSymbolEntry(
		const FString& FilePath,
		const FString& SymbolName,
		const FString& SymbolType,
		const FString& Signature,
		const int32 LineNumber,
		const FString& ParentSymbol)
	{
		MonolithSimpleArtifactSerialization::FCppSymbolPayloadEntry Entry;
		Entry.FilePath = FilePath;
		Entry.SymbolName = SymbolName;
		Entry.SymbolType = SymbolType;
		Entry.Signature = Signature;
		Entry.LineNumber = LineNumber;
		Entry.ParentSymbol = ParentSymbol;
		return Entry;
	}
}

bool FCppIndexer::IndexGlobal(FMonolithIndexDatabase& DB)
{
	// C++ reducer 现在只保留 artifact 主链这一种实现。
	return MonolithGlobalArtifactHelpers::ExecuteIndexGlobalFromArtifact(*this, DB);
}

bool FCppIndexer::BuildGlobalArtifactIdentity(FMonolithArtifactIdentityV1& OutIdentity) const
{
	return MonolithGlobalArtifactHelpers::BuildInputFileManifestIdentity(
		*this,
		CppIndexerInternal::GlobalPackageName,
		[](TArray<MonolithGlobalArtifactHelpers::FInputFile>& OutFiles)
		{
			CollectSourceFiles(OutFiles);
		},
		OutIdentity);
}

bool FCppIndexer::BuildGlobalArtifact(FMonolithArtifact& OutArtifact)
{
	return MonolithGlobalArtifactHelpers::BuildGlobalPayloadArtifact<MonolithSimpleArtifactSerialization::FCppSymbolPayload>(
		*this,
		CppIndexerInternal::GlobalPackageName,
		[](MonolithSimpleArtifactSerialization::FCppSymbolPayload& Payload)
		{
			return BuildPayload(Payload);
		},
		[](const MonolithSimpleArtifactSerialization::FCppSymbolPayload& Payload, TArray<uint8>& OutBytes)
		{
			MonolithSimpleArtifactSerialization::SerializeCppSymbolPayload(Payload, OutBytes);
		},
		[](const MonolithSimpleArtifactSerialization::FCppSymbolPayload& Payload)
		{
			return Payload.Symbols.Num();
		},
		OutArtifact);
}

bool FCppIndexer::MaterializeGlobalArtifact(const FMonolithArtifact& Artifact, FMonolithIndexDatabase& DB)
{
	return MonolithGlobalArtifactHelpers::MaterializeGlobalPayloadArtifact<MonolithSimpleArtifactSerialization::FCppSymbolPayload>(
		Artifact,
		DB,
		[](const TArray<uint8>& Bytes, MonolithSimpleArtifactSerialization::FCppSymbolPayload& Payload)
		{
			return MonolithSimpleArtifactSerialization::DeserializeCppSymbolPayload(Bytes, Payload);
		},
		[](const MonolithSimpleArtifactSerialization::FCppSymbolPayload& Payload, FMonolithIndexDatabase& InDB)
		{
			return MonolithSimpleArtifactSerialization::MaterializeCppSymbolPayload(Payload, InDB);
		},
		[](const MonolithSimpleArtifactSerialization::FCppSymbolPayload& Payload)
		{
			return Payload.Symbols.Num();
		},
		TEXT("CppIndexer"),
		TEXT("C++ symbols"));
}

void FCppIndexer::CollectSourceFiles(TArray<MonolithGlobalArtifactHelpers::FInputFile>& OutFiles)
{
	OutFiles.Reset();
	MonolithGlobalArtifactHelpers::AppendFilesRecursive(OutFiles, FPaths::ProjectDir() / TEXT("Source"), TEXT("*.h"));
	MonolithGlobalArtifactHelpers::AppendFilesRecursive(OutFiles, FPaths::ProjectDir() / TEXT("Source"), TEXT("*.cpp"));
	MonolithGlobalArtifactHelpers::AppendFilesRecursive(OutFiles, FPaths::ProjectDir() / TEXT("Plugins"), TEXT("*.h"));
	MonolithGlobalArtifactHelpers::AppendFilesRecursive(OutFiles, FPaths::ProjectDir() / TEXT("Plugins"), TEXT("*.cpp"));
	MonolithGlobalArtifactHelpers::SortAndUniqueInputFiles(OutFiles);
}

bool FCppIndexer::BuildPayload(MonolithSimpleArtifactSerialization::FCppSymbolPayload& OutPayload)
{
	OutPayload = MonolithSimpleArtifactSerialization::FCppSymbolPayload();

	TArray<MonolithGlobalArtifactHelpers::FInputFile> InputFiles;
	CollectSourceFiles(InputFiles);
	for (const MonolithGlobalArtifactHelpers::FInputFile& InputFile : InputFiles)
	{
		if (!ParseSourceFile(InputFile, OutPayload))
		{
			return false;
		}
	}

	OutPayload.Symbols.Sort([](
		const MonolithSimpleArtifactSerialization::FCppSymbolPayloadEntry& A,
		const MonolithSimpleArtifactSerialization::FCppSymbolPayloadEntry& B)
	{
		if (A.FilePath != B.FilePath)
		{
			return A.FilePath < B.FilePath;
		}
		if (A.SymbolName != B.SymbolName)
		{
			return A.SymbolName < B.SymbolName;
		}
		if (A.SymbolType != B.SymbolType)
		{
			return A.SymbolType < B.SymbolType;
		}
		return A.LineNumber < B.LineNumber;
	});

	UE_LOG(
		LogMonolithIndex,
		Log,
		TEXT("CppIndexer: prepared %d symbols from %d source files"),
		OutPayload.Symbols.Num(),
		InputFiles.Num());
	return true;
}

bool FCppIndexer::ParseSourceFile(
	const MonolithGlobalArtifactHelpers::FInputFile& InputFile,
	MonolithSimpleArtifactSerialization::FCppSymbolPayload& OutPayload)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *InputFile.AbsolutePath))
	{
		return false;
	}

	FString CurrentClass;
	static const FRegexPattern ClassPattern(TEXT("class\\s+(?:[A-Z_]+\\s+)?([A-Za-z_][A-Za-z0-9_]*)"));
	static const FRegexPattern StructPattern(TEXT("struct\\s+(?:[A-Z_]+\\s+)?([A-Za-z_][A-Za-z0-9_]*)"));
	static const FRegexPattern EnumPattern(TEXT("enum\\s+(?:class\\s+)?([A-Za-z_][A-Za-z0-9_]*)"));
	static const FRegexPattern FuncPattern(TEXT("(?:virtual\\s+|static\\s+|FORCEINLINE\\s+)*(?:[A-Za-z_][A-Za-z0-9_<>:*&\\s]*?)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
	static const FRegexPattern PropPattern(TEXT("([A-Za-z_][A-Za-z0-9_]*)\\s*(?:=.*)?;"));
	static const FRegexPattern DelegatePattern(TEXT("DECLARE_\\w+DELEGATE\\w*\\(\\s*([A-Za-z_][A-Za-z0-9_]*)"));

	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		const FString Line = Lines[Index].TrimStartAndEnd();

		if (Line.Contains(TEXT("UCLASS(")))
		{
			for (int32 InnerIndex = Index + 1; InnerIndex < FMath::Min(Index + 5, Lines.Num()); ++InnerIndex)
			{
				const FString NextLine = Lines[InnerIndex].TrimStartAndEnd();
				FRegexMatcher Matcher(ClassPattern, NextLine);
				if (!Matcher.FindNext())
				{
					continue;
				}

				const FString SymbolName = Matcher.GetCaptureGroup(1);
				CurrentClass = SymbolName;
				OutPayload.Symbols.Add(CppIndexerInternal::MakeSymbolEntry(
					InputFile.LogicalPath,
					SymbolName,
					TEXT("Class"),
					NextLine,
					InnerIndex + 1,
					FString()));
				break;
			}
		}
		else if (Line.Contains(TEXT("USTRUCT(")))
		{
			for (int32 InnerIndex = Index + 1; InnerIndex < FMath::Min(Index + 5, Lines.Num()); ++InnerIndex)
			{
				const FString NextLine = Lines[InnerIndex].TrimStartAndEnd();
				FRegexMatcher Matcher(StructPattern, NextLine);
				if (!Matcher.FindNext())
				{
					continue;
				}

				const FString SymbolName = Matcher.GetCaptureGroup(1);
				CurrentClass = SymbolName;
				OutPayload.Symbols.Add(CppIndexerInternal::MakeSymbolEntry(
					InputFile.LogicalPath,
					SymbolName,
					TEXT("Struct"),
					NextLine,
					InnerIndex + 1,
					FString()));
				break;
			}
		}
		else if (Line.Contains(TEXT("UENUM(")))
		{
			for (int32 InnerIndex = Index + 1; InnerIndex < FMath::Min(Index + 5, Lines.Num()); ++InnerIndex)
			{
				const FString NextLine = Lines[InnerIndex].TrimStartAndEnd();
				FRegexMatcher Matcher(EnumPattern, NextLine);
				if (!Matcher.FindNext())
				{
					continue;
				}

				const FString SymbolName = Matcher.GetCaptureGroup(1);
				CurrentClass = SymbolName;
				OutPayload.Symbols.Add(CppIndexerInternal::MakeSymbolEntry(
					InputFile.LogicalPath,
					SymbolName,
					TEXT("Enum"),
					NextLine,
					InnerIndex + 1,
					FString()));
				break;
			}
		}
		else if (Line.Contains(TEXT("UFUNCTION(")))
		{
			for (int32 InnerIndex = Index + 1; InnerIndex < FMath::Min(Index + 5, Lines.Num()); ++InnerIndex)
			{
				const FString NextLine = Lines[InnerIndex].TrimStartAndEnd();
				if (NextLine.IsEmpty() || NextLine.StartsWith(TEXT("UFUNCTION")))
				{
					continue;
				}

				FRegexMatcher Matcher(FuncPattern, NextLine);
				if (Matcher.FindNext())
				{
					OutPayload.Symbols.Add(CppIndexerInternal::MakeSymbolEntry(
						InputFile.LogicalPath,
						Matcher.GetCaptureGroup(1),
						TEXT("Function"),
						NextLine,
						InnerIndex + 1,
						CurrentClass));
				}
				break;
			}
		}
		else if (Line.Contains(TEXT("UPROPERTY(")))
		{
			for (int32 InnerIndex = Index + 1; InnerIndex < FMath::Min(Index + 5, Lines.Num()); ++InnerIndex)
			{
				const FString NextLine = Lines[InnerIndex].TrimStartAndEnd();
				if (NextLine.IsEmpty() || NextLine.StartsWith(TEXT("UPROPERTY")))
				{
					continue;
				}

				FRegexMatcher Matcher(PropPattern, NextLine);
				if (Matcher.FindNext())
				{
					OutPayload.Symbols.Add(CppIndexerInternal::MakeSymbolEntry(
						InputFile.LogicalPath,
						Matcher.GetCaptureGroup(1),
						TEXT("Property"),
						NextLine,
						InnerIndex + 1,
						CurrentClass));
				}
				break;
			}
		}
		else if (Line.Contains(TEXT("DECLARE_DYNAMIC_MULTICAST_DELEGATE"))
			|| Line.Contains(TEXT("DECLARE_DYNAMIC_DELEGATE"))
			|| Line.Contains(TEXT("DECLARE_MULTICAST_DELEGATE"))
			|| Line.Contains(TEXT("DECLARE_DELEGATE")))
		{
			FRegexMatcher Matcher(DelegatePattern, Line);
			if (Matcher.FindNext())
			{
				OutPayload.Symbols.Add(CppIndexerInternal::MakeSymbolEntry(
					InputFile.LogicalPath,
					Matcher.GetCaptureGroup(1),
					TEXT("Delegate"),
					Line,
					Index + 1,
					CurrentClass));
			}
		}
	}

	return true;
}
