#ifndef ALACODER_H_INCLUDED
#define ALACODER_H_INCLUDED

#include "bit_stream.h"
#include <stdint.h>

/* 符号化ハンドル */
struct ALACoder;

#ifdef __cplusplus
extern "C" {
#endif 

/* 符号化ハンドルの作成 */
struct ALACoder* ALACoder_Create(uint32_t max_num_channels);

/* 符号化ハンドルの破棄 */
void ALACoder_Destroy(struct ALACoder* coder);

/* 符号付き整数配列の符号化 */
void ALACoder_PutDataArray(
    struct ALACoder* coder, struct BitStream* strm,
    const int32_t** data, uint32_t num_channels, uint32_t num_samples);

/* 符号付き整数配列の復号 */
void ALACoder_GetDataArray(
    struct ALACoder* coder, struct BitStream* strm,
    int32_t** data, uint32_t num_channels, uint32_t num_samples);

#ifdef __cplusplus
}
#endif 

#endif /* ALACODER_H_INCLUDED */
