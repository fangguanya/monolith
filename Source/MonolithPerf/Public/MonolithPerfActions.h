#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "Misc/OutputDevice.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithPerf, Log, All);

/** Short-lived log capture used by `ExecCommandCapture` — Begin() adds self to GLog,
 *  records lines until End(). Scoped to a single console command. */
class FPerfLogScope : public FOutputDevice
{
public:
	FPerfLogScope();
	virtual ~FPerfLogScope();

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;

	void Begin();
	void End();

	struct FEntry
	{
		double Timestamp;
		FName Category;
		ELogVerbosity::Type Verbosity;
		FString Message;
	};

	TArray<FEntry> GetEntries() const;

private:
	mutable FCriticalSection Lock;
	TArray<FEntry> Entries;
	bool bActive = false;
};


/** Long-lived log ring buffer. Installed on GLog at MonolithPerf StartupModule and torn
 *  down at ShutdownModule. Backs `perf.time_between` + any handler that needs to
 *  correlate log events over a window. 10k entries max; oldest wraps. */
class FPerfLogCapture : public FOutputDevice
{
public:
	static constexpr int32 MaxEntries = 10000;

	FPerfLogCapture();
	virtual ~FPerfLogCapture();

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;

	struct FEntry
	{
		double Timestamp;
		FName Category;
		ELogVerbosity::Type Verbosity;
		FString Message;
	};

	/** Entries with `Timestamp >= SinceTs` (FPlatformTime::Seconds based). `CategoryFilter`
	 *  empty = all; non-empty = only matching categories. Up to `Limit` entries. */
	TArray<FEntry> GetEntriesSince(double SinceTs, const TArray<FName>& CategoryFilter, int32 Limit) const;

	/** Singleton accessor — module's StartupModule registers; handlers fetch via this. */
	static FPerfLogCapture* Get();

	void Install();
	void Uninstall();

private:
	mutable FCriticalSection Lock;
	TArray<FEntry> RingBuffer;
	int32 WriteIndex = 0;
	bool bWrapped = false;
	bool bInstalled = false;

	static FPerfLogCapture* SingletonInstance;
};


/** Helper: return true if a PIE game world is active. Most `perf.*` handlers that need
 *  ticking subsystems (audio/physics/anim/network/niagara/streaming/etc) gate on this. */
bool IsPerfPIEActive();


class FMonolithPerfActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// --- Tier P0: Core perf primitives (migrated from editor + new) ---
	static FMonolithActionResult HandleTimeBetween(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRunCommandlet(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleExecuteConsoleCommand(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDumpShaderCompileStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStartTrace(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStopTrace(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureStatsSession(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureCsvProfile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureMemreport(const TSharedPtr<FJsonObject>& Params);

	// --- Tier P1: New perf diagnostic APIs ---
	static FMonolithActionResult HandleGetFrameStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleProfileGPUFrame(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListStreamingTextures(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDumpMemoryByClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleActorCountByClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStatsSnapshot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetRenderingStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStreamingStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetDDCStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetWorldPartitionStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetPIEPerfSnapshot(const TSharedPtr<FJsonObject>& Params);

	// --- M3 D2 Stat system: generic primitives ---
	static FMonolithActionResult HandleStatListGroups(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatDumpframe(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatDumpave(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatDumphitches(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatDumpevents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatSlowConfigure(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatNamedEvents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatUnitPeak(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStatNone(const TSharedPtr<FJsonObject>& Params);

	// --- M3 D2 Stat system: per-domain high-level wrappers ---
	static FMonolithActionResult HandleGetCoreStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetMemoryStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetShadowStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetPhysicsStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAudioStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAnimationStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetNetworkStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetTaskStats(const TSharedPtr<FJsonObject>& Params);

	// --- M4 D3 GPU extended ---
	static FMonolithActionResult HandleProfileGPUWithDrawCalls(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCaptureGPUFrames(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGPURealtime(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGPUMemory(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetNaniteStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetLumenStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetRayTracingStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetVSMStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSubstrateStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListDrawCalls(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleProfileGPUSetSort(const TSharedPtr<FJsonObject>& Params);

	// --- M5 D10+D11 Shader/PSO/DDC extended ---
	static FMonolithActionResult HandleGetShaderCompileJobs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetPSOCacheStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetPSOPrecacheStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetShaderMemory(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetMaterialPermutationCount(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListMaterialsByCost(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDumpMaterialShaderMap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleZenHttpStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleZenHealth(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleZenListNamespaces(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDDCDump(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDDCBackendGraph(const TSharedPtr<FJsonObject>& Params);

	// --- M6 D13 Niagara ---
	static FMonolithActionResult HandleNiagaraDumpComponents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNiagaraListSystems(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNiagaraEmitterOptimInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNiagaraSimCacheStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNiagaraPerSystemCost(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNiagaraGPUCPUSplit(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNiagaraGetQualityLevel(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNiagaraDebugHUDToggle(const TSharedPtr<FJsonObject>& Params);

	// --- M7 D6+D7 Streaming extended ---
	static FMonolithActionResult HandleListWaitingTextures(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStreamingPoolSize(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetStreamingPoolStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDumpTextureStreaming(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleForceStreamAllUsed(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetVTResidency(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDumpVTPoolUsage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListStreamingLevels(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleWPCellsAtLocation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleWPListLoadedCells(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleWPListStreamingSources(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleHLODListTransitions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetHLODStats(const TSharedPtr<FJsonObject>& Params);

	// --- M8 D5 Memory extended ---
	static FMonolithActionResult HandleParseMemreportSection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDiffMemreports(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetPlatformMemory(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetPlatformMemoryConstants(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleLLMDumpContext(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleLLMListTags(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleLLMReport(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindAssetRefs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDumpClassHierarchy(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGCCollect(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGCDumpReachable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleMallocLeakSnapshot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleMallocLeakCheck(const TSharedPtr<FJsonObject>& Params);

	// --- M9 Remaining domains: D8 TaskGraph / D14 Anim / D15 Physics / D16 Audio / D17 Network ---
	static FMonolithActionResult HandleGetTaskgraphStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetThreadStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAsyncLoadingThreadState(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetParallelForStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleABPPerActorCost(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleABPDumpStates(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleListActiveMontages(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAnimSharingStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetChaosStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetChaosCollisions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetActiveRigidbodyCount(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleChaosDumpEvolution(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetActiveVoiceCount(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAudioDumpConcurrency(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAudioListActiveSounds(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleMetaSoundListActive(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetNetBandwidth(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNetRPCByClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNetListConnections(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleNetDumpReplicationGraph(const TSharedPtr<FJsonObject>& Params);

	// --- CSV Profiler extended ---
	static FMonolithActionResult HandleCsvBreadcrumb(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCsvMetadata(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCsvListCategories(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCsvSetCategory(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSummarizeCsv(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDetectCsvHitches(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCsvListMetrics(const TSharedPtr<FJsonObject>& Params);

	// --- Trace control extended ---
	static FMonolithActionResult HandlePauseTrace(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleResumeTrace(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTraceStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTraceBookmark(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTraceListChannels(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTraceSnapshot(const TSharedPtr<FJsonObject>& Params);

	// --- Asset Dependency (D9) ---
	static FMonolithActionResult HandleGetAssetDependencies(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAssetReferencers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetAssetRegistryStats(const TSharedPtr<FJsonObject>& Params);

	// --- Workflow aggregates ---
	static FMonolithActionResult HandleGetFullSnapshot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStopAllOverlays(const TSharedPtr<FJsonObject>& Params);

	// --- Material (D12) ---
	static FMonolithActionResult HandleGetMaterialStats(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetMaterialComplexity(const TSharedPtr<FJsonObject>& Params);

	// --- M10 D1 Trace analysis (in-process TraceServices) ---
	static FMonolithActionResult HandleAnalyzeTrace(const TSharedPtr<FJsonObject>& Params);

	// --- M10 D5 LLM programmatic access ---
	static FMonolithActionResult HandleLLMTagValues(const TSharedPtr<FJsonObject>& Params);

private:
	static TSharedPtr<FJsonObject> RunTraceAnalysis(const FString& TracePath, int32 TopN);
};
