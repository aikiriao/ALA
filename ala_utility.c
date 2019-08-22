#include "ala_utility.h"

#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <float.h>
#include <assert.h>

/* NLZ計算のためのテーブル */
#define UNUSED 99
static const uint32_t nlz10_table[64] = {
      32,     20,     19, UNUSED, UNUSED,     18, UNUSED,      7,
      10,     17, UNUSED, UNUSED,     14, UNUSED,      6, UNUSED,
  UNUSED,      9, UNUSED,     16, UNUSED, UNUSED,      1,     26,
  UNUSED,     13, UNUSED, UNUSED,     24,      5, UNUSED, UNUSED,
  UNUSED,     21, UNUSED,      8,     11, UNUSED,     15, UNUSED,
  UNUSED, UNUSED, UNUSED,      2,     27,      0,     25, UNUSED,
      22, UNUSED,     12, UNUSED, UNUSED,      3,     28, UNUSED,
      23, UNUSED,      4,     29, UNUSED, UNUSED,     30,     31
};
#undef UNUSED

/* 窓の適用 */
void ALAUtility_ApplyWindow(const double* window, double* data, uint32_t num_samples)
{
  uint32_t smpl;

  assert(window != NULL && data != NULL);

  for (smpl = 0; smpl < num_samples; smpl++) {
    data[smpl] *= window[smpl];
  }
}

/* サイン窓を作成 */
void ALAUtility_MakeSinWindow(double* window, uint32_t window_size)
{
  uint32_t  smpl;
  double    x;

  assert(window != NULL);

  /* 0除算対策 */
  if (window_size == 1) {
    window[0] = 1.0f;
    return;
  }

  for (smpl = 0; smpl < window_size; smpl++) {
    x = (double)smpl / (window_size - 1);
    window[smpl] = sin(ALA_PI * x);
  }
}

/* NLZ（最上位ビットから1に当たるまでのビット数）を計算する黒魔術 */
/* ハッカーのたのしみ参照 */
static uint32_t nlz10(uint32_t x)
{
  x = x | (x >> 1);
  x = x | (x >> 2);
  x = x | (x >> 4);
  x = x | (x >> 8);
  x = x & ~(x >> 16);
  x = (x << 9) - x;
  x = (x << 11) - x;
  x = (x << 14) - x;
  return nlz10_table[x >> 26];
}

/* ceil(log2(val)) を計算する */
uint32_t ALAUtility_Log2Ceil(uint32_t val)
{
  assert(val != 0);
  return 32U - nlz10(val - 1);
}

/* floor(log2(val)) を計算する */
uint32_t ALAUtility_Log2Floor(uint32_t val)
{
  return 31U - nlz10(val);
}

/* 2の冪乗数に切り上げる ハッカーのたのしみ参照 */
uint32_t ALAUtility_RoundUp2Powered(uint32_t val)
{
  val--;
  val |= val >> 1;
  val |= val >> 2;
  val |= val >> 4;
  val |= val >> 8;
  val |= val >> 16;
  return val + 1;
}

/* LR -> MS（double） */
void ALAUtility_LRtoMSDouble(double **data,
    uint32_t num_channels, uint32_t num_samples)
{
  uint32_t  smpl;
  double    mid, side;

  assert(data != NULL);
  assert(data[0] != NULL);
  assert(data[1] != NULL);
  assert(num_channels >= 2);
  ALAUTILITY_UNUSED_ARGUMENT(num_channels);

  for (smpl = 0; smpl < num_samples; smpl++) {
    mid   = (data[0][smpl] + data[1][smpl]) / 2;
    side  = data[0][smpl] - data[1][smpl];
    data[0][smpl] = mid; 
    data[1][smpl] = side;
  }
}

/* LR -> MS（int32_t） */
void ALAUtility_LRtoMSInt32(int32_t **data, 
    uint32_t num_channels, uint32_t num_samples)
{
  uint32_t  smpl;
  int32_t   mid, side;

  assert(data != NULL);
  assert(data[0] != NULL);
  assert(data[1] != NULL);
  assert(num_channels >= 2);
  ALAUTILITY_UNUSED_ARGUMENT(num_channels); 

  for (smpl = 0; smpl < num_samples; smpl++) {
    /* 注意: 除算は右シフト必須(/2ではだめ。0方向に丸められる) */
    mid   = (data[0][smpl] + data[1][smpl]) >> 1; 
    side  = data[0][smpl] - data[1][smpl];
    data[0][smpl] = mid; 
    data[1][smpl] = side;
  }
}

/* MS -> LR（int32_t） */
void ALAUtility_MStoLRInt32(int32_t **data, 
    uint32_t num_channels, uint32_t num_samples)
{
  uint32_t  smpl;
  int32_t   mid, side;

  assert(data != NULL);
  assert(data[0] != NULL);
  assert(data[1] != NULL);
  assert(num_channels >= 2);
  ALAUTILITY_UNUSED_ARGUMENT(num_channels);

  for (smpl = 0; smpl < num_samples; smpl++) {
    side  = data[1][smpl];
    mid   = (data[0][smpl] << 1) | (side & 1);
    data[0][smpl] = (mid + side) >> 1;
    data[1][smpl] = (mid - side) >> 1;
  }
}

/* round関数（C89で定義されてないため自己定義） */
double ALAUtility_Round(double d)
{
    return (d >= 0.0f) ? floor(d + 0.5f) : -floor(-d + 0.5f);
}
