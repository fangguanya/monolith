#include "MonolithArtifactCache.h"

#include "DerivedDataCache.h"
#include "DerivedDataCacheKey.h"
#include "DerivedDataCachePolicy.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataRequestOwner.h"
#include "DerivedDataValue.h"
#include "MonolithArtifactCacheBreaker.h"
#include "Async/Async.h"
#include "Compression/CompressedBuffer.h"
#include "Containers/Queue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Memory/CompositeBuffer.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryWriter.h"

/*
 * 这个文件实现的是“把 artifact 存进 Unreal DDC”的具体流程。
 *
 * 可以把它想成一个两层仓库：
 * - 先查本地仓库；
 * - 本地没有，再看远端仓库；
 * - 新结果先同步写本地，再把远端写排进异步队列。
 *
 * 同时它还带了一个 breaker（熔断器）：
 * - 如果远端最近一直失败，就先暂时别再打扰远端；
 * - 等冷静一段时间后，再允许重试。
 *
 * 这里特意把“本地写成功”和“远端慢慢补齐”拆开，
 * 是为了满足两种看起来相反、其实都重要的语义：
 * - 编辑器主索引不能因为远端 DDC 变慢而卡住；
 * - warmup commandlet 又需要在结束前有机会把远端写尽量收完。
 */
namespace MonolithArtifactCacheInternal
{
	using namespace UE::DerivedData;

	/** 一次 cache 请求在 DDC 层实际采用的调度配置。 */
	struct FArtifactCacheRequestSettings
	{
		/** 传给 DDC owner 的优先级。 */
		EPriority Priority = EPriority::Normal;
		/** 远端 get 的硬超时；负数表示不设超时。 */
		double RemoteGetTimeoutSeconds = -1.0;
		/** 远端 put 的硬超时；负数表示不设超时。 */
		double RemotePutTimeoutSeconds = 2.0;
	};

	/** 一次 get 响应解析后的真实结果状态。 */
	enum class EArtifactLookupState : uint8
	{
		/** 确认命中了有效 artifact。 */
		Hit,
		/** 请求成功，但远端/本地没有这条数据。 */
		Miss,
		/** 请求虽然返回了 Ok，但 payload 本身不可解码。 */
		Invalid,
	};

	/** 统一承载 get 响应解析结果，避免调用方再各自复制命中判断。 */
	struct FArtifactLookupResult
	{
		EArtifactLookupState State = EArtifactLookupState::Miss;
		TOptional<FMonolithArtifact> Artifact;
	};

	/** 给 DDC 请求打上的统一名字，方便日志和剖析里认出来源。 */
	static const TCHAR* RequestName = TEXT("MonolithIndex");
	/** 非兼容的 artifact 编码格式切到 V2 bucket，避免旧缓存和新格式混读。 */
	static constexpr TCHAR CacheBucketName[] = TEXT("MonolithIndexV2");
	/** chunk 前的统一 header schema 版本。 */
	static constexpr uint8 ArtifactStorageSchemaVersion = 1;
	/** 单个 payload value 的压缩后目标上限。 */
	static constexpr uint64 MaxSingleValueCompressedBytes = 4ull * 1024ull * 1024ull;
	/** 超过这个原始尺寸的 artifact 不再进入共享缓存。 */
	static constexpr uint64 MaxSharedCacheRawPayloadBytes = 16ull * 1024ull * 1024ull;

	/** header 里记录 payload 是单 value 还是 chunked value。 */
	enum class EArtifactStorageMode : uint8
	{
		SingleValue = 1,
		Chunked = 2,
	};

	/** record header 的强类型视图，避免字段名散落在读写两边。 */
	struct FArtifactStorageHeader
	{
		uint8 StorageSchema = ArtifactStorageSchemaVersion;
		EArtifactStorageMode StorageMode = EArtifactStorageMode::SingleValue;
		uint64 RawPayloadSize = 0;
		uint64 CompressedPayloadSize = 0;
		uint32 ChunkCount = 0;
	};

	/** Build 阶段把 record 和统计字节数一起打包返回，避免调用方重复计算。 */
	struct FEncodedArtifactRecord
	{
		TOptional<FCacheRecord> Record;
		uint64 EncodedRecordBytes = 0;
		bool bOversized = false;
		bool bValid = false;
	};

	/** 用 identity 生成 DDC 的 bucket + hash 组合 key。 */
	static FCacheKey MakeCacheKey(const FMonolithArtifactIdentityV1& Identity)
	{
		FCacheKey Key;
		Key.Bucket = FCacheBucket(CacheBucketName);
		Key.Hash = HashMonolithArtifactIdentity(Identity);
		return Key;
	}

	/** chunked record 的小 header。 */
	static FValueId GetArtifactHeaderValueId()
	{
		return FValueId::FromName(TEXT("artifactheader"));
	}

	/** 单 value payload 的 value id。 */
	static FValueId GetArtifactPayloadValueId()
	{
		return FValueId::FromName(TEXT("artifactpayload"));
	}

	/** chunk.N 用同一个 base id 再加稳定索引，避免手拼字符串。 */
	static FValueId GetArtifactChunkValueId(const int32 ChunkIndex)
	{
		return FValueId::FromName(TEXT("artifactchunk")).MakeIndexed(ChunkIndex);
	}

	/** 把调用侧的抽象请求模式映射成 DDC 真实优先级和超时。 */
	static FArtifactCacheRequestSettings BuildRequestSettings(const EMonolithArtifactCacheRequestMode RequestMode)
	{
		FArtifactCacheRequestSettings Settings;
		switch (RequestMode)
		{
		case EMonolithArtifactCacheRequestMode::Interactive:
			Settings.Priority = EPriority::Highest;
			Settings.RemoteGetTimeoutSeconds = 0.25;
			Settings.RemotePutTimeoutSeconds = 2.0;
			break;
		case EMonolithArtifactCacheRequestMode::Warmup:
			Settings.Priority = EPriority::Low;
			Settings.RemoteGetTimeoutSeconds = 1.0;
			Settings.RemotePutTimeoutSeconds = 2.0;
			break;
		case EMonolithArtifactCacheRequestMode::Background:
		default:
			Settings.Priority = EPriority::Normal;
			Settings.RemoteGetTimeoutSeconds = 1.0;
			Settings.RemotePutTimeoutSeconds = 2.0;
			break;
		}

		return Settings;
	}

	/** 把秒级 timeout 转成 FEvent::Wait 需要的毫秒整数。 */
	static uint32 ToWaitMilliseconds(const double TimeoutSeconds)
	{
		return static_cast<uint32>(FMath::Max(0, FMath::CeilToInt(TimeoutSeconds * 1000.0)));
	}

	/*
	 * DDC 的同步包装需要同时守住两件事：
	 * 1. 调用线程必须有真实的 timeout 预算；
	 * 2. 请求对象和回调状态又必须活到 DDC 自己彻底收尾为止。
	 *
	 * 所以这里把“正在飞行中的一个请求”收进共享状态里：
	 * - 正常完成时，调用线程自己 Wait 到结束；
	 * - 命中 timeout 时，调用线程只负责 Cancel 并立刻返回，
	 *   剩下的 Owner->Wait() 交给后台清理任务异步收尾。
	 *
	 * 这样既不会把 250ms / 1000ms 的预算偷偷变成“先 timeout，再无上限 Wait”，
	 * 也不会把栈上回调状态提早释放掉。
	 */
	template <typename TResponse>
	struct TMonolithDdcRequestState
	{
		explicit TMonolithDdcRequestState(TResponse&& InDefaultResponse, const EPriority InPriority)
			: Response(MoveTemp(InDefaultResponse))
			, Owner(MakeUnique<FRequestOwner>(InPriority))
			, CompletionEvent(FPlatformProcess::GetSynchEventFromPool(true))
		{
		}

		~TMonolithDdcRequestState()
		{
			if (CompletionEvent)
			{
				FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
				CompletionEvent = nullptr;
			}
		}

		FCriticalSection Mutex;
		TResponse Response;
		bool bCompleted = false;
		TUniquePtr<FRequestOwner> Owner;
		FEvent* CompletionEvent = nullptr;
	};

	/** 超时后把真正的 Wait 留给后台清理任务，避免调用线程被拖住。 */
	template <typename TResponse>
	static void ScheduleRequestCleanup(const TSharedRef<TMonolithDdcRequestState<TResponse>, ESPMode::ThreadSafe>& RequestState)
	{
		Async(EAsyncExecution::ThreadPool, [RequestState]()
		{
			if (RequestState->Owner)
			{
				RequestState->Owner->Wait();
				RequestState->Owner.Reset();
			}
		});
	}

	/*
	 * FRequestOwner 没有 timeout wait，所以这里统一补一个“按预算等待 + 超时时显式 Cancel”的小 helper。
	 *
	 * 返回 true 表示在预算内完成；
	 * 返回 false 表示命中 timeout，并且已经把后续清理移交给后台任务。
	 */
	template <typename TResponse>
	static bool WaitForRequestCompletion(
		const TSharedRef<TMonolithDdcRequestState<TResponse>, ESPMode::ThreadSafe>& RequestState,
		const double TimeoutSeconds)
	{
		bool bCompletedWithinBudget = true;
		if (RequestState->CompletionEvent)
		{
			if (TimeoutSeconds < 0.0)
			{
				RequestState->CompletionEvent->Wait();
			}
			else
			{
				bCompletedWithinBudget = RequestState->CompletionEvent->Wait(ToWaitMilliseconds(TimeoutSeconds));
			}
		}

		if (!bCompletedWithinBudget)
		{
			if (RequestState->Owner)
			{
				RequestState->Owner->Cancel();
			}

			ScheduleRequestCleanup(RequestState);
			return false;
		}

		if (RequestState->Owner)
		{
			RequestState->Owner->Wait();
			RequestState->Owner.Reset();
		}

		return true;
	}

	/** header 里的 storage mode 需要人类可读文本时统一走这里。 */
	static const TCHAR* LexToString(const EArtifactStorageMode StorageMode)
	{
		return StorageMode == EArtifactStorageMode::Chunked ? TEXT("chunked") : TEXT("single");
	}

	/** 把强类型 header 序列化成一个小的 CB 对象，作为 record 的控制面。 */
	static FSharedBuffer BuildArtifactHeaderBuffer(const FArtifactStorageHeader& Header)
	{
		FCbWriter Writer;
		Writer.BeginObject();
		Writer << "storage_schema" << Header.StorageSchema;
		Writer << "storage_mode" << static_cast<uint8>(Header.StorageMode);
		Writer << "raw_payload_size" << Header.RawPayloadSize;
		Writer << "compressed_payload_size" << Header.CompressedPayloadSize;
		Writer << "chunk_count" << Header.ChunkCount;
		Writer.EndObject();
		return Writer.Save().AsObject().GetBuffer().ToShared();
	}

	/** 读取 record header；只接受我们当前定义的唯一 schema。 */
	static bool ParseArtifactHeaderBuffer(const FSharedBuffer& HeaderBuffer, FArtifactStorageHeader& OutHeader)
	{
		if (!HeaderBuffer)
		{
			return false;
		}

		FCbField HeaderField = FCbField::MakeView(HeaderBuffer.GetData(), HeaderBuffer);
		const FCbObject HeaderObject = HeaderField.AsObject();
		if (!HeaderObject)
		{
			return false;
		}

		OutHeader.StorageSchema = HeaderObject["storage_schema"].AsUInt8(0);
		OutHeader.StorageMode = static_cast<EArtifactStorageMode>(HeaderObject["storage_mode"].AsUInt8(0));
		OutHeader.RawPayloadSize = HeaderObject["raw_payload_size"].AsUInt64(0);
		OutHeader.CompressedPayloadSize = HeaderObject["compressed_payload_size"].AsUInt64(0);
		OutHeader.ChunkCount = HeaderObject["chunk_count"].AsUInt32(0);

		return OutHeader.StorageSchema == ArtifactStorageSchemaVersion
			&& (OutHeader.StorageMode == EArtifactStorageMode::SingleValue || OutHeader.StorageMode == EArtifactStorageMode::Chunked)
			&& OutHeader.ChunkCount > 0;
	}

	/** 额外存一份可读元信息，排查 DDC 内容时更直观。 */
	static FCbObject BuildMetaObject(
		const FMonolithArtifactIdentityV1& Identity,
		const FMonolithArtifact& Artifact,
		const FArtifactStorageHeader& Header)
	{
		FCbWriter Writer;
		Writer.BeginObject();
		Writer << "meta_schema" << static_cast<uint8>(1);
		Writer << "indexer_id" << Artifact.IndexerId.ToString();
		Writer << "indexer_version" << Artifact.IndexerVersion;
		Writer << "artifact_schema_version" << Artifact.ArtifactSchemaVersion;
		Writer << "package_name" << Artifact.PackageName;
		Writer << "identity_provider" << LexToString(Identity.IdentityProvider);
		Writer << "build_ms" << static_cast<uint32>(0);
		Writer << "payload_storage_schema" << Header.StorageSchema;
		Writer << "payload_storage_mode" << LexToString(Header.StorageMode);
		Writer << "payload_chunk_count" << Header.ChunkCount;
		Writer << "payload_raw_size" << Header.RawPayloadSize;
		Writer << "payload_compressed_size" << Header.CompressedPayloadSize;
		Writer.EndObject();
		return Writer.Save().AsObject();
	}

	/** 同步等待一次 DDC Get 完成，并在需要时按预算取消远端请求。 */
	static FCacheGetResponse ExecuteGet(
		ICache& Cache,
		const FCacheKey& Key,
		const ECachePolicy Policy,
		const EPriority Priority,
		const double TimeoutSeconds)
	{
		FCacheGetRequest Request{{RequestName}, Key, Policy};
		const TSharedRef<TMonolithDdcRequestState<FCacheGetResponse>, ESPMode::ThreadSafe> RequestState =
			MakeShared<TMonolithDdcRequestState<FCacheGetResponse>, ESPMode::ThreadSafe>(
				Request.MakeResponse(EStatus::Error),
				Priority);

		Cache.Get({Request}, *RequestState->Owner,
			[RequestState](FCacheGetResponse&& InResponse)
			{
				{
					FScopeLock Lock(&RequestState->Mutex);
					RequestState->Response = MoveTemp(InResponse);
					RequestState->bCompleted = true;
				}

				if (RequestState->CompletionEvent)
				{
					RequestState->CompletionEvent->Trigger();
				}
			});

		const bool bCompletedWithinBudget = WaitForRequestCompletion(RequestState, TimeoutSeconds);
		FScopeLock Lock(&RequestState->Mutex);
		if (!RequestState->bCompleted)
		{
			RequestState->Response.Status = bCompletedWithinBudget ? EStatus::Error : EStatus::Canceled;
		}

		return MoveTemp(RequestState->Response);
	}

	/** 同步等待一次 DDC Put 完成，并在需要时按预算取消远端请求。 */
	static FCachePutResponse ExecutePut(
		ICache& Cache,
		FCacheRecord&& Record,
		const ECachePolicy Policy,
		const EPriority Priority,
		const double TimeoutSeconds)
	{
		FCachePutRequest Request{{RequestName}, MoveTemp(Record), Policy};
		const TSharedRef<TMonolithDdcRequestState<FCachePutResponse>, ESPMode::ThreadSafe> RequestState =
			MakeShared<TMonolithDdcRequestState<FCachePutResponse>, ESPMode::ThreadSafe>(
				Request.MakeResponse(EStatus::Error),
				Priority);

		Cache.Put({MoveTemp(Request)}, *RequestState->Owner,
			[RequestState](FCachePutResponse&& InResponse)
			{
				{
					FScopeLock Lock(&RequestState->Mutex);
					RequestState->Response = MoveTemp(InResponse);
					RequestState->bCompleted = true;
				}

				if (RequestState->CompletionEvent)
				{
					RequestState->CompletionEvent->Trigger();
				}
			});

		const bool bCompletedWithinBudget = WaitForRequestCompletion(RequestState, TimeoutSeconds);
		FScopeLock Lock(&RequestState->Mutex);
		if (!RequestState->bCompleted)
		{
			RequestState->Response.Status = bCompletedWithinBudget ? EStatus::Error : EStatus::Canceled;
		}

		return MoveTemp(RequestState->Response);
	}

	/*
	 * 如果 scheduler 提供了专门的 IO 线程池，就把 DDC 工作扔过去；
	 * 否则就在当前线程直接执行。
	 *
	 * 这样做是为了：
	 * - 不让 IO 操作占满默认线程池；
	 * - 也避免 game thread 意外等待一些慢远端请求。
	 */
	template <typename TResult, typename CallableType>
	static TResult ExecuteOnIoThreadPool(FQueuedThreadPool* IoThreadPool, CallableType&& Callable)
	{
		if (!IoThreadPool)
		{
			return Callable();
		}

		TFuture<TResult> Future = AsyncPool(
			*IoThreadPool,
			Forward<CallableType>(Callable),
			nullptr,
			EQueuedWorkPriority::Normal);
		Future.Wait();
		return Future.Get();
	}

	/** 把 artifact payload 压成统一的“header + payload/chunks” record。 */
	static FEncodedArtifactRecord BuildEncodedArtifactRecord(
		const FCacheKey& Key,
		const FMonolithArtifactIdentityV1& Identity,
		const FMonolithArtifact& Artifact)
	{
		FEncodedArtifactRecord Result;

		const FSharedBuffer RawPayload = Artifact.Payload.Num() > 0
			? FSharedBuffer::Clone(Artifact.Payload.GetData(), static_cast<uint64>(Artifact.Payload.Num()))
			: FSharedBuffer::Clone(FMemoryView());
		const FCompressedBuffer CompressedPayload = FCompressedBuffer::Compress(RawPayload);
		if (!CompressedPayload)
		{
			return Result;
		}

		FArtifactStorageHeader Header;
		Header.RawPayloadSize = RawPayload.GetSize();
		Header.CompressedPayloadSize = CompressedPayload.GetCompressedSize();

		if (Header.RawPayloadSize > MaxSharedCacheRawPayloadBytes)
		{
			Result.bOversized = true;
			return Result;
		}

		const FCompositeBuffer EncodedCompressedPayload = CompressedPayload.GetCompressed();
		const uint64 EncodedPayloadBytes = EncodedCompressedPayload.GetSize();
		Header.StorageMode = EncodedPayloadBytes <= MaxSingleValueCompressedBytes
			? EArtifactStorageMode::SingleValue
			: EArtifactStorageMode::Chunked;
		Header.ChunkCount = Header.StorageMode == EArtifactStorageMode::SingleValue
			? 1u
			: static_cast<uint32>((EncodedPayloadBytes + MaxSingleValueCompressedBytes - 1) / MaxSingleValueCompressedBytes);

		FCacheRecordBuilder Builder(Key);
		Builder.SetMeta(BuildMetaObject(Identity, Artifact, Header));

		const FSharedBuffer HeaderBuffer = BuildArtifactHeaderBuffer(Header);
		const FValue HeaderValue = FValue::Compress(HeaderBuffer);
		Result.EncodedRecordBytes += HeaderValue.GetData().GetCompressedSize();
		Builder.AddValue(GetArtifactHeaderValueId(), HeaderValue);

		if (Header.StorageMode == EArtifactStorageMode::SingleValue)
		{
			const FValue PayloadValue = FValue::Compress(EncodedCompressedPayload);
			Result.EncodedRecordBytes += PayloadValue.GetData().GetCompressedSize();
			Builder.AddValue(GetArtifactPayloadValueId(), PayloadValue);
		}
		else
		{
			uint64 ChunkOffset = 0;
			for (uint32 ChunkIndex = 0; ChunkIndex < Header.ChunkCount; ++ChunkIndex)
			{
				const uint64 ChunkSize = FMath::Min<uint64>(MaxSingleValueCompressedBytes, EncodedPayloadBytes - ChunkOffset);
				const FValue ChunkValue = FValue::Compress(EncodedCompressedPayload.Mid(ChunkOffset, ChunkSize));
				Result.EncodedRecordBytes += ChunkValue.GetData().GetCompressedSize();
				Builder.AddValue(GetArtifactChunkValueId(static_cast<int32>(ChunkIndex)), ChunkValue);
				ChunkOffset += ChunkSize;
			}
		}

		Result.Record = Builder.Build();
		Result.bValid = true;
		return Result;
	}

	/** 从 header + payload/chunks 里拼回完整的 payload 压缩字节串。 */
	static bool RebuildEncodedCompressedPayload(
		const FCacheRecord& Record,
		const FArtifactStorageHeader& Header,
		FCompressedBuffer& OutCompressedPayload)
	{
		if (Header.StorageMode == EArtifactStorageMode::SingleValue)
		{
			const FValueWithId& PayloadValue = Record.GetValue(GetArtifactPayloadValueId());
			if (!PayloadValue || !PayloadValue.HasData())
			{
				return false;
			}

			const FSharedBuffer EncodedCompressedPayload = PayloadValue.GetData().Decompress();
			if (!EncodedCompressedPayload)
			{
				return false;
			}

			OutCompressedPayload = FCompressedBuffer::FromCompressed(EncodedCompressedPayload);
			return !!OutCompressedPayload;
		}

		TArray<FSharedBuffer> ChunkBuffers;
		ChunkBuffers.Reserve(Header.ChunkCount);
		for (uint32 ChunkIndex = 0; ChunkIndex < Header.ChunkCount; ++ChunkIndex)
		{
			const FValueWithId& ChunkValue = Record.GetValue(GetArtifactChunkValueId(static_cast<int32>(ChunkIndex)));
			if (!ChunkValue || !ChunkValue.HasData())
			{
				return false;
			}

			const FSharedBuffer ChunkBuffer = ChunkValue.GetData().Decompress();
			if (!ChunkBuffer)
			{
				return false;
			}

			ChunkBuffers.Add(ChunkBuffer);
		}

		OutCompressedPayload = FCompressedBuffer::FromCompressed(FCompositeBuffer(MoveTemp(ChunkBuffers)));
		return !!OutCompressedPayload;
	}

	/** 把 record 解回 artifact raw payload，并验证 header 声明的尺寸。 */
	static FArtifactLookupResult DecodeArtifactFromResponse(
		const FMonolithArtifactIdentityV1& Identity,
		const FCacheGetResponse& Response)
	{
		FArtifactLookupResult Result;
		if (Response.Status != EStatus::Ok)
		{
			Result.State = EArtifactLookupState::Invalid;
			return Result;
		}

		const FValueWithId& HeaderValue = Response.Record.GetValue(GetArtifactHeaderValueId());
		if (!HeaderValue || !HeaderValue.HasData())
		{
			Result.State = EArtifactLookupState::Invalid;
			return Result;
		}

		const FSharedBuffer HeaderBuffer = HeaderValue.GetData().Decompress();
		FArtifactStorageHeader Header;
		if (!HeaderBuffer || !ParseArtifactHeaderBuffer(HeaderBuffer, Header))
		{
			Result.State = EArtifactLookupState::Invalid;
			return Result;
		}

		FCompressedBuffer CompressedPayload;
		if (!RebuildEncodedCompressedPayload(Response.Record, Header, CompressedPayload))
		{
			Result.State = EArtifactLookupState::Invalid;
			return Result;
		}

		if (CompressedPayload.GetCompressedSize() != Header.CompressedPayloadSize
			|| CompressedPayload.GetRawSize() != Header.RawPayloadSize)
		{
			Result.State = EArtifactLookupState::Invalid;
			return Result;
		}

		const FSharedBuffer RawData = CompressedPayload.Decompress();
		if (!RawData)
		{
			Result.State = EArtifactLookupState::Invalid;
			return Result;
		}

		FMonolithArtifact Artifact;
		Artifact.ArtifactSchemaVersion = Identity.ArtifactSchemaVersion;
		Artifact.IndexerId = Identity.IndexerId;
		Artifact.IndexerVersion = Identity.IndexerVersion;
		Artifact.PackageName = Identity.PackageName.ToString();
		Artifact.IdentityHash = HashMonolithArtifactIdentity(Identity);
		Artifact.Payload.Append(static_cast<const uint8*>(RawData.GetData()), static_cast<int32>(RawData.GetSize()));
		Result.State = EArtifactLookupState::Hit;
		Result.Artifact = MoveTemp(Artifact);
		return Result;
	}
}

/** 具体实现细节都放在 impl 里，外部头文件只暴露最小接口。 */
struct FMonolithDdcArtifactCache::FImpl
{
	/** 一条待发的远端写请求。 */
	struct FPendingRemotePut
	{
		explicit FPendingRemotePut(
			UE::DerivedData::FCacheRecord&& InRecord,
			const uint64 InEncodedRecordBytes,
			const UE::DerivedData::EPriority InPriority,
			const double InTimeoutSeconds)
			: Record(MoveTemp(InRecord))
			, EncodedRecordBytes(InEncodedRecordBytes)
			, Priority(InPriority)
			, TimeoutSeconds(InTimeoutSeconds)
		{
		}

		/** 已经构造好的 DDC record。 */
		UE::DerivedData::FCacheRecord Record;
		/** 这次成功写到远端后应该累计多少编码字节。 */
		uint64 EncodedRecordBytes = 0;
		/** 这次远端 put 在 DDC 层使用的真实优先级。 */
		UE::DerivedData::EPriority Priority = UE::DerivedData::EPriority::Normal;
		/** 这次远端 put 的硬超时预算。 */
		double TimeoutSeconds = 2.0;
	};

	FImpl()
		: RemoteWriteDrainEvent(FPlatformProcess::GetSynchEventFromPool(true))
	{
		// 初始状态下队列是空的，所以 drain 事件应该处于“已完成”。
		if (RemoteWriteDrainEvent)
		{
			RemoteWriteDrainEvent->Trigger();
		}
	}

	~FImpl()
	{
		if (RemoteWriteDrainEvent)
		{
			FPlatformProcess::ReturnSynchEventToPool(RemoteWriteDrainEvent);
			RemoteWriteDrainEvent = nullptr;
		}
	}

	/** 保护统计信息、breaker、队列和线程池指针。 */
	mutable FCriticalSection Mutex;
	/** 对外展示的统计快照。 */
	FMonolithArtifactCacheStats Stats;
	/** 远端失败过多时，暂时关闭远端访问。 */
	FMonolithArtifactCacheBreaker Breaker;
	/** 可选的专用 IO 线程池。 */
	FQueuedThreadPool* IoThreadPool = nullptr;
	/** 还没真正发到远端的写请求队列。 */
	TQueue<TUniquePtr<FPendingRemotePut>, EQueueMode::Mpsc> PendingRemotePuts;
	/** 当前是否还允许继续排新的远端写。 */
	bool bAcceptRemoteWrites = true;
	/** 远端写 worker 是否已经被调度出去了。 */
	bool bRemoteWorkerScheduled = false;
	/** 队列里还剩多少待发请求。 */
	int32 PendingRemoteWriteCount = 0;
	/** 当前真的有多少请求正在执行远端写。 */
	int32 InFlightRemoteWriteCount = 0;
	/** drain 等待时复用的完成事件。 */
	FEvent* RemoteWriteDrainEvent = nullptr;
};

/** 当前是否还有没收尾的远端写工作。 */
bool FMonolithDdcArtifactCache::HasOutstandingRemoteWrites(const FImpl& InImpl)
{
	return InImpl.PendingRemoteWriteCount > 0
		|| InImpl.InFlightRemoteWriteCount > 0
		|| InImpl.bRemoteWorkerScheduled;
}

/** 把 breaker 实时状态刷新进统计。 */
void FMonolithDdcArtifactCache::RefreshBreakerSnapshot(FImpl& InImpl, const double NowSeconds)
{
	InImpl.Stats.bRemoteDisabled = InImpl.Breaker.IsOpen(NowSeconds);
	InImpl.Stats.RemoteBreakerRemainingSeconds = InImpl.Breaker.GetRemainingOpenSeconds(NowSeconds);
}

/** 把远端写队列交给后台线程真正执行。 */
void FMonolithDdcArtifactCache::ScheduleRemoteWriteWorker(const TSharedPtr<FImpl, ESPMode::ThreadSafe>& InImpl)
{
	if (!InImpl.IsValid())
	{
		return;
	}

	auto WorkerBody = [InImpl]()
	{
		using namespace MonolithArtifactCacheInternal;
		using namespace UE::DerivedData;

		while (true)
		{
			TUniquePtr<FImpl::FPendingRemotePut> PendingWrite;
			{
				FScopeLock Lock(&InImpl->Mutex);
				if (!InImpl->bAcceptRemoteWrites)
				{
					// 退出阶段要求丢弃待发远端写时，这里统一把队列清空。
					while (InImpl->PendingRemotePuts.Dequeue(PendingWrite))
					{
						--InImpl->PendingRemoteWriteCount;
					}
				}

				if (!InImpl->PendingRemotePuts.Dequeue(PendingWrite))
				{
					InImpl->bRemoteWorkerScheduled = false;
					if (!HasOutstandingRemoteWrites(*InImpl) && InImpl->RemoteWriteDrainEvent)
					{
						InImpl->RemoteWriteDrainEvent->Trigger();
					}
					return;
				}

				--InImpl->PendingRemoteWriteCount;
				++InImpl->InFlightRemoteWriteCount;
			}

			ICache* Cache = TryGetCache();
			const double NowSeconds = FPlatformTime::Seconds();
			const bool bRemotePutOk = Cache
				&& ExecutePut(
					*Cache,
					MoveTemp(PendingWrite->Record),
					ECachePolicy::StoreRemote,
					PendingWrite->Priority,
					PendingWrite->TimeoutSeconds).Status == EStatus::Ok;

			FScopeLock Lock(&InImpl->Mutex);
			--InImpl->InFlightRemoteWriteCount;

			if (bRemotePutOk)
			{
				InImpl->Breaker.RecordRemotePutSuccess(NowSeconds);
				++InImpl->Stats.RemoteWriteOkCount;
				InImpl->Stats.RemoteWriteBytes += PendingWrite->EncodedRecordBytes;
			}
			else
			{
				InImpl->Breaker.RecordFailure(NowSeconds);
				++InImpl->Stats.RemoteWriteFailCount;
			}

			RefreshBreakerSnapshot(*InImpl, NowSeconds);
			if (!HasOutstandingRemoteWrites(*InImpl) && InImpl->RemoteWriteDrainEvent)
			{
				InImpl->RemoteWriteDrainEvent->Trigger();
			}
		}
	};

	FQueuedThreadPool* IoThreadPool = nullptr;
	{
		FScopeLock Lock(&InImpl->Mutex);
		IoThreadPool = InImpl->IoThreadPool;
	}

	// 优先复用 scheduler 的 IoDdcPool；如果当前调用方还没提供，就退回默认线程池。
	if (IoThreadPool)
	{
		AsyncPool(*IoThreadPool, MoveTemp(WorkerBody), nullptr, EQueuedWorkPriority::Low);
	}
	else
	{
		Async(EAsyncExecution::ThreadPool, MoveTemp(WorkerBody));
	}
}

FMonolithDdcArtifactCache::FMonolithDdcArtifactCache()
	: Impl(MakeShared<FImpl, ESPMode::ThreadSafe>())
{
}

FMonolithDdcArtifactCache::~FMonolithDdcArtifactCache()
{
	// 析构前先把还没发出去的远端写请求丢掉，避免对象销毁后还有人继续排队。
	DiscardPendingRemoteWrites();
}

TOptional<FMonolithArtifact> FMonolithDdcArtifactCache::Get(
	const FMonolithArtifactIdentityV1& Identity,
	const EMonolithArtifactCacheRequestMode RequestMode)
{
	using namespace MonolithArtifactCacheInternal;
	using namespace UE::DerivedData;

	const TSharedPtr<FImpl, ESPMode::ThreadSafe> LocalImpl = Impl;
	if (!LocalImpl.IsValid())
	{
		return {};
	}

	// DDC 不可用时直接返回空，让上层走正常重建路径。
	ICache* Cache = TryGetCache();
	if (!Cache)
	{
		return {};
	}

	const double NowSeconds = FPlatformTime::Seconds();
	const FCacheKey Key = MakeCacheKey(Identity);
	const FArtifactCacheRequestSettings RequestSettings = BuildRequestSettings(RequestMode);

	FQueuedThreadPool* IoThreadPool = nullptr;
	{
		FScopeLock Lock(&LocalImpl->Mutex);
		IoThreadPool = LocalImpl->IoThreadPool;
	}

	// 第一站永远先查本地：最快，也最便宜。
	const FCacheGetResponse LocalResponse = ExecuteOnIoThreadPool<FCacheGetResponse>(
		IoThreadPool,
		[Cache, Key, RequestSettings]()
		{
			return ExecuteGet(*Cache, Key, ECachePolicy::QueryLocal, RequestSettings.Priority, -1.0);
		});
	const FArtifactLookupResult LocalLookup = DecodeArtifactFromResponse(Identity, LocalResponse);
	if (LocalLookup.State == EArtifactLookupState::Hit && LocalLookup.Artifact.IsSet())
	{
		FScopeLock Lock(&LocalImpl->Mutex);
		++LocalImpl->Stats.LocalHitCount;
		RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
		return LocalLookup.Artifact;
	}

	// breaker 打开时，说明远端最近不稳定，这次就不再尝试远端。
	{
		FScopeLock Lock(&LocalImpl->Mutex);
		if (!LocalImpl->Breaker.AllowGet(NowSeconds))
		{
			RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
			return {};
		}

		RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
	}

	// 第二站才查远端；如果拿到了，会顺手要求 DDC 落回本地。
	const FCacheGetResponse RemoteResponse = ExecuteOnIoThreadPool<FCacheGetResponse>(
		IoThreadPool,
		[Cache, Key, RequestSettings]()
		{
			return ExecuteGet(
				*Cache,
				Key,
				ECachePolicy::QueryRemote | ECachePolicy::StoreLocal,
				RequestSettings.Priority,
				RequestSettings.RemoteGetTimeoutSeconds);
		});
	const FArtifactLookupResult RemoteLookup = RemoteResponse.Status == EStatus::Ok
		? DecodeArtifactFromResponse(Identity, RemoteResponse)
		: FArtifactLookupResult{};

	{
		FScopeLock Lock(&LocalImpl->Mutex);
		if (RemoteResponse.Status != EStatus::Ok || RemoteLookup.State == EArtifactLookupState::Invalid)
		{
			// 远端访问失败和坏数据都算真正失败，必须收紧 breaker，而不是伪装成 miss。
			LocalImpl->Breaker.RecordFailure(NowSeconds);
			RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
			return {};
		}

		// 远端能正常返回，即使只是 miss，也说明链路是健康的。
		LocalImpl->Breaker.RecordRemoteGetSuccess(NowSeconds);
		if (RemoteLookup.State == EArtifactLookupState::Hit)
		{
			++LocalImpl->Stats.RemoteHitCount;
		}
		else
		{
			++LocalImpl->Stats.RemoteMissCount;
		}
		RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
	}

	return RemoteLookup.Artifact;
}

bool FMonolithDdcArtifactCache::Put(
	const FMonolithArtifactIdentityV1& Identity,
	const FMonolithArtifact& Artifact,
	const EMonolithArtifactCacheRequestMode RequestMode)
{
	using namespace MonolithArtifactCacheInternal;
	using namespace UE::DerivedData;

	const TSharedPtr<FImpl, ESPMode::ThreadSafe> LocalImpl = Impl;
	if (!LocalImpl.IsValid())
	{
		return false;
	}

	const FCacheKey Key = MakeCacheKey(Identity);
	const FArtifactCacheRequestSettings RequestSettings = BuildRequestSettings(RequestMode);
	const FEncodedArtifactRecord EncodedRecord = BuildEncodedArtifactRecord(Key, Identity, Artifact);
	if (EncodedRecord.bOversized)
	{
		// oversized artifact 仍然允许主索引继续提交，只是不再进入任何共享缓存层。
		FScopeLock Lock(&LocalImpl->Mutex);
		++LocalImpl->Stats.OversizedArtifactCount;
		RefreshBreakerSnapshot(*LocalImpl, FPlatformTime::Seconds());
		return true;
	}
	if (!EncodedRecord.bValid)
	{
		return false;
	}

	// DDC 不可用时，普通 artifact 仍然视为缓存写失败；只有 oversized 例外，因为它本来就不进缓存。
	ICache* Cache = TryGetCache();
	if (!Cache)
	{
		return false;
	}

	FQueuedThreadPool* IoThreadPool = nullptr;
	{
		FScopeLock Lock(&LocalImpl->Mutex);
		IoThreadPool = LocalImpl->IoThreadPool;
	}

	// 本地写入失败时直接返回 false，因为至少本地仓库得先站稳。
	const FCachePutResponse LocalPut = ExecuteOnIoThreadPool<FCachePutResponse>(
		IoThreadPool,
		[Cache, LocalRecord = EncodedRecord.Record.GetValue(), RequestSettings]() mutable
		{
			return ExecutePut(*Cache, MoveTemp(LocalRecord), ECachePolicy::StoreLocal, RequestSettings.Priority, -1.0);
		});
	if (LocalPut.Status != EStatus::Ok)
	{
		return false;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	bool bShouldScheduleRemoteWorker = false;
	{
		FScopeLock Lock(&LocalImpl->Mutex);
		if (!LocalImpl->bAcceptRemoteWrites)
		{
			// 退出路径已经要求“不要再发远端写”时，本地成功就足够了。
			RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
			return true;
		}

		if (!LocalImpl->Breaker.AllowPut(NowSeconds))
		{
			// breaker 打开时不再同步阻塞远端，只保留本地成果。
			RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
			return true;
		}

		FCacheRecord RemoteRecord = EncodedRecord.Record.GetValue();
		TUniquePtr<FImpl::FPendingRemotePut> PendingRemotePut = MakeUnique<FImpl::FPendingRemotePut>(
			MoveTemp(RemoteRecord),
			EncodedRecord.EncodedRecordBytes,
			RequestSettings.Priority,
			RequestSettings.RemotePutTimeoutSeconds);

		LocalImpl->PendingRemotePuts.Enqueue(MoveTemp(PendingRemotePut));
		++LocalImpl->PendingRemoteWriteCount;
		if (LocalImpl->RemoteWriteDrainEvent)
		{
			LocalImpl->RemoteWriteDrainEvent->Reset();
		}
		if (!LocalImpl->bRemoteWorkerScheduled)
		{
			LocalImpl->bRemoteWorkerScheduled = true;
			bShouldScheduleRemoteWorker = true;
		}
	}

	if (bShouldScheduleRemoteWorker)
	{
		ScheduleRemoteWriteWorker(LocalImpl);
	}

	return true;
}

bool FMonolithDdcArtifactCache::DrainRemoteWrites(const double TimeoutSeconds)
{
	const TSharedPtr<FImpl, ESPMode::ThreadSafe> LocalImpl = Impl;
	if (!LocalImpl.IsValid())
	{
		return true;
	}

	FEvent* DrainEvent = nullptr;
	{
		FScopeLock Lock(&LocalImpl->Mutex);
		if (!HasOutstandingRemoteWrites(*LocalImpl))
		{
			return true;
		}

		DrainEvent = LocalImpl->RemoteWriteDrainEvent;
	}

	if (!DrainEvent)
	{
		return true;
	}

	if (TimeoutSeconds < 0.0)
	{
		DrainEvent->Wait();
		return true;
	}

	return DrainEvent->Wait(static_cast<uint32>(FMath::Max(0.0, TimeoutSeconds) * 1000.0));
}

void FMonolithDdcArtifactCache::DiscardPendingRemoteWrites()
{
	const TSharedPtr<FImpl, ESPMode::ThreadSafe> LocalImpl = Impl;
	if (!LocalImpl.IsValid())
	{
		return;
	}

	FScopeLock Lock(&LocalImpl->Mutex);
	LocalImpl->bAcceptRemoteWrites = false;

	TUniquePtr<FImpl::FPendingRemotePut> PendingRemotePut;
	while (LocalImpl->PendingRemotePuts.Dequeue(PendingRemotePut))
	{
		--LocalImpl->PendingRemoteWriteCount;
	}

	if (!HasOutstandingRemoteWrites(*LocalImpl) && LocalImpl->RemoteWriteDrainEvent)
	{
		LocalImpl->RemoteWriteDrainEvent->Trigger();
	}
}

FMonolithArtifactCacheStats FMonolithDdcArtifactCache::GetStats() const
{
	const TSharedPtr<FImpl, ESPMode::ThreadSafe> LocalImpl = Impl;
	if (!LocalImpl.IsValid())
	{
		return FMonolithArtifactCacheStats();
	}

	// 每次读 stats 时顺手刷新一下 breaker 的实时状态，避免 UI 看见旧值。
	const double NowSeconds = FPlatformTime::Seconds();
	FScopeLock Lock(&LocalImpl->Mutex);
	RefreshBreakerSnapshot(*LocalImpl, NowSeconds);
	FMonolithArtifactCacheStats Snapshot = LocalImpl->Stats;
	Snapshot.PendingRemoteWriteCount = LocalImpl->PendingRemoteWriteCount;
	Snapshot.InFlightRemoteWriteCount = LocalImpl->InFlightRemoteWriteCount;
	return Snapshot;
}

void FMonolithDdcArtifactCache::ResetStats()
{
	const TSharedPtr<FImpl, ESPMode::ThreadSafe> LocalImpl = Impl;
	if (!LocalImpl.IsValid())
	{
		return;
	}

	// 这里只清统计，不动 breaker 状态和真正缓存内容。
	FScopeLock Lock(&LocalImpl->Mutex);
	LocalImpl->Stats = FMonolithArtifactCacheStats();
	RefreshBreakerSnapshot(*LocalImpl, FPlatformTime::Seconds());
}

void FMonolithDdcArtifactCache::SetIoThreadPool(FQueuedThreadPool* InIoThreadPool)
{
	const TSharedPtr<FImpl, ESPMode::ThreadSafe> LocalImpl = Impl;
	if (!LocalImpl.IsValid())
	{
		return;
	}

	// scheduler 初始化完成后，会把自己的 IO 池塞进来。
	FScopeLock Lock(&LocalImpl->Mutex);
	LocalImpl->IoThreadPool = InIoThreadPool;
}
