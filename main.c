/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "wav.h"
#include "bit_stream.h"
#include "ala_utility.h"
#include "ala_coder.h"
#include "ala_predictor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

/* バージョン番号 */
#define ALA_VERSION_STRING  "1.0.0"

/* フォーマットバージョン */
#define ALA_FORMAT_VERSION  1

/* ブロックあたりサンプル数 */
#define ALA_NUM_SAMPLES_PER_BLOCK 4096

/* PARCOR係数の次数 */
#define ALA_PARCOR_ORDER          10

/* エンファシスフィルタのシフト量 */
#define ALA_EMPHASIS_FILTER_SHIFT 5

/* エンコード 成功時は0、失敗時は0以外を返す */
int do_encode(const char* in_filename, const char* out_filename)
{
  struct WAVFile*           in_wav;
  struct stat               fstat;
  struct BitStream*         out_strm;
  struct ALACoder*          coder;
  struct ALALPCCalculator*  lpcc;
  struct ALALPCSynthesizer* lpcs;
  uint32_t  ch, smpl, ord;
  double**  parcor_coef;
  int32_t** parcor_coef_int32;
  int32_t   encoded_data_size;
  double**  input;
  int32_t** input_int32;
  int32_t** residual;
  double*   window;
  uint32_t  num_channels, num_samples;
  uint32_t  enc_offset_sample;

  /* WAVファイルオープン */
  if ((in_wav = WAV_CreateFromFile(in_filename)) == NULL) {
    fprintf(stderr, "Failed to open %s. \n", in_filename);
    return 1;
  }

  /* 入力ファイルのサイズを拾っておく */
  stat(in_filename, &fstat);

  /* 出力ファイルオープン */
  if ((out_strm = BitStream_Open(out_filename, "wb", NULL, 0)) == NULL) {
    fprintf(stderr, "Failed to open %s. \n", out_filename);
    return 1;
  }

  /* 16bitよりも大きい量子化ビットの波形はエンコード不可 */
  if (in_wav->format.bits_per_sample > 16) {
    fprintf(stderr, "Unsupported bit-width(%d) \n", in_wav->format.bits_per_sample);
    return 1;
  }

  /* 頻繁に使用する変数をオート変数に受けておく */
  num_channels  = in_wav->format.num_channels;
  num_samples   = in_wav->format.num_samples;

  /* 領域割当て */
  input             = (double **)malloc(sizeof(double *) * num_channels);
  input_int32       = (int32_t **)malloc(sizeof(int32_t *) * num_channels);
  residual          = (int32_t **)malloc(sizeof(int32_t *) * num_channels);
  parcor_coef       = (double **)malloc(sizeof(double *) * num_channels);
  parcor_coef_int32 = (int32_t **)malloc(sizeof(int32_t *) * num_channels);
  for (ch = 0; ch < num_channels; ch++) {
    input[ch]       = (double *)malloc(sizeof(double) * num_samples);
    input_int32[ch] = (int32_t *)malloc(sizeof(int32_t) * num_samples);
    residual[ch]    = (int32_t *)malloc(sizeof(int32_t) * ALA_NUM_SAMPLES_PER_BLOCK);
    parcor_coef[ch] = (double *)malloc(sizeof(double) * (ALA_PARCOR_ORDER + 1));
    parcor_coef_int32[ch] = (int32_t *)malloc(sizeof(int32_t) * (ALA_PARCOR_ORDER + 1));
  }
  window = (double *)malloc(sizeof(double) * ALA_NUM_SAMPLES_PER_BLOCK);

  /* 分析合成ハンドル作成 */
  lpcc = ALALPCCalculator_Create(ALA_PARCOR_ORDER);
  lpcs = ALALPCSynthesizer_Create(ALA_PARCOR_ORDER);

  /* 残差符号化ハンドル作成 */
  coder = ALACoder_Create(num_channels);

  /* 入力データ取得 */
  for (ch = 0; ch < num_channels; ch++) {
    for (smpl = 0; smpl < num_samples; smpl++) {
      input[ch][smpl] = WAVFile_PCM(in_wav, smpl, ch) * pow(2, -31);
      input_int32[ch][smpl] = WAVFile_PCM(in_wav, smpl, ch);
      /* 情報が失われない程度に右シフト */
      input_int32[ch][smpl] >>= (32 - in_wav->format.bits_per_sample);
    }
  }

  /* ヘッダの書き出し */
  /* シグネチャ */
  BitStream_PutBits(out_strm,  8, 'A');
  BitStream_PutBits(out_strm,  8, 'L');
  BitStream_PutBits(out_strm,  8, 'A');
  BitStream_PutBits(out_strm,  8, '\0');
  /* フォーマットバージョン */
  BitStream_PutBits(out_strm, 16, ALA_FORMAT_VERSION);
  /* チャンネル数 */
  BitStream_PutBits(out_strm,  8, num_channels);
  /* サンプル数 */
  BitStream_PutBits(out_strm, 32, num_samples);
  /* サンプリングレート */
  BitStream_PutBits(out_strm, 32, in_wav->format.sampling_rate);
  /* サンプルあたりbit数 */
  BitStream_PutBits(out_strm,  8, in_wav->format.bits_per_sample);
  /* ブロックあたりサンプル数 */
  BitStream_PutBits(out_strm, 16, ALA_NUM_SAMPLES_PER_BLOCK);
  /* PARCOR係数次数 */
  BitStream_PutBits(out_strm,  8, ALA_PARCOR_ORDER);

  /* ステレオチャンネル以上ならばMS処理を行う */
  if (num_channels >= 2) {
    ALAChannelDecorrelator_LRtoMSDouble(input, num_channels, num_samples);
    ALAChannelDecorrelator_LRtoMSInt32(input_int32, num_channels, num_samples);
  }

  /* ブロック単位で残差計算/符号化 */
  enc_offset_sample = 0;
  while (enc_offset_sample < num_samples) {
    uint32_t num_encode_samples;

    /* エンコードするサンプル数の決定 */
    num_encode_samples = ALAUTILITY_MIN(ALA_NUM_SAMPLES_PER_BLOCK, num_samples - enc_offset_sample);

    /* 窓の作成 */
    ALAUtility_MakeSinWindow(window, num_encode_samples);
    /* 窓掛け */
    for (ch = 0; ch < num_channels; ch++) {
      ALAUtility_ApplyWindow(window, &input[ch][enc_offset_sample], num_encode_samples);
    }

    /* PARCOR係数の導出 */
    for (ch = 0; ch < num_channels; ch++) {
      /* プリエンファシス */
      ALAEmphasisFilter_PreEmphasisDouble(
          &input[ch][enc_offset_sample], num_encode_samples, ALA_EMPHASIS_FILTER_SHIFT);
      if (ALALPCCalculator_CalculatePARCORCoefDouble(lpcc,
            &input[ch][enc_offset_sample], num_encode_samples,
            parcor_coef[ch], ALA_PARCOR_ORDER) != ALAPREDICTOR_APIRESULT_OK) {
        fprintf(stderr, "Failed to calculate PARCOR coefficients. \n");
        return 1;
      }
    }

    /* PARCOR係数量子化 */
    for (ch = 0; ch < num_channels; ch++) {
      /* PARCOR係数の0次成分は0.0のはずなので処理をスキップ */
      parcor_coef_int32[ch][0] = 0;
      for (ord = 0; ord < ALA_PARCOR_ORDER; ord++) {
        /* 整数へ丸める */
        parcor_coef_int32[ch][ord]
          = (int32_t)ALAUtility_Round(parcor_coef[ch][ord] * pow(2.0f, 15));
        /* roundによる丸めによりビット幅をはみ出てしまうことがあるので範囲制限 */
        parcor_coef_int32[ch][ord] 
          = ALAUTILITY_INNER_VALUE(parcor_coef_int32[ch][ord], INT16_MIN, INT16_MAX);
      }
    }

    /* 残差計算 */
    /* プリエンファシスフィルタ */
    for (ch = 0; ch < num_channels; ch++) {
      if (ALAEmphasisFilter_PreEmphasisInt32(&input_int32[ch][enc_offset_sample],
            num_encode_samples, ALA_EMPHASIS_FILTER_SHIFT) != ALAPREDICTOR_APIRESULT_OK) {
        fprintf(stderr, "Failed to apply pre-emphasis. \n");
        return 1;
      }
    }
    /* PARCOR予測フィルタ */
    for (ch = 0; ch < num_channels; ch++) {
      if (ALALPCSynthesizer_PredictByParcorCoefInt32(lpcs, 
            &input_int32[ch][enc_offset_sample], num_encode_samples,
            parcor_coef_int32[ch], ALA_PARCOR_ORDER, residual[ch]) != ALAPREDICTOR_APIRESULT_OK) {
        fprintf(stderr, "Failed to predict parcor filter. \n");
        return 1;
      }
    }

    /* ブロック符号化 */
    /* ブロック先頭を示す同期コード */
    BitStream_PutBits(out_strm, 16, 0xFFFF);
    /* 各チャンネルのPARCOR係数 */
    for (ch = 0; ch < num_channels; ch++) {
      /* 0次係数は0だから飛ばす */
      for (ord = 1; ord < ALA_PARCOR_ORDER + 1; ord++) {
        BitStream_PutBits(out_strm, 16, ALAUTILITY_SINT32_TO_UINT32(parcor_coef_int32[ch][ord]));
      }
    }
    /* 残差符号化 */
    ALACoder_PutDataArray(coder, out_strm, 
        (const int32_t **)residual, num_channels, num_encode_samples);

    /* バイト境界に揃える */
    BitStream_Flush(out_strm);

    /* エンコードしたサンプル分進める */
    enc_offset_sample += num_encode_samples;

    /* 進捗を表示 */
    if ((enc_offset_sample % (10 * ALA_NUM_SAMPLES_PER_BLOCK)) == 0) {
      printf("Progress... %4.1f %%\r", 100.0f * (double)enc_offset_sample / num_samples);
      fflush(stdout);
    }
  }

  /* 出力サイズ取得 */
  BitStream_Tell(out_strm, &encoded_data_size);

  /* 圧縮結果表示 */
  printf("Encode succuess! size:%d -> %d \n", (uint32_t)fstat.st_size, encoded_data_size);

  /* 領域開放 */
  for (ch = 0; ch < num_channels; ch++) {
    free(input[ch]);
    free(input_int32[ch]);
    free(residual[ch]);
    free(parcor_coef[ch]);
    free(parcor_coef_int32[ch]);
  }
  free(input);
  free(input_int32);
  free(residual);
  free(parcor_coef);
  free(parcor_coef_int32);
  free(window);

  /* ハンドル破棄 */
  ALALPCCalculator_Destroy(lpcc);
  ALALPCSynthesizer_Destroy(lpcs);
  ALACoder_Destroy(coder);
  WAV_Destroy(in_wav);
  BitStream_Close(out_strm);

  return 0;
}

/* デコード 成功時は0、失敗時は0以外を返す */
int do_decode(const char* in_filename, const char* out_filename)
{
  struct BitStream*         in_strm;
  struct WAVFile*           out_wav;
  struct WAVFileFormat      wav_format;
  struct ALALPCSynthesizer* lpcs;
  struct ALACoder*          coder;
  uint32_t  ch, smpl, ord;
  uint64_t  bitsbuf;
  uint32_t  parcor_order;
  uint32_t  num_block_samples;
  uint32_t  num_channels, num_samples;
  uint32_t  dec_offset_sample;
  int32_t** parcor_coef;
  int32_t** residual;
  int32_t** output;

  /* 入力ファイルオープン */
  if ((in_strm = BitStream_Open(in_filename, "rb", NULL, 0)) == NULL) {
    fprintf(stderr, "Failed to open %s. \n", in_filename);
    return 1;
  }

  /* ヘッダの読み出し */
  /* シグネチャ */
  BitStream_GetBits(in_strm, 32, &bitsbuf);
  /* シグネチャの確認 */
  if (   (((bitsbuf >> 24) & 0xFF) != 'A')
      || (((bitsbuf >> 16) & 0xFF) != 'L')
      || (((bitsbuf >>  8) & 0xFF) != 'A')
      || (((bitsbuf >>  0) & 0xFF) != '\0')) {
    fprintf(stderr, "Invalid signature. \n");
    return 1;
  }
  /* フォーマットバージョン */
  BitStream_GetBits(in_strm, 16, &bitsbuf);
  /* シグネチャの確認 */
  if (bitsbuf != ALA_FORMAT_VERSION) {
    fprintf(stderr, "Unsupported format version:%d \n", (uint16_t)bitsbuf);
    return 1;
  }
  /* チャンネル数 */
  BitStream_GetBits(in_strm,  8, &bitsbuf);
  num_channels = wav_format.num_channels = (uint32_t)bitsbuf;
  /* サンプル数 */
  BitStream_GetBits(in_strm, 32, &bitsbuf);
  num_samples = wav_format.num_samples = (uint32_t)bitsbuf;
  /* サンプリングレート */
  BitStream_GetBits(in_strm, 32, &bitsbuf);
  wav_format.sampling_rate = (uint32_t)bitsbuf;
  /* サンプルあたりbit数 */
  BitStream_GetBits(in_strm,  8, &bitsbuf);
  wav_format.bits_per_sample = (uint32_t)bitsbuf;
  /* ブロックあたりサンプル数 */
  BitStream_GetBits(in_strm, 16, &bitsbuf);
  num_block_samples = (uint32_t)bitsbuf;
  /* PARCOR係数次数 */
  BitStream_GetBits(in_strm,  8, &bitsbuf);
  parcor_order = (uint32_t)bitsbuf;

  /* 得られた情報を表示 */
  printf("Num Channels:%d \n",          wav_format.num_channels);
  printf("Num Samples:%d \n",           wav_format.num_samples);
  printf("Sampling Rate:%d \n",         wav_format.sampling_rate);
  printf("Bits Per Sample:%d \n",       wav_format.bits_per_sample);
  printf("PARCOR Order:%d \n",          parcor_order);
  printf("Num Samples Per Block:%d \n", num_block_samples);

  /* 出力wavハンドルの生成 */
  wav_format.data_format  = WAV_DATA_FORMAT_PCM;
  if ((out_wav = WAV_Create(&wav_format)) == NULL) {
    fprintf(stderr, "Failed to create wav handle. \n");
    return 1;
  }

  /* 変数領域割当て */
  parcor_coef = (int32_t **)malloc(sizeof(int32_t*) * num_channels);
  residual    = (int32_t **)malloc(sizeof(int32_t*) * num_channels);
  output      = (int32_t **)malloc(sizeof(int32_t*) * num_channels);
  for (ch = 0; ch < num_channels; ch++) {
    parcor_coef[ch] = (int32_t *)malloc(sizeof(int32_t) * (parcor_order + 1));
    residual[ch]    = (int32_t *)malloc(sizeof(int32_t) * num_block_samples);
    output[ch]      = (int32_t *)malloc(sizeof(int32_t) * num_block_samples);
  }

  /* 合成ハンドル作成 */
  lpcs  = ALALPCSynthesizer_Create(parcor_order);
  /* 残差復号ハンドル作成 */
  coder = ALACoder_Create(num_channels);

  /* ブロックデコード */
  dec_offset_sample = 0;
  while (dec_offset_sample < num_samples) {
    uint32_t num_decode_samples;

    /* 同期コード */
    BitStream_GetBits(in_strm, 16, &bitsbuf);
    if (bitsbuf != 0xFFFF) {
      fprintf(stderr, "Failed to decode block: it's not sync code(=0x%4x). \n", (uint16_t)bitsbuf);
      return 1;
    }
    /* PARCOR係数 */
    for (ch = 0; ch < num_channels; ch++) {
      parcor_coef[ch][0] = 0;
      for (ord = 1; ord < parcor_order + 1; ord++) {
        BitStream_GetBits(in_strm, 16, &bitsbuf);
        parcor_coef[ch][ord] = (int32_t)ALAUTILITY_UINT32_TO_SINT32(bitsbuf);
      }
    }

    /* デコードサンプル数の確定 */
    num_decode_samples = ALAUTILITY_MIN(num_block_samples, num_samples - dec_offset_sample);

    /* 残差復号 */
    ALACoder_GetDataArray(coder, in_strm, residual, num_channels, num_decode_samples);

    /* バイト境界に揃える */
    BitStream_Flush(in_strm);

    /* 残差から合成 */
    /* PARCOR合成フィルタ */
    for (ch = 0; ch < num_channels; ch++) {
      if (ALALPCSynthesizer_SynthesizeByParcorCoefInt32(lpcs,
            residual[ch], num_decode_samples,
            parcor_coef[ch], parcor_order,
            output[ch]) != ALAPREDICTOR_APIRESULT_OK) {
        fprintf(stderr, "Failed to synthesize by parcor filter. \n");
        return 1;
      }
    }
    /* デエンファシスフィルタ */
    for (ch = 0; ch < num_channels; ch++) {
      if (ALAEmphasisFilter_DeEmphasisInt32(output[ch],
            num_decode_samples, ALA_EMPHASIS_FILTER_SHIFT) != ALAPREDICTOR_APIRESULT_OK) {
        fprintf(stderr, "Failed to apply de-emphasis. \n");
        return 1;
      }
    }

    /* MS処理をしていたら元に戻す */
    if (num_channels >= 2) {
      ALAChannelDecorrelator_MStoLRInt32(output, num_channels, num_decode_samples);
    }

    /* エンコード時に右シフトした分を戻す */
    for (ch = 0; ch < num_channels; ch++) {
      for (smpl = 0; smpl < num_decode_samples; smpl++) {
        WAVFile_PCM(out_wav, dec_offset_sample + smpl, ch)
          = output[ch][smpl] << (32 - wav_format.bits_per_sample);
      }
    }

    /* デコードしたサンプル分進める */
    dec_offset_sample += num_decode_samples;

    /* 進捗を表示 */
    if ((dec_offset_sample % (10 * num_block_samples)) == 0) {
      printf("Progress... %4.1f %%\r", 100.0f * (double)dec_offset_sample / num_samples);
      fflush(stdout);
    }
  }

  /* WAVファイル書き出し */
  if (WAV_WriteToFile(out_filename, out_wav) != WAV_APIRESULT_OK) {
    fprintf(stderr, "Failed to write wav file. \n");
    return 1;
  }

  /* 領域開放 */
  for (ch = 0; ch < num_channels; ch++) {
    free(parcor_coef[ch]);
    free(residual[ch]);
    free(output[ch]);
  }
  free(parcor_coef);
  free(residual);
  free(output);
  WAV_Destroy(out_wav);
  BitStream_Close(in_strm);

  return 0;
}

/* 使用法の表示 */
static void print_usage(char** argv)
{
  printf("ALA - Ayashi Lossless Audio Compressor Version %s \n", ALA_VERSION_STRING);
  printf("Usage: %s -[ed] INPUT_FILE_NAME OUTPUT_FILE_NAME \n", argv[0]);
}

/* メインエントリ */
int main(int argc, char** argv)
{
  const char* option;
  const char* input_file;
  const char* output_file;

  /* 引数が足らない */
  if (argc < 4) {
    print_usage(argv);
    return 1;
  }

  /* 引数文字列の取得 */
  option      = argv[1];
  input_file  = argv[2];
  output_file = argv[3];

  /* エンコード/デコード呼び分け */
  if (strcmp(option, "-e") == 0) {
    if (do_encode(input_file, output_file) != 0) {
      fprintf(stderr, "Failed to encode. \n");
      return 1;
    }
  } else if (strcmp(option, "-d") == 0) {
    if (do_decode(input_file, output_file) != 0) {
      fprintf(stderr, "Failed to decode. \n");
      return 1;
    }
  } else {
    print_usage(argv);
    return 1;
  }

  return 0;
}
