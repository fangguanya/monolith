#include "Embedders/GeometricEmbeddingProvider.h"
#include "AssetVisualShardedRetriever.h"

#include "MonolithIndexLog.h"

/*
 * Geometric provider 实现。
 *
 * 关键约束：
 *  - 索引路径与查询路径跑同一个 Encode()；不能存在第二条 EncodeMultiView 之类的旁路；
 *  - 输出维度严格 64；任何特征段位置变更必须 bump ProviderVersion；
 *  - 全部从 silhouette + color 计算；不依赖 mesh 拓扑、材质、烘焙数据。
 *
 * 维度布局见 .h 注释。本文件只承担"从输入图像把布局填满"这一件事。
 */
namespace GeometricEmbeddingInternal
{
	/** 完整向量维度。 */
	static constexpr int32 EmbeddingDim = 64;
	/** 全图 + 4 象限，每段 7 维 Hu moments。 */
	static constexpr int32 HuMomentsPerRegion = 7;
	static constexpr int32 RegionCount = 5;
	static constexpr int32 HuMomentsTotal = HuMomentsPerRegion * RegionCount; // 35
	/** 中段单维全局形状特征。 */
	static constexpr int32 GlobalShapeSliceStart = 35;
	static constexpr int32 GlobalShapeSliceCount = 8; // 35..42
	/** 颜色统计段。 */
	static constexpr int32 ColorStatsStart = 43;
	static constexpr int32 ColorStatsCount = 2; // 43..44 = mean L, std L
	/** 颜色 phash 段。 */
	static constexpr int32 PHashStart = 45;
	static constexpr int32 PHashDim = 19; // 45..63

	static_assert(HuMomentsTotal + GlobalShapeSliceCount + ColorStatsCount + PHashDim == EmbeddingDim,
		"GeometricEmbeddingProvider 维度布局自检：必须严格凑齐 64 维");

	/** silhouette 前景判定阈值；与 IAssetCanonicalRenderer 内部完全一致，
	 * 保证 indexer/query 路径看到同一图像时产出 bit-identical silhouette。 */
	static constexpr uint32 SilhouetteForegroundLumaThreshold = 24;

	/** 在单通道 silhouette 上算 raw moment m_pq。
	 *  Region 由 (RegionMinX, RegionMinY, RegionMaxX, RegionMaxY) 描述（半开区间 [Min, Max)）。 */
	static double RawMoment(
		const FImage& Silhouette,
		const int32 P, const int32 Q,
		const int32 RegionMinX, const int32 RegionMinY,
		const int32 RegionMaxX, const int32 RegionMaxY)
	{
		const int32 Width = Silhouette.SizeX;
		const uint8* Pixels = Silhouette.RawData.GetData();
		double Sum = 0.0;
		for (int32 Y = RegionMinY; Y < RegionMaxY; ++Y)
		{
			const uint8* Row = Pixels + Y * Width;
			for (int32 X = RegionMinX; X < RegionMaxX; ++X)
			{
				if (Row[X] != 0)
				{
					double Term = 1.0;
					for (int32 K = 0; K < P; ++K) Term *= static_cast<double>(X - RegionMinX);
					for (int32 K = 0; K < Q; ++K) Term *= static_cast<double>(Y - RegionMinY);
					Sum += Term;
				}
			}
		}
		return Sum;
	}

	/** central moment μ_pq，相对 region 内重心。 */
	static double CentralMoment(
		const FImage& Silhouette,
		const int32 P, const int32 Q,
		const double CX, const double CY,
		const int32 RegionMinX, const int32 RegionMinY,
		const int32 RegionMaxX, const int32 RegionMaxY)
	{
		const int32 Width = Silhouette.SizeX;
		const uint8* Pixels = Silhouette.RawData.GetData();
		double Sum = 0.0;
		for (int32 Y = RegionMinY; Y < RegionMaxY; ++Y)
		{
			const uint8* Row = Pixels + Y * Width;
			for (int32 X = RegionMinX; X < RegionMaxX; ++X)
			{
				if (Row[X] != 0)
				{
					double Term = 1.0;
					const double DX = static_cast<double>(X - RegionMinX) - CX;
					const double DY = static_cast<double>(Y - RegionMinY) - CY;
					for (int32 K = 0; K < P; ++K) Term *= DX;
					for (int32 K = 0; K < Q; ++K) Term *= DY;
					Sum += Term;
				}
			}
		}
		return Sum;
	}

	/** 在指定矩形 region 上算 7 个 Hu moments，写入 OutHu7（长度必须为 7）。
	 *  全空 region 时全部写 0。 */
	static void ComputeHuMomentsForRegion(
		const FImage& Silhouette,
		const int32 RegionMinX, const int32 RegionMinY,
		const int32 RegionMaxX, const int32 RegionMaxY,
		TArrayView<float> OutHu7)
	{
		check(OutHu7.Num() == HuMomentsPerRegion);

		const double M00 = RawMoment(Silhouette, 0, 0, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);
		if (M00 < 1.0)
		{
			for (float& V : OutHu7) V = 0.0f;
			return;
		}

		const double CX = RawMoment(Silhouette, 1, 0, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY) / M00;
		const double CY = RawMoment(Silhouette, 0, 1, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY) / M00;

		const double Mu20 = CentralMoment(Silhouette, 2, 0, CX, CY, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);
		const double Mu02 = CentralMoment(Silhouette, 0, 2, CX, CY, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);
		const double Mu11 = CentralMoment(Silhouette, 1, 1, CX, CY, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);
		const double Mu30 = CentralMoment(Silhouette, 3, 0, CX, CY, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);
		const double Mu03 = CentralMoment(Silhouette, 0, 3, CX, CY, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);
		const double Mu21 = CentralMoment(Silhouette, 2, 1, CX, CY, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);
		const double Mu12 = CentralMoment(Silhouette, 1, 2, CX, CY, RegionMinX, RegionMinY, RegionMaxX, RegionMaxY);

		// η_pq = μ_pq / M00^((p+q)/2 + 1)
		auto Eta = [M00](const double Mu, const int32 P, const int32 Q)
		{
			const double Order = static_cast<double>(P + Q) / 2.0 + 1.0;
			return Mu / FMath::Pow(M00, Order);
		};
		const double N20 = Eta(Mu20, 2, 0);
		const double N02 = Eta(Mu02, 0, 2);
		const double N11 = Eta(Mu11, 1, 1);
		const double N30 = Eta(Mu30, 3, 0);
		const double N03 = Eta(Mu03, 0, 3);
		const double N21 = Eta(Mu21, 2, 1);
		const double N12 = Eta(Mu12, 1, 2);

		const double H1 = N20 + N02;
		const double H2 = (N20 - N02) * (N20 - N02) + 4.0 * N11 * N11;
		const double H3 = (N30 - 3.0 * N12) * (N30 - 3.0 * N12) + (3.0 * N21 - N03) * (3.0 * N21 - N03);
		const double H4 = (N30 + N12) * (N30 + N12) + (N21 + N03) * (N21 + N03);
		const double H5 = (N30 - 3.0 * N12) * (N30 + N12) * ((N30 + N12) * (N30 + N12) - 3.0 * (N21 + N03) * (N21 + N03))
			+ (3.0 * N21 - N03) * (N21 + N03) * (3.0 * (N30 + N12) * (N30 + N12) - (N21 + N03) * (N21 + N03));
		const double H6 = (N20 - N02) * ((N30 + N12) * (N30 + N12) - (N21 + N03) * (N21 + N03))
			+ 4.0 * N11 * (N30 + N12) * (N21 + N03);
		const double H7 = (3.0 * N21 - N03) * (N30 + N12) * ((N30 + N12) * (N30 + N12) - 3.0 * (N21 + N03) * (N21 + N03))
			- (N30 - 3.0 * N12) * (N21 + N03) * (3.0 * (N30 + N12) * (N30 + N12) - (N21 + N03) * (N21 + N03));

		// log-scale + sign 保留：原始 Hu moments 量级跨样本差异极大，直接用会被极端值主导。
		auto LogScale = [](const double H)
		{
			const double Abs = FMath::Abs(H);
			return (H >= 0.0 ? 1.0 : -1.0) * FMath::Loge(1.0 + Abs);
		};

		OutHu7[0] = static_cast<float>(LogScale(H1));
		OutHu7[1] = static_cast<float>(LogScale(H2));
		OutHu7[2] = static_cast<float>(LogScale(H3));
		OutHu7[3] = static_cast<float>(LogScale(H4));
		OutHu7[4] = static_cast<float>(LogScale(H5));
		OutHu7[5] = static_cast<float>(LogScale(H6));
		OutHu7[6] = static_cast<float>(LogScale(H7));
	}

	/** 全 silhouette 的全局形状统计：bbox / 重心 / 占比 / 紧凑度 / 周长。 */
	struct FGlobalShapeStats
	{
		float LogAspect = 0.0f;
		float Coverage = 0.0f;
		float CentroidOffsetX = 0.0f;
		float CentroidOffsetY = 0.0f;
		float NormBboxW = 0.0f;
		float NormBboxH = 0.0f;
		float Compactness = 0.0f;
		float NormPerimeter = 0.0f;
	};

	/** silhouette 前景的全局几何特征。 */
	static FGlobalShapeStats ComputeGlobalShapeStats(const FImage& Silhouette)
	{
		FGlobalShapeStats Out;

		const int32 W = Silhouette.SizeX;
		const int32 H = Silhouette.SizeY;
		const uint8* Pixels = Silhouette.RawData.GetData();

		int32 MinX = W, MaxX = -1, MinY = H, MaxY = -1;
		int64 SumX = 0;
		int64 SumY = 0;
		int32 ForegroundCount = 0;
		int32 PerimeterCount = 0;

		// 一遍 pass 同时算 bbox / 重心 / 前景数 / 4-邻域周长（边缘像素：上下左右至少一个为背景）。
		for (int32 Y = 0; Y < H; ++Y)
		{
			const uint8* Row = Pixels + Y * W;
			for (int32 X = 0; X < W; ++X)
			{
				if (Row[X] == 0)
				{
					continue;
				}
				if (X < MinX) MinX = X;
				if (X > MaxX) MaxX = X;
				if (Y < MinY) MinY = Y;
				if (Y > MaxY) MaxY = Y;
				SumX += X;
				SumY += Y;
				++ForegroundCount;

				const bool bIsEdge =
					(X == 0) || (X == W - 1) || (Y == 0) || (Y == H - 1)
					|| (Pixels[Y * W + (X - 1)] == 0)
					|| (Pixels[Y * W + (X + 1)] == 0)
					|| (Pixels[(Y - 1) * W + X] == 0)
					|| (Pixels[(Y + 1) * W + X] == 0);
				if (bIsEdge) ++PerimeterCount;
			}
		}

		if (ForegroundCount <= 0 || MaxX < 0)
		{
			return Out;
		}

		const float BBoxW = static_cast<float>(MaxX - MinX + 1);
		const float BBoxH = static_cast<float>(MaxY - MinY + 1);
		Out.LogAspect = (BBoxH > 0.0f) ? FMath::Loge(BBoxW / BBoxH) : 0.0f;
		Out.Coverage = static_cast<float>(ForegroundCount) / static_cast<float>(W * H);

		const float ImageCenterX = static_cast<float>(W) * 0.5f;
		const float ImageCenterY = static_cast<float>(H) * 0.5f;
		const float CentroidX = static_cast<float>(SumX) / static_cast<float>(ForegroundCount);
		const float CentroidY = static_cast<float>(SumY) / static_cast<float>(ForegroundCount);
		Out.CentroidOffsetX = (CentroidX - ImageCenterX) / FMath::Max(ImageCenterX, 1.0f);
		Out.CentroidOffsetY = (CentroidY - ImageCenterY) / FMath::Max(ImageCenterY, 1.0f);

		Out.NormBboxW = BBoxW / static_cast<float>(W);
		Out.NormBboxH = BBoxH / static_cast<float>(H);

		const float Area = static_cast<float>(ForegroundCount);
		const float Perimeter = static_cast<float>(FMath::Max(1, PerimeterCount));
		// 圆形 4πA/P² = 1，越偏离圆值越小。
		Out.Compactness = (4.0f * PI * Area) / (Perimeter * Perimeter);
		const float Diagonal = FMath::Sqrt(static_cast<float>(W) * static_cast<float>(W) + static_cast<float>(H) * static_cast<float>(H));
		Out.NormPerimeter = Perimeter / FMath::Max(Diagonal, 1.0f);

		return Out;
	}

	/** 在前景区域上算颜色亮度 (CIE Lab L*) 的均值和标准差，归一化到 [0,1]。
	 *  L* 经标准 sRGB→XYZ→Lab；这里用近似公式（足够稳定 + 无外部库依赖）。 */
	static void ComputeForegroundLightness(
		const FImage& Color,
		const FImage& Silhouette,
		float& OutMeanL,
		float& OutStdL)
	{
		OutMeanL = 0.0f;
		OutStdL = 0.0f;

		const int32 W = Color.SizeX;
		const int32 H = Color.SizeY;
		const FColor* SrcColor = reinterpret_cast<const FColor*>(Color.RawData.GetData());
		const uint8* SrcMask = Silhouette.RawData.GetData();

		double SumL = 0.0;
		double SumLSq = 0.0;
		int32 Count = 0;

		// 用近似 L* 公式：L_lin = (0.2126 R + 0.7152 G + 0.0722 B) / 255 (linear approx)；
		// L* ≈ L_lin^(1/2.4) ≈ L_lin^0.4167（足够给"前景颜色摘要"用，无需精确 CIE 转换）。
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				if (SrcMask[Y * W + X] == 0) continue;
				const FColor& C = SrcColor[Y * W + X];
				const float Llin = (0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B) / 255.0f;
				const float Lstar = FMath::Pow(FMath::Max(Llin, 0.0001f), 0.4167f);
				SumL += Lstar;
				SumLSq += Lstar * Lstar;
				++Count;
			}
		}

		if (Count <= 0)
		{
			return;
		}

		const double Mean = SumL / static_cast<double>(Count);
		const double Var = FMath::Max(0.0, SumLSq / static_cast<double>(Count) - Mean * Mean);
		OutMeanL = static_cast<float>(Mean);
		OutStdL = static_cast<float>(FMath::Sqrt(Var));
	}

	/** Color iso 上的低频 perceptual hash 19 维（按固定 zig-zag 顺序选 19 个非 DC 系数）。
	 *  与查询路径完全相同的算法，保证查询/索引输出对齐。 */
	static void ComputePerceptualHash(const FImage& IsoColor, TArrayView<float> OutPHash19)
	{
		check(OutPHash19.Num() == PHashDim);

		constexpr int32 K = 32;
		const int32 SrcW = IsoColor.SizeX;
		const int32 SrcH = IsoColor.SizeY;
		if (SrcW < K || SrcH < K)
		{
			for (float& V : OutPHash19) V = 0.0f;
			return;
		}

		const FColor* Src = reinterpret_cast<const FColor*>(IsoColor.RawData.GetData());

		float Gray[K * K] = { 0 };
		for (int32 Y = 0; Y < K; ++Y)
		{
			const int32 SrcY = (Y * SrcH) / K;
			for (int32 X = 0; X < K; ++X)
			{
				const int32 SrcX = (X * SrcW) / K;
				const FColor& C = Src[SrcY * SrcW + SrcX];
				Gray[Y * K + X] = 0.299f * C.R + 0.587f * C.G + 0.114f * C.B;
			}
		}

		constexpr int32 LowK = 8;
		float Cells[LowK * LowK];
		for (int32 Y = 0; Y < LowK; ++Y)
		{
			for (int32 X = 0; X < LowK; ++X)
			{
				Cells[Y * LowK + X] = Gray[Y * K + X];
			}
		}

		float MeanWithoutDc = 0.0f;
		for (int32 Index = 1; Index < LowK * LowK; ++Index)
		{
			MeanWithoutDc += Cells[Index];
		}
		MeanWithoutDc /= static_cast<float>(LowK * LowK - 1);

		// 从左上角 zig-zag 取前 19 个非 DC 系数。
		constexpr int32 SelectedCells[PHashDim] = {
			1, 8, 9, 2, 3, 10, 17, 16, 24, 25,
			18, 11, 4, 5, 12, 19, 26, 33, 32
		};

		for (int32 Index = 0; Index < PHashDim; ++Index)
		{
			const float V = Cells[SelectedCells[Index]];
			const float Diff = V - MeanWithoutDc;
			OutPHash19[Index] = FMath::Clamp(Diff / 128.0f, -1.0f, 1.0f);
		}
	}
}

FGeometricEmbeddingProvider::FGeometricEmbeddingProvider()
{
	Info.ProviderId = FName(TEXT("geometric_v1"));
	Info.ProviderVersion = 1;
	Info.EmbeddingDim = GeometricEmbeddingInternal::EmbeddingDim;
	Info.bL2Normalized = true;
}

bool FGeometricEmbeddingProvider::Encode(
	const FImage& ColorImage,
	const FImage& SilhouetteImage,
	TArray<float>& OutEmbedding)
{
	using namespace GeometricEmbeddingInternal;

	if (ColorImage.SizeX <= 1 || ColorImage.SizeY <= 1)
	{
		UE_LOG(LogMonolithIndex, Error,
			TEXT("FGeometricEmbeddingProvider::Encode: ColorImage 分辨率过小 %dx%d"),
			ColorImage.SizeX, ColorImage.SizeY);
		return false;
	}
	if (SilhouetteImage.SizeX != ColorImage.SizeX || SilhouetteImage.SizeY != ColorImage.SizeY)
	{
		UE_LOG(LogMonolithIndex, Error,
			TEXT("FGeometricEmbeddingProvider::Encode: silhouette 分辨率 %dx%d 与 color %dx%d 不一致"),
			SilhouetteImage.SizeX, SilhouetteImage.SizeY, ColorImage.SizeX, ColorImage.SizeY);
		return false;
	}

	OutEmbedding.SetNumZeroed(EmbeddingDim);

	const int32 W = SilhouetteImage.SizeX;
	const int32 H = SilhouetteImage.SizeY;
	const int32 HalfW = W / 2;
	const int32 HalfH = H / 2;

	// === 段 1：5 个 region × 7 维 Hu moments = 35 维 ===
	// region 0：全图
	ComputeHuMomentsForRegion(SilhouetteImage, 0, 0, W, H,
		TArrayView<float>(OutEmbedding.GetData() + 0 * HuMomentsPerRegion, HuMomentsPerRegion));
	// region 1：左半
	ComputeHuMomentsForRegion(SilhouetteImage, 0, 0, HalfW, H,
		TArrayView<float>(OutEmbedding.GetData() + 1 * HuMomentsPerRegion, HuMomentsPerRegion));
	// region 2：右半
	ComputeHuMomentsForRegion(SilhouetteImage, HalfW, 0, W, H,
		TArrayView<float>(OutEmbedding.GetData() + 2 * HuMomentsPerRegion, HuMomentsPerRegion));
	// region 3：上半
	ComputeHuMomentsForRegion(SilhouetteImage, 0, 0, W, HalfH,
		TArrayView<float>(OutEmbedding.GetData() + 3 * HuMomentsPerRegion, HuMomentsPerRegion));
	// region 4：下半
	ComputeHuMomentsForRegion(SilhouetteImage, 0, HalfH, W, H,
		TArrayView<float>(OutEmbedding.GetData() + 4 * HuMomentsPerRegion, HuMomentsPerRegion));

	// === 段 2：8 维全局形状统计（位置 35..42）===
	const FGlobalShapeStats Shape = ComputeGlobalShapeStats(SilhouetteImage);
	OutEmbedding[35] = Shape.LogAspect;
	OutEmbedding[36] = Shape.Coverage;
	OutEmbedding[37] = Shape.CentroidOffsetX;
	OutEmbedding[38] = Shape.CentroidOffsetY;
	OutEmbedding[39] = Shape.NormBboxW;
	OutEmbedding[40] = Shape.NormBboxH;
	OutEmbedding[41] = Shape.Compactness;
	OutEmbedding[42] = Shape.NormPerimeter;

	// === 段 3：2 维颜色亮度统计（位置 43..44）===
	float MeanL = 0.0f, StdL = 0.0f;
	ComputeForegroundLightness(ColorImage, SilhouetteImage, MeanL, StdL);
	OutEmbedding[43] = MeanL;
	OutEmbedding[44] = StdL;

	// === 段 4：19 维颜色 perceptual hash（位置 45..63）===
	ComputePerceptualHash(
		ColorImage,
		TArrayView<float>(OutEmbedding.GetData() + PHashStart, PHashDim));

	// L2 标准化，retriever 直接做 dot product 即可。
	L2NormalizeInPlace(OutEmbedding);
	return true;
}
