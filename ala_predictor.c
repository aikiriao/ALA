#include "ala_predictor.h"
#include "ala_utility.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <assert.h>

/* 内部エラー型 */
typedef enum ALAPredictorErrorTag {
  ALA_PREDICTOR_ERROR_OK,
  ALA_PREDICTOR_ERROR_NG,
  ALA_PREDICTOR_ERROR_INVALID_ARGUMENT
} ALAPredictorError;

/* LPC計算ハンドル */
struct ALALPCCalculator {
  uint32_t  max_order;     /* 最大次数           */
  /* 内部的な計算結果は精度を担保するため全てdoubleで持つ */
  /* floatだとサンプル数を増やすと標本自己相関値の誤差に起因して出力の計算結果がnanになる */
  double*   a_vec;         /* 計算用ベクトル1    */
  double*   e_vec;         /* 計算用ベクトル2    */
  double*   u_vec;         /* 計算用ベクトル3    */
  double*   v_vec;         /* 計算用ベクトル4    */
  double*   auto_corr;     /* 標本自己相関       */
  double*   lpc_coef;      /* LPC係数ベクトル    */
  double*   parcor_coef;   /* PARCOR係数ベクトル */
};

/* 音声合成ハンドル（格子型フィルタ） */
struct ALALPCSynthesizer {
  uint32_t  max_order;            /* 最大次数     */
  int32_t*  forward_residual;     /* 前向き誤差   */
  int32_t*  backward_residual;    /* 後ろ向き誤差 */
};

/* エンファシスフィルタハンドル */
struct ALAEmphasisFilter {
  int32_t prev_int32;           /* 直前のサンプル */
};

/* LPC係数計算ハンドルの作成 */
struct ALALPCCalculator* ALALPCCalculator_Create(uint32_t max_order)
{
  struct ALALPCCalculator* lpc;

  lpc = (struct ALALPCCalculator *)malloc(sizeof(struct ALALPCCalculator));

  lpc->max_order = max_order;

  /* 計算用ベクトルの領域割当 */
  lpc->a_vec = (double *)malloc(sizeof(double) * (max_order + 2)); /* a_0, a_k+1を含めるとmax_order+2 */
  lpc->e_vec = (double *)malloc(sizeof(double) * (max_order + 2)); /* e_0, e_k+1を含めるとmax_order+2 */
  lpc->u_vec = (double *)malloc(sizeof(double) * (max_order + 2));
  lpc->v_vec = (double *)malloc(sizeof(double) * (max_order + 2));

  /* 標本自己相関の領域割当 */
  lpc->auto_corr = (double *)malloc(sizeof(double) * (max_order + 1));

  /* 係数ベクトルの領域割当 */
  lpc->lpc_coef     = (double *)malloc(sizeof(double) * (max_order + 1));
  lpc->parcor_coef  = (double *)malloc(sizeof(double) * (max_order + 1));

  return lpc;
}

/* LPC係数計算ハンドルの破棄 */
void ALALPCCalculator_Destroy(struct ALALPCCalculator* lpcc)
{
  if (lpcc != NULL) {
    free(lpcc->a_vec);
    free(lpcc->e_vec);
    free(lpcc->u_vec);
    free(lpcc->v_vec);
    free(lpcc->auto_corr);
    free(lpcc->lpc_coef);
    free(lpcc->parcor_coef);
    free(lpcc);
  }
}

/*（標本）自己相関の計算 */
static ALAPredictorError ALA_CalculateAutoCorrelation(
    const double* data, uint32_t num_samples,
    double* auto_corr, uint32_t order)
{
  uint32_t smpl, lag;

  /* 引数チェック */
  if (data == NULL || auto_corr == NULL) {
    return ALA_PREDICTOR_ERROR_INVALID_ARGUMENT;
  }

  /* （標本）自己相関の計算 */
  for (lag = 0; lag < order; lag++) {
    auto_corr[lag] = 0.0f;
    /* 係数が0以上の時のみ和を取る */
    for (smpl = lag; smpl < num_samples; smpl++) {
      auto_corr[lag] += data[smpl] * data[smpl - lag];
    }
  }

  return ALA_PREDICTOR_ERROR_OK;
}

/* Levinson-Durbin再帰計算 */
static ALAPredictorError ALA_LevinsonDurbinRecursion(
    struct ALALPCCalculator* lpc, const double* auto_corr,
    double* lpc_coef, double* parcor_coef, uint32_t order)
{
  uint32_t delay, i;
  double gamma;      /* 反射係数 */
  /* オート変数にポインタをコピー */
  double* a_vec = lpc->a_vec;
  double* e_vec = lpc->e_vec;
  double* u_vec = lpc->u_vec;
  double* v_vec = lpc->v_vec;

  /* 引数チェック */
  if (lpc == NULL || lpc_coef == NULL || auto_corr == NULL) {
    return ALA_PREDICTOR_ERROR_INVALID_ARGUMENT;
  }

  /* 
   * 0次自己相関（信号の二乗和）が小さい場合
   * => 係数は全て0として無音出力システムを予測.
   */
  if (fabs(auto_corr[0]) < FLT_EPSILON) {
    for (i = 0; i < order + 1; i++) {
      lpc_coef[i] = parcor_coef[i] = 0.0f;
    }
    return ALA_PREDICTOR_ERROR_OK;
  }

  /* 初期化 */
  for (i = 0; i < order + 2; i++) {
    a_vec[i] = u_vec[i] = v_vec[i] = 0.0f;
  }

  /* 最初のステップの係数をセット */
  a_vec[0]        = 1.0f;
  e_vec[0]        = auto_corr[0];
  a_vec[1]        = - auto_corr[1] / auto_corr[0];
  parcor_coef[0]  = 0.0f;
  parcor_coef[1]  = auto_corr[1] / e_vec[0];
  e_vec[1]        = auto_corr[0] + auto_corr[1] * a_vec[1];
  u_vec[0]        = 1.0f; u_vec[1] = 0.0f; 
  v_vec[0]        = 0.0f; v_vec[1] = 1.0f; 

  /* 再帰処理 */
  for (delay = 1; delay < order; delay++) {
    gamma = 0.0f;
    for (i = 0; i < delay + 1; i++) {
      gamma += a_vec[i] * auto_corr[delay + 1 - i];
    }
    gamma /= (-e_vec[delay]);
    e_vec[delay + 1] = (1.0f - gamma * gamma) * e_vec[delay];
    /* 誤差分散（パワー）は非負 */
    assert(e_vec[delay] >= 0.0f);

    /* u_vec, v_vecの更新 */
    for (i = 0; i < delay; i++) {
      u_vec[i + 1] = v_vec[delay - i] = a_vec[i + 1];
    }
    u_vec[0] = 1.0f; u_vec[delay+1] = 0.0f;
    v_vec[0] = 0.0f; v_vec[delay+1] = 1.0f;

    /* 係数の更新 */
    for (i = 0; i < delay + 2; i++) {
       a_vec[i] = u_vec[i] + gamma * v_vec[i];
    }
    /* PARCOR係数は反射係数の符号反転 */
    parcor_coef[delay + 1] = -gamma;
    /* PARCOR係数の絶対値は1未満（収束条件） */
    assert(fabs(gamma) < 1.0f);
  }

  /* 結果を取得 */
  memcpy(lpc_coef, a_vec, sizeof(double) * (order + 1));

  return ALA_PREDICTOR_ERROR_OK;
}

/* 係数計算の共通関数 */
static ALAPredictorError ALA_CalculateCoef(
    struct ALALPCCalculator* lpc, 
    const double* data, uint32_t num_samples, uint32_t order)
{
  /* 引数チェック */
  if (lpc == NULL) {
    return ALA_PREDICTOR_ERROR_INVALID_ARGUMENT;
  }

  /* 自己相関を計算 */
  if (ALA_CalculateAutoCorrelation(
        data, num_samples, lpc->auto_corr, order + 1) != ALA_PREDICTOR_ERROR_OK) {
    return ALA_PREDICTOR_ERROR_NG;
  }

  /* 入力サンプル数が少ないときは、係数が発散することが多数
   * => 無音データとして扱い、係数はすべて0とする */
  if (num_samples < order) {
    uint32_t ord;
    for (ord = 0; ord < order + 1; ord++) {
      lpc->lpc_coef[ord] = lpc->parcor_coef[ord] = 0.0f;
    }
    return ALA_PREDICTOR_ERROR_OK;
  }

  /* 再帰計算を実行 */
  if (ALA_LevinsonDurbinRecursion(
        lpc, lpc->auto_corr,
        lpc->lpc_coef, lpc->parcor_coef, order) != ALA_PREDICTOR_ERROR_OK) {
    return ALA_PREDICTOR_ERROR_NG;
  }

  return ALA_PREDICTOR_ERROR_OK;
}

/* Levinson-Durbin再帰計算によりPARCOR係数を求める（倍精度） */
/* 係数parcor_coefはorder+1個の配列 */
ALAPredictorApiResult ALALPCCalculator_CalculatePARCORCoefDouble(
    struct ALALPCCalculator* lpc,
    const double* data, uint32_t num_samples,
    double* parcor_coef, uint32_t order)
{
  /* 引数チェック */
  if (lpc == NULL || data == NULL || parcor_coef == NULL) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* 次数チェック */
  if (order > lpc->max_order) {
    return ALAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;
  }

  /* 係数計算 */
  if (ALA_CalculateCoef(lpc, data, num_samples, order) != ALA_PREDICTOR_ERROR_OK) {
    return ALAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION;
  }

  /* 計算成功時は結果をコピー */
  /* parcor_coef と lpc->parcor_coef が同じ場所を指しているときもあるのでmemmove */
  memmove(parcor_coef, lpc->parcor_coef, sizeof(double) * (order + 1));

  return ALAPREDICTOR_APIRESULT_OK;
}

/* LPC音声合成ハンドルの作成 */
struct ALALPCSynthesizer* ALALPCSynthesizer_Create(uint32_t max_order)
{
  uint32_t ord;
  struct ALALPCSynthesizer* lpcs;

  lpcs = (struct ALALPCSynthesizer *)malloc(sizeof(struct ALALPCSynthesizer));

  lpcs->max_order = max_order;

  /* 前向き/後ろ向き誤差の領域確保 */
  lpcs->forward_residual  = malloc(sizeof(int32_t) * (max_order + 1));
  lpcs->backward_residual = malloc(sizeof(int32_t) * (max_order + 1));

  /* 誤差をゼロ初期化 */
  for (ord = 0; ord < max_order + 1; ord++) {
    lpcs->forward_residual[ord] = lpcs->backward_residual[ord] = 0;
  }

  return lpcs;
}

/* LPC音声合成ハンドルの破棄 */
void ALALPCSynthesizer_Destroy(struct ALALPCSynthesizer* lpc)
{
  if (lpc != NULL) {
    free(lpc->forward_residual);
    free(lpc->backward_residual);
    free(lpc);
  }
}

/* PARCOR係数により予測/誤差出力（32bit整数入出力） */
ALAPredictorApiResult ALALPCSynthesizer_PredictByParcorCoefInt32(
    struct ALALPCSynthesizer* lpc,
    const int32_t* data, uint32_t num_samples,
    const int32_t* parcor_coef, uint32_t order, int32_t* residual)
{
  uint32_t      samp, ord;
  int32_t*      forward_residual;
  int32_t*      backward_residual;
  int32_t       mul_temp;
  /* 丸め誤差軽減のための加算定数 = 0.5 */
  const int32_t half = (1UL << 14); 

  /* 引数チェック */
  if (lpc == NULL || data == NULL
      || parcor_coef == NULL || residual == NULL) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* 次数チェック */
  if (order > lpc->max_order) {
    return ALAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;
  }

  /* オート変数にポインタをコピー */
  forward_residual  = lpc->forward_residual;
  backward_residual = lpc->backward_residual;

  /* 誤差計算 */
  for (samp = 0; samp < num_samples; samp++) {
    /* 格子型フィルタにデータ入力 */
    forward_residual[0] = data[samp];
    /* 前向き誤差計算 */
    for (ord = 1; ord <= order; ord++) {
      mul_temp 
        = (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * backward_residual[ord - 1] + half, 15);
      forward_residual[ord] = forward_residual[ord - 1] - mul_temp;
    }
    /* 後ろ向き誤差計算 */
    for (ord = order; ord >= 1; ord--) {
      mul_temp 
        = (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * forward_residual[ord - 1] + half, 15);
      backward_residual[ord] = backward_residual[ord - 1] - mul_temp;
    }
    /* 後ろ向き誤差計算部にデータ入力 */
    backward_residual[0] = data[samp];
    /* 残差信号 */
    residual[samp] = forward_residual[order];
  }

  return ALAPREDICTOR_APIRESULT_OK;
}

/* PARCOR係数により誤差信号から音声合成（32bit整数入出力） */
ALAPredictorApiResult ALALPCSynthesizer_SynthesizeByParcorCoefInt32(
    struct ALALPCSynthesizer* lpc,
    const int32_t* residual, uint32_t num_samples,
    const int32_t* parcor_coef, uint32_t order, int32_t* output)
{
  uint32_t      ord, samp;
  int32_t       forward_residual;   /* 合成時は記憶領域を持つ必要なし */
  int32_t*      backward_residual;
  int32_t       mul_temp;
  const int32_t half = (1UL << 14); /* 丸め誤差軽減のための加算定数 = 0.5 */

  /* 引数チェック */
  if (lpc == NULL || residual == NULL
      || parcor_coef == NULL || output == NULL) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* 次数チェック */
  if (order > lpc->max_order) {
    return ALAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;
  }

  /* オート変数にポインタをコピー */
  backward_residual = lpc->backward_residual;

  /* 格子型フィルタによる音声合成 */
  for (samp = 0; samp < num_samples; samp++) {
    /* 誤差入力 */
    forward_residual = residual[samp];
    for (ord = order; ord >= 1; ord--) {
      /* 前向き誤差計算 */
      forward_residual
        += (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * backward_residual[ord - 1] + half, 15);
      /* 後ろ向き誤差計算 */
      mul_temp
        = (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * forward_residual + half, 15);
      backward_residual[ord] = backward_residual[ord - 1] - mul_temp;
    }
    /* 合成信号 */
    output[samp] = forward_residual;
    /* 後ろ向き誤差計算部にデータ入力 */
    backward_residual[0] = forward_residual;
  }

  return ALAPREDICTOR_APIRESULT_OK;
}

/* プリエンファシス(int32, in-place) */
ALAPredictorApiResult ALAEmphasisFilter_PreEmphasisInt32(
    int32_t* data, uint32_t num_samples, int32_t coef_shift)
{
  uint32_t  smpl;
  int32_t   prev_int32, tmp_int32;
  const int32_t coef_numer = (int32_t)((1 << coef_shift) - 1);

  /* 引数チェック */
  if (data == NULL) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* フィルタ適用 */
  prev_int32 = 0;
  for (smpl = 0; smpl < num_samples; smpl++) {
    tmp_int32   = data[smpl];
    data[smpl] -= (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(prev_int32 * coef_numer, coef_shift);
    prev_int32  = tmp_int32;
  }

  return ALAPREDICTOR_APIRESULT_OK;
}

/* デエンファシス(int32, in-place) */
ALAPredictorApiResult ALAEmphasisFilter_DeEmphasisInt32(
    int32_t* data, uint32_t num_samples, int32_t coef_shift)
{
  uint32_t  smpl;
  const int32_t coef_numer = (int32_t)((1 << coef_shift) - 1);

  /* 引数チェック */
  if (data == NULL) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* フィルタ適用 */
  for (smpl = 1; smpl < num_samples; smpl++) {
    data[smpl] += (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(data[smpl - 1] * coef_numer, coef_shift);
  }

  return ALAPREDICTOR_APIRESULT_OK;
}

/* プリエンファシス(double, in-place) */
ALAPredictorApiResult ALAEmphasisFilter_PreEmphasisDouble(
    double* data, uint32_t num_samples, int32_t coef_shift)
{
  uint32_t  smpl;
  double    prev, tmp;
  double    coef;

  /* 引数チェック */
  if (data == NULL) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* フィルタ係数の計算 */
  coef = (pow(2.0f, (double)coef_shift) - 1.0f) * pow(2.0f, (double)-coef_shift);

  /* フィルタ適用 */
  prev = 0.0f;
  for (smpl = 0; smpl < num_samples; smpl++) {
    tmp         = data[smpl];
    data[smpl] -= prev * coef;
    prev        = tmp;
  }

  return ALAPREDICTOR_APIRESULT_OK;
}

/* LR -> MS（double） */
ALAPredictorApiResult ALAChannelDecorrelator_LRtoMSDouble(double **data,
    uint32_t num_channels, uint32_t num_samples)
{
  uint32_t  smpl;
  double    mid, side;

  /* 引数チェック */
  if ((data != NULL)
      || (data[0] != NULL) || (data[1] != NULL)
      || (num_channels < 2)) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* サンプル単位でLR -> MS処理 */
  for (smpl = 0; smpl < num_samples; smpl++) {
    mid   = (data[0][smpl] + data[1][smpl]) / 2;
    side  = data[0][smpl] - data[1][smpl];
    data[0][smpl] = mid; 
    data[1][smpl] = side;
  }

  return ALAPREDICTOR_APIRESULT_OK;
}

/* LR -> MS（int32_t） */
ALAPredictorApiResult ALAChannelDecorrelator_LRtoMSInt32(int32_t **data, 
    uint32_t num_channels, uint32_t num_samples)
{
  uint32_t  smpl;
  int32_t   mid, side;

  /* 引数チェック */
  if ((data != NULL)
      || (data[0] != NULL) || (data[1] != NULL)
      || (num_channels < 2)) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* サンプル単位でLR -> MS処理 */
  for (smpl = 0; smpl < num_samples; smpl++) {
    /* 注意: 除算は右シフト必須(/2ではだめ。0方向に丸められる) */
    mid   = (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(data[0][smpl] + data[1][smpl], 1); 
    side  = data[0][smpl] - data[1][smpl];
    data[0][smpl] = mid; 
    data[1][smpl] = side;
  }

  return ALAPREDICTOR_APIRESULT_OK;
}

/* MS -> LR（int32_t） */
ALAPredictorApiResult ALAChannelDecorrelator_MStoLRInt32(int32_t **data, 
    uint32_t num_channels, uint32_t num_samples)
{
  uint32_t  smpl;
  int32_t   mid, side;

  /* 引数チェック */
  if ((data != NULL)
      || (data[0] != NULL) || (data[1] != NULL)
      || (num_channels < 2)) {
    return ALAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
  }

  /* サンプル単位でMS -> LR処理 */
  for (smpl = 0; smpl < num_samples; smpl++) {
    side  = data[1][smpl];
    mid   = (data[0][smpl] << 1) | (side & 1);
    data[0][smpl] = (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(mid + side, 1);
    data[1][smpl] = (int32_t)ALAUTILITY_SHIFT_RIGHT_ARITHMETIC(mid - side, 1);
  }

  return ALAPREDICTOR_APIRESULT_OK;
}
