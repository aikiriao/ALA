#include "ala_coder.h"
#include "ala_utility.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

/* 固定小数の小数部ビット数 */
#define ALACODER_NUM_FRACTION_PART_BITS         8
/* 固定小数の0.5 */
#define ALACODER_FIXED_FLOAT_0_5                (1UL << ((ALACODER_NUM_FRACTION_PART_BITS) - 1))
/* 符号なし整数を固定小数に変換 */
#define ALACODER_UINT32_TO_FIXED_FLOAT(u32)     ((u32) << (ALACODER_NUM_FRACTION_PART_BITS))
/* 固定小数を符号なし整数に変換 */
#define ALACODER_FIXED_FLOAT_TO_UINT32(fixed)   (uint32_t)(((fixed) + (ALACODER_FIXED_FLOAT_0_5)) >> (ALACODER_NUM_FRACTION_PART_BITS))
/* 推定平均値の更新マクロ（指数平滑平均により推定平均値を更新） */
#define ALACODER_UPDATE_ESTIMATED_MEAN(mean, uint) {\
  (mean) = (ALACoderFixedFloat)(119 * (mean) + 9 * ALACODER_UINT32_TO_FIXED_FLOAT(uint) + (1UL << 6)) >> 7; \
}
/* Rice符号のパラメータ計算 2 ** ceil(log2(E(x)/2)) = E(x)/2の2の冪乗切り上げ */
#define ALACODER_CALCULATE_RICE_PARAMETER(mean) \
  ALAUtility_RoundUp2Powered(ALAUTILITY_MAX(ALACODER_FIXED_FLOAT_TO_UINT32((mean) >> 1), 1UL))

/* 固定小数点型 */
typedef uint64_t ALACoderFixedFloat;

/* 符号化/復号ハンドル */
struct ALACoder {
  ALACoderFixedFloat* estimated_mean;
  uint32_t            max_num_channels;
};

/* ライス符号の出力 */
static void ALACoder_PutRiceCode(
    struct BitStream* strm, uint32_t rice_parameter, uint32_t val)
{
  uint32_t i, quot, rest;

  assert(strm != NULL);

  /* 商と剰余の計算 */
  quot = val >> ALAUtility_Log2Ceil(rice_parameter);
  rest = val & (rice_parameter - 1);

  /* 商部分の出力 */
  for (i = 0; i < quot; i++) {
    BitStream_PutBit(strm, 0);
  }
  BitStream_PutBit(strm, 1);

  /* 剰余部分の出力 */
  BitStream_PutBits(strm, ALAUtility_Log2Ceil(rice_parameter), rest);
}

/* ライス符号の取得 */
static uint32_t ALACoder_GetRiceCode(
    struct BitStream* strm, uint32_t rice_parameter)
{
  uint32_t  quot, rest;
  uint8_t   bit;

  assert(strm != NULL);
  
  /* 商部分を取得 */
  quot = 0;
  BitStream_GetBit(strm, &bit);
  while (bit == 0) {
    quot++;
    BitStream_GetBit(strm, &bit);
  }

  /* 剰余部分を取得 */
  if (rice_parameter == 1) {
    /* 1の剰余は0 */
    rest = 0;
  } else {
    /* ライス符号の剰余部分取得 */
    uint64_t bitsbuf;
    BitStream_GetBits(strm, ALAUtility_Log2Ceil(rice_parameter), &bitsbuf);
    rest = (uint32_t)bitsbuf;
  }

  return (rice_parameter * quot + rest);
}

/* 符号化ハンドルの作成 */
struct ALACoder* ALACoder_Create(uint32_t max_num_channels)
{
  struct ALACoder* coder;
  
  coder = (struct ALACoder *)malloc(sizeof(struct ALACoder));
  coder->max_num_channels   = max_num_channels;

  coder->estimated_mean
    = (ALACoderFixedFloat *)malloc(sizeof(ALACoderFixedFloat) * max_num_channels);

  return coder;
}

/* 符号化ハンドルの破棄 */
void ALACoder_Destroy(struct ALACoder* coder)
{
  if (coder != NULL) {
    free(coder->estimated_mean);
    free(coder);
  }
}

/* 符号付き整数配列の符号化 */
ALACoderApiResult ALACoder_PutDataArray(
    struct ALACoder* coder, struct BitStream* strm,
    const int32_t** data, uint32_t num_channels, uint32_t num_samples)
{
  uint32_t smpl, ch, uint;

  /* 引数チェック */
  if ((strm == NULL) || (data == NULL) || (coder == NULL)) {
    return ALACODER_APIRESULT_INVALID_ARGUMENT;
  }

  /* 各チャンネルの平均値をセット/記録 */
  for (ch = 0; ch < num_channels; ch++) {
    uint64_t mean_uint = 0;
    for (smpl = 0; smpl < num_samples; smpl++) {
      mean_uint += ALAUTILITY_SINT32_TO_UINT32(data[ch][smpl]);
    }
    mean_uint /= num_samples;
    assert(mean_uint < (1UL << 16));
    BitStream_PutBits(strm, 16, mean_uint);
    coder->estimated_mean[ch] = ALACODER_UINT32_TO_FIXED_FLOAT(mean_uint);
  }

  /* 各チャンネル毎に符号化 */
  for (ch = 0; ch < num_channels; ch++) {
    for (smpl = 0; smpl < num_samples; smpl++) {
      /* 符号なし整数に変換 */
      uint = ALAUTILITY_SINT32_TO_UINT32(data[ch][smpl]);
      /* ライス符号化 */
      ALACoder_PutRiceCode(strm, ALACODER_CALCULATE_RICE_PARAMETER(coder->estimated_mean[ch]), uint);
      /* 推定平均値を更新 */
      ALACODER_UPDATE_ESTIMATED_MEAN(coder->estimated_mean[ch], uint);
    }
  }

  return ALACODER_APIRESULT_OK;
}

/* 符号付き整数配列の復号 */
ALACoderApiResult ALACoder_GetDataArray(
    struct ALACoder* coder, struct BitStream* strm,
    int32_t** data, uint32_t num_channels, uint32_t num_samples)
{
  uint32_t ch, smpl, uint;

  /* 引数チェック */
  if ((strm == NULL) || (data == NULL) || (coder == NULL)) {
    return ALACODER_APIRESULT_INVALID_ARGUMENT;
  }

  /* 平均値初期値の取得 */
  for (ch = 0; ch < num_channels; ch++) {
    uint64_t bitsbuf;
    BitStream_GetBits(strm, 16, &bitsbuf);
    coder->estimated_mean[ch] = ALACODER_UINT32_TO_FIXED_FLOAT(bitsbuf);
  }

  /* 各チャンネル毎に復号 */
  for (ch = 0; ch < num_channels; ch++) {
    for (smpl = 0; smpl < num_samples; smpl++) {
      /* ライス符号を復号 */
      uint = ALACoder_GetRiceCode(strm, ALACODER_CALCULATE_RICE_PARAMETER(coder->estimated_mean[ch]));
      /* 推定平均値を更新 */
      ALACODER_UPDATE_ESTIMATED_MEAN(coder->estimated_mean[ch], uint);
      /* 符号付き整数に変換 */
      data[ch][smpl] = ALAUTILITY_UINT32_TO_SINT32(uint);
    }
  }

  return ALACODER_APIRESULT_OK;
}
