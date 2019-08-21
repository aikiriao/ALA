#include "bit_stream.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* アラインメント */
#define BITSTREAM_ALIGNMENT                   16
/* 読みモードか？（0で書きモード） */
#define BITSTREAM_FLAGS_FILEOPENMODE_READ     (1 << 0)
/* メモリはワーク渡しか？（1:ワーク渡し, 0:mallocで自前確保） */
#define BITSTREAM_FLAGS_MEMORYALLOC_BYWORK    (1 << 1)

/* 下位n_bitsを取得 */
#define BITSTREAM_GETLOWERBITS(n_bits, val) ((val) & st_lowerbits_mask[(n_bits)])

/* ビットストリーム構造体 */
struct BitStream {
  FILE*      fp;          /* ファイルポインタ           */
  uint8_t    flags;       /* 内部状態フラグ             */
  uint8_t    bit_buffer;  /* 内部ビット入出力バッファ   */
  uint32_t   bit_count;   /* 内部ビット入出力カウント   */
  void*      work_ptr;    /* ワーク領域先頭ポインタ     */
};

/* 下位ビットを取り出すマスク 32bitまで */
static const uint32_t st_lowerbits_mask[33] = {
  0x00000000UL,
  0x00000001UL, 0x00000003UL, 0x00000007UL, 0x0000000FUL,
  0x0000001FUL, 0x0000003FUL, 0x0000007FUL, 0x000000FFUL,
  0x000001FFUL, 0x000003FFUL, 0x000007FFUL, 0x00000FFFUL,
  0x00001FFFUL, 0x00003FFFUL, 0x00007FFFUL, 0x0000FFFFUL,
  0x0001FFFFUL, 0x0003FFFFUL, 0x0007FFFFUL, 0x000FFFFFUL, 
  0x001FFFFFUL, 0x300FFFFFUL, 0x007FFFFFUL, 0x00FFFFFFUL,
  0x01FFFFFFUL, 0x03FFFFFFUL, 0x07FFFFFFUL, 0x0FFFFFFFUL, 
  0x1FFFFFFFUL, 0x3FFFFFFFUL, 0x7FFFFFFFUL, 0xFFFFFFFFUL
};

/* ワークサイズの取得 */
int32_t BitStream_CalculateWorkSize(void)
{
  return (sizeof(struct BitStream) + BITSTREAM_ALIGNMENT);
}

/* ビットストリームのオープン */
struct BitStream* BitStream_Open(const char* filepath,
    const char* mode, void *work, int32_t work_size)
{
  struct BitStream*  stream;
  int8_t                is_malloc_by_work = 0;
  FILE*                 tmp_fp;
  uint8_t*              work_ptr = (uint8_t *)work;
  
  /* 引数チェック */
  if ((mode == NULL) || (work_size < 0)
      || ((work != NULL) && (work_size < BitStream_CalculateWorkSize()))) {
    return NULL;
  }

  /* ワーク渡しか否か？ */
  if ((work == NULL) && (work_size == 0)) {
    is_malloc_by_work = 0;
    work = (uint8_t *)malloc((size_t)BitStream_CalculateWorkSize());
    if (work == NULL) {
      return NULL;
    }
  } else {
    is_malloc_by_work = 1;
  }

  /* アラインメント切り上げ */
  work_ptr = (uint8_t *)(((uintptr_t)work + (BITSTREAM_ALIGNMENT-1)) & ~(uintptr_t)(BITSTREAM_ALIGNMENT-1));

  /* 構造体の配置 */
  stream            = (struct BitStream *)work_ptr;
  work_ptr          += sizeof(struct BitStream);
  stream->work_ptr  = work;
  stream->flags     = 0;

  /* モードの1文字目でオープンモードを確定
   * 内部カウンタもモードに合わせて設定 */
  switch (mode[0]) {
    case 'r':
      stream->flags     |= BITSTREAM_FLAGS_FILEOPENMODE_READ;
      stream->bit_count = 0;
      break;
    case 'w':
      stream->flags     &= (uint8_t)(~BITSTREAM_FLAGS_FILEOPENMODE_READ);
      stream->bit_count = 8;
      break;
    default:
      return NULL;
  }

  /* メモリアロケート方法を記録 */
  if (is_malloc_by_work != 0) {
    stream->flags |= BITSTREAM_FLAGS_MEMORYALLOC_BYWORK;
  }
  if (stream == NULL) {
    return NULL;
  }

  /* ファイルオープン */
  tmp_fp = fopen(filepath, mode);
  if (tmp_fp == NULL) {
    return NULL;
  }
  stream->fp = tmp_fp;

  /* 内部状態初期化 */
  fseek(stream->fp, SEEK_SET, 0);
  stream->bit_buffer  = 0;

  return stream;
}

/* ビットストリームのクローズ */
void BitStream_Close(struct BitStream* stream)
{
  /* 引数チェック */
  if (stream == NULL) {
    return;
  }

  /* バッファのクリア 返り値は無視する */
  (void)BitStream_Flush(stream);

  /* ファイルハンドルクローズ */
  fclose(stream->fp);

  /* 必要ならばメモリ解放 */
  if (!(stream->flags & BITSTREAM_FLAGS_MEMORYALLOC_BYWORK)) {
    free(stream->work_ptr);
    stream->work_ptr = NULL;
  }
}

/* シーク(fseek準拠) */
BitStreamApiResult BitStream_Seek(struct BitStream* stream, int32_t offset, int32_t wherefrom)
{
  /* 引数チェック */
  if (stream == NULL) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 内部バッファをクリア（副作用が起こる） */
  if (BitStream_Flush(stream) != BITSTREAM_APIRESULT_OK) {
    return BITSTREAM_APIRESULT_NG;
  }

  /* シーク実行 */
  if (fseek(stream->fp, offset, wherefrom) != 0) {
    return BITSTREAM_APIRESULT_NG;
  }

  return BITSTREAM_APIRESULT_OK;
}

/* 現在位置(ftell)準拠 */
BitStreamApiResult BitStream_Tell(struct BitStream* stream, int32_t* result)
{
  long tmp;

  /* 引数チェック */
  if (stream == NULL || result == NULL) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* ftell実行/結果の記録 */
  if ((tmp = ftell(stream->fp)) >= 0) {
    *result = (int32_t)tmp;
  }

  return (tmp >= 0) ? BITSTREAM_APIRESULT_OK : BITSTREAM_APIRESULT_NG;
}

/* 1bit出力 */
BitStreamApiResult BitStream_PutBit(struct BitStream* stream, uint8_t bit)
{
  /* 引数チェック */
  if (stream == NULL) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 読み込みモードでは実行不可能 */
  if (stream->flags & BITSTREAM_FLAGS_FILEOPENMODE_READ) {
    return BITSTREAM_APIRESULT_INVALID_MODE;
  }

  /* バイト出力するまでのカウントを減らす */
  stream->bit_count--;

  /* ビット出力バッファに値を格納 */
  if (bit != 0) {
    stream->bit_buffer |= (uint8_t)(1 << stream->bit_count);
  }

  /* バッファ出力・更新 */
  if (stream->bit_count == 0) {
    if (fputc(stream->bit_buffer, stream->fp) == EOF) {
      return BITSTREAM_APIRESULT_IOERROR;
    }
    stream->bit_buffer = 0;
    stream->bit_count  = 8;
  }

  return BITSTREAM_APIRESULT_OK;
}

/*
 * valの右側（下位）n_bits 出力（最大64bit出力可能）
 * BitStream_PutBits(stream, 3, 6);は次と同じ:
 * BitStream_PutBit(stream, 1); BitStream_PutBit(stream, 1); BitStream_PutBit(stream, 0); 
 */
BitStreamApiResult BitStream_PutBits(struct BitStream* stream, uint32_t n_bits, uint64_t val)
{
  /* 引数チェック */
  if (stream == NULL) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 読み込みモードでは実行不可能 */
  if (stream->flags & BITSTREAM_FLAGS_FILEOPENMODE_READ) {
    return BITSTREAM_APIRESULT_INVALID_MODE;
  }

  /* 出力可能な最大ビット数を越えている */
  if (n_bits > sizeof(uint64_t) * 8) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 0ビット出力では何もしない */
  if (n_bits == 0) {
    return BITSTREAM_APIRESULT_OK;
  }

  /* valの上位ビットから順次出力
   * 初回ループでは端数（出力に必要なビット数）分を埋め出力
   * 2回目以降は8bit単位で出力 */
  while (n_bits >= stream->bit_count) {
    n_bits              = n_bits - stream->bit_count;
    stream->bit_buffer  |= (uint8_t)BITSTREAM_GETLOWERBITS(stream->bit_count, val >> n_bits);
    if (fputc(stream->bit_buffer, stream->fp) == EOF) {
      return BITSTREAM_APIRESULT_IOERROR;
    }
    stream->bit_buffer  = 0;
    stream->bit_count   = 8;
  }

  /* 端数ビットの処理:
   * 残った分をバッファの上位ビットにセット */
  assert(n_bits <= 8);
  stream->bit_count   -= n_bits;
  stream->bit_buffer  |= (uint8_t)(BITSTREAM_GETLOWERBITS(n_bits, val) << stream->bit_count);

  return BITSTREAM_APIRESULT_OK;
}

/* 1bit取得 */
BitStreamApiResult BitStream_GetBit(struct BitStream* stream, uint8_t* bit)
{
  int32_t ch;

  /* 引数チェック */
  if (stream == NULL || bit == NULL) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 読み込みモードでない場合は即時リターン */
  if (!(stream->flags & BITSTREAM_FLAGS_FILEOPENMODE_READ)) {
    return BITSTREAM_APIRESULT_INVALID_MODE;
  }

  /* 入力ビットカウントを1減らし、バッファの対象ビットを出力 */
  if (stream->bit_count > 0) {
    stream->bit_count--;
    (*bit) = (stream->bit_buffer >> stream->bit_count) & 1;
    /* (*bit) = (stream->bit_buffer & st_bit_mask[stream->bit_count]); */
    return BITSTREAM_APIRESULT_OK;
  }

  /* 1バイト読み込みとエラー処理 */
  if ((ch = getc(stream->fp)) == EOF) {
    if (feof(stream->fp)) {
      /* ファイル終端に達した */
      return BITSTREAM_APIRESULT_EOS;
    } else {
      /* それ以外のエラー */
      return BITSTREAM_APIRESULT_IOERROR;
    }
  }

  /* カウンタとバッファの更新 */
  stream->bit_count   = 7;
  stream->bit_buffer  = (uint8_t)ch;

  /* 取得したバッファの最上位ビットを出力 */
  (*bit) = (stream->bit_buffer >> 7) & 1;

  return BITSTREAM_APIRESULT_OK;
}

/* n_bits 取得（最大64bit）し、その値を右詰めして出力 */
BitStreamApiResult BitStream_GetBits(struct BitStream* stream, uint32_t n_bits, uint64_t *val)
{
  int32_t  ch;
  uint64_t tmp = 0;

  /* 引数チェック */
  if (stream == NULL || val == NULL) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 読み込みモードでない場合は即時リターン */
  if (!(stream->flags & BITSTREAM_FLAGS_FILEOPENMODE_READ)) {
    return BITSTREAM_APIRESULT_INVALID_MODE;
  }

  /* 入力可能な最大ビット数を越えている */
  if (n_bits > sizeof(uint64_t) * 8) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 最上位ビットからデータを埋めていく
   * 初回ループではtmpの上位ビットにセット
   * 2回目以降は8bit単位で入力しtmpにセット */
  while (n_bits > stream->bit_count) {
    n_bits  -= stream->bit_count;
    tmp     |= BITSTREAM_GETLOWERBITS(stream->bit_count, stream->bit_buffer) << n_bits;
    /* 1バイト読み込みとエラー処理 */
    if ((ch = getc(stream->fp)) == EOF) {
      if (feof(stream->fp)) {
        /* 途中でファイル終端に達していたら、ループを抜ける */
        goto END_OF_STREAM;
      } else {
        /* それ以外のエラー */
        return BITSTREAM_APIRESULT_IOERROR;
      }
    }
    stream->bit_buffer  = (uint8_t)ch;
    stream->bit_count   = 8;
  }

END_OF_STREAM:
  /* 端数ビットの処理 
   * 残ったビット分をtmpの最上位ビットにセット */
  stream->bit_count -= n_bits;
  tmp               |= (uint64_t)BITSTREAM_GETLOWERBITS(n_bits, (uint32_t)(stream->bit_buffer >> stream->bit_count));

  /* 正常終了 */
  *val = tmp;
  return BITSTREAM_APIRESULT_OK;
}

/* バッファにたまったビットをクリア */
BitStreamApiResult BitStream_Flush(struct BitStream* stream)
{
  /* 引数チェック */
  if (stream == NULL) {
    return BITSTREAM_APIRESULT_INVALID_ARGUMENT;
  }

  /* 既に先頭にあるときは何もしない */
  if (stream->bit_count == 8) {
    return BITSTREAM_APIRESULT_OK;
  }

  /* 読み込み位置を次のバイト先頭に */
  if (stream->flags & BITSTREAM_FLAGS_FILEOPENMODE_READ) {
    /* 残りビット分を空読み */
    uint64_t dummy;
    return BitStream_GetBits(stream, (uint32_t)stream->bit_count, &dummy);
  } else {
    /* バッファに余ったビットを強制出力 */
    return BitStream_PutBits(stream, (uint16_t)stream->bit_count, 0);
  }
}
