#ifndef ALAPREDICTOR_H_INCLUDED
#define ALAPREDICTOR_H_INCLUDED

#include <stdint.h>

/* LPC係数計算ハンドル */
struct ALALPCCalculator;

/* LPC音声合成ハンドル */
struct ALALPCSynthesizer;

/* エンファシスフィルタハンドル */
struct ALAEmphasisFilter;

/* API結果型 */
typedef enum ALAPredictorApiResultTag {
  ALAPREDICTOR_APIRESULT_OK,                     /* OK */
  ALAPREDICTOR_APIRESULT_NG,                     /* 分類不能なエラー */
  ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT,       /* 不正な引数 */
  ALAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER,       /* 最大次数を超えた */
  ALAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION   /* 計算に失敗 */
} ALAPredictorApiResult;

#ifdef __cplusplus
extern "C" {
#endif

/* LPC係数計算ハンドルの作成 */
struct ALALPCCalculator* ALALPCCalculator_Create(uint32_t max_order);

/* LPC係数計算ハンドルの破棄 */
void ALALPCCalculator_Destroy(struct ALALPCCalculator* lpc);

/* Levinson-Durbin再帰計算によりPARCOR係数を求める（倍精度） */
/* 係数parcor_coefはorder+1個の配列 */
ALAPredictorApiResult ALALPCCalculator_CalculatePARCORCoefDouble(
    struct ALALPCCalculator* lpcc,
    const double* data, uint32_t num_samples,
    double* parcor_coef, uint32_t order);

/* LPC音声合成ハンドルの作成 */
struct ALALPCSynthesizer* ALALPCSynthesizer_Create(uint32_t max_order);

/* LPC音声合成ハンドルの破棄 */
void ALALPCSynthesizer_Destroy(struct ALALPCSynthesizer* lpc);

/* PARCOR係数により予測/誤差出力（32bit整数入出力） */
/* 係数parcor_coefはorder+1個の配列 */
ALAPredictorApiResult ALALPCSynthesizer_PredictByParcorCoefInt32(
    struct ALALPCSynthesizer* lpcs,
    const int32_t* data, uint32_t num_samples,
    const int32_t* parcor_coef, uint32_t order,
    int32_t* residual);

/* PARCOR係数により誤差信号から音声合成（32bit整数入出力） */
/* 係数parcor_coefはorder+1個の配列 */
ALAPredictorApiResult ALALPCSynthesizer_SynthesizeByParcorCoefInt32(
    struct ALALPCSynthesizer* lpcs,
    const int32_t* residual, uint32_t num_samples,
    const int32_t* parcor_coef, uint32_t order,
    int32_t* output);

/* エンファシスフィルタの作成 */
struct ALAEmphasisFilter* ALAEmphasisFilter_Create(void);

/* エンファシスフィルタのリセット */
ALAPredictorApiResult ALAEmphasisFilter_Reset(struct ALAEmphasisFilter* emp);

/* エンファシスフィルタの破棄 */
void ALAEmphasisFilter_Destroy(struct ALAEmphasisFilter* emp);

/* プリエンファシス(int32) */
ALAPredictorApiResult ALAEmphasisFilter_PreEmphasisInt32(
    struct ALAEmphasisFilter* emp,
    int32_t* data, uint32_t num_samples, int32_t coef_shift);

/* デエンファシス(int32) */
ALAPredictorApiResult ALAEmphasisFilter_DeEmphasisInt32(
    struct ALAEmphasisFilter* emp,
    int32_t* data, uint32_t num_samples, int32_t coef_shift);

/* プリエンファシス(double, 解析用) */
void ALAEmphasisFilter_PreEmphasisDouble(double* data, uint32_t num_samples, int32_t coef_shift);

#ifdef __cplusplus
}
#endif

#endif /* ALAPREDICTOR_H_INCLUDED */
