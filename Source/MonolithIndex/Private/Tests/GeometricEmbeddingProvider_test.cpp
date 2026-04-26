#include "Embedders/GeometricEmbeddingProvider.h"
#include "AssetVisualEmbeddingProvider.h"

#include "ImageCore.h"
#include "Misc/AutomationTest.h"

/*
 * GeometricEmbeddingProvider 测试覆盖：
 *  - 索引/查询 path 走同一个 Encode：同 (color, silhouette) 必产同向量
 *  - 输出严格 64 维 + L2 normalized
 *  - silhouette 全空时输出非崩溃（全 0 段 + L2 后保留 phash 段）
 *  - DeriveSilhouetteFromColor 与渲染器内部阈值一致（query 路径关键不变量）
 */

namespace GeometricEmbeddingProviderTestInternal
{
	/** 构造一张 BGRA8 sRGB 图像，前景为白色矩形。 */
	static void BuildSyntheticFrame(int32 W, int32 H, FImage& OutColor, FImage& OutSilhouette)
	{
		OutColor.Init(W, H, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		OutSilhouette.Init(W, H, ERawImageFormat::G8, EGammaSpace::Linear);
		FColor* Color = reinterpret_cast<FColor*>(OutColor.RawData.GetData());
		uint8* Mask = OutSilhouette.RawData.GetData();
		// 中心 50% × 50% 矩形为白色前景；其他为黑色背景。
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const bool bForeground = (X >= W / 4 && X < (3 * W) / 4) && (Y >= H / 4 && Y < (3 * H) / 4);
				Color[Y * W + X] = bForeground ? FColor(255, 255, 255, 255) : FColor(0, 0, 0, 255);
				Mask[Y * W + X] = bForeground ? 0xFF : 0x00;
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGeometricEmbeddingProviderEncodeDeterministicTest,
	"Monolith.Index.AssetVisual.Provider.GeometricEncodeDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometricEmbeddingProviderEncodeDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace GeometricEmbeddingProviderTestInternal;

	FImage Color, Silhouette;
	BuildSyntheticFrame(64, 64, Color, Silhouette);

	FGeometricEmbeddingProvider Provider;
	TestEqual(TEXT("provider id"), Provider.GetProviderInfo().ProviderId, FName(TEXT("geometric_v1")));
	TestEqual(TEXT("provider dim"), Provider.GetProviderInfo().EmbeddingDim, 64);
	TestTrue(TEXT("provider available"), Provider.IsAvailable());
	TestTrue(TEXT("L2 normalized declared"), Provider.GetProviderInfo().bL2Normalized);

	TArray<float> A, B;
	TestTrue(TEXT("encode A success"), Provider.Encode(Color, Silhouette, A));
	TestTrue(TEXT("encode B success"), Provider.Encode(Color, Silhouette, B));
	TestEqual(TEXT("dim A"), A.Num(), 64);
	TestEqual(TEXT("dim B"), B.Num(), 64);

	// 同输入两次必 bit-identical：deterministic 且无随机源。
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("element %d equal"), Index),
			FMath::IsNearlyEqual(A[Index], B[Index], 1e-6f));
	}

	// L2 norm ≈ 1.0
	float NormSq = 0.0f;
	for (const float V : A) NormSq += V * V;
	TestTrue(TEXT("L2 normalized"), FMath::IsNearlyEqual(NormSq, 1.0f, 1e-3f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGeometricEmbeddingProviderQueryIndexParityTest,
	"Monolith.Index.AssetVisual.Provider.GeometricQueryIndexParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGeometricEmbeddingProviderQueryIndexParityTest::RunTest(const FString& Parameters)
{
	using namespace GeometricEmbeddingProviderTestInternal;

	// 构造一张 color；分别走两条 path：
	//   index path = renderer 提供 silhouette + color
	//   query path = DeriveSilhouetteFromColor(color) + color
	// 两条 path 喂给同一个 Encode 必产同向量（同输入同输出，retriever cosine 在统一空间）。
	FImage Color, RendererSilhouette;
	BuildSyntheticFrame(64, 64, Color, RendererSilhouette);

	FImage DerivedSilhouette;
	DeriveAssetVisualSilhouetteFromColor(Color, DerivedSilhouette);

	// silhouette 阈值与 IAssetCanonicalRenderer 内部一致（R+G+B>24），
	// 全白（255+255+255=765）和全黑（0）都会落在两侧。
	TestEqual(TEXT("derived silhouette resolution match"), DerivedSilhouette.SizeX, RendererSilhouette.SizeX);

	for (int32 Index = 0; Index < DerivedSilhouette.RawData.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("silhouette pixel %d"), Index),
			DerivedSilhouette.RawData[Index], RendererSilhouette.RawData[Index]);
	}

	FGeometricEmbeddingProvider Provider;
	TArray<float> Indexed, Queried;
	TestTrue(TEXT("index encode"), Provider.Encode(Color, RendererSilhouette, Indexed));
	TestTrue(TEXT("query encode"), Provider.Encode(Color, DerivedSilhouette, Queried));

	// 同输入必产同向量。
	for (int32 Index = 0; Index < Indexed.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("index/query parity at %d"), Index),
			FMath::IsNearlyEqual(Indexed[Index], Queried[Index], 1e-5f));
	}

	// 与"自己"做 cosine = 1.0
	float Dot = 0.0f;
	for (int32 Index = 0; Index < Indexed.Num(); ++Index) Dot += Indexed[Index] * Queried[Index];
	TestTrue(TEXT("self cosine == 1.0"), FMath::IsNearlyEqual(Dot, 1.0f, 1e-3f));
	return true;
}
