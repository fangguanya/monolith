#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.generated.h"

/**
 * Editor subsystem that owns the engine source DB and can trigger Python re-indexing.
 */
UCLASS()
class MONOLITHSOURCE_API UMonolithSourceSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Get the source database (read-only). May be null if DB doesn't exist. */
	FMonolithSourceDatabase* GetDatabase() { return Database.IsValid() ? Database.Get() : nullptr; }

	/** Trigger Python indexer subprocess to rebuild the DB */
	void TriggerReindex();

	/** Is indexing currently running? */
	bool IsIndexing() const { return bIsIndexing; }

private:
	FString GetDatabasePath() const;
	FString GetEngineSourcePath() const;
	FString GetEngineShaderPath() const;
	FString FindPython() const;

	TUniquePtr<FMonolithSourceDatabase> Database;
	TAtomic<bool> bIsIndexing{false};
};
