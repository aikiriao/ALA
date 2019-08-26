#ifndef ALACODER_H_INCLUDED
#define ALACODER_H_INCLUDED

#include "bit_stream.h"
#include <stdint.h>

/* 符号化ハンドル */
struct ALACoder;

/* API結果型 */
typedef enum ALACoderApiResultTag {
  ALACODER_APIRESULT_OK,                /* OK */
  ALACODER_APIRESULT_NG,                /* 分類不能なエラー */
  ALACODER_APIRESULT_INVALID_ARGUMENT   /* 不正な引数 */
} ALACoderApiResult;

#ifdef __cplusplus
extern "C" {
#endif 

/* 符号化ハンドルの作成 */
struct ALACoder* ALACoder_Create(uint32_t max_num_channels);

/* 符号化ハンドルの破棄 */
void ALACoder_Destroy(struct ALACoder* coder);

/* 符号付き整数配列の符号化 */
ALACoderApiResult ALACoder_PutDataArray(
    struct ALACoder* coder, struct BitStream* strm,
    const int32_t** data, uint32_t num_channels, uint32_t num_samples);

/* 符号付き整数配列の復号 */
ALACoderApiResult ALACoder_GetDataArray(
    struct ALACoder* coder, struct BitStream* strm,
    int32_t** data, uint32_t num_channels, uint32_t num_samples);

#ifdef __cplusplus
}
#endif 

#endif /* ALACODER_H_INCLUDED */
