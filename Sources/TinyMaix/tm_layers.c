/* Copyright 2022 Sipeed Technology Co., Ltd. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
// It is default O0 implement
#include "float.h"
#include "math.h"
#include "tinymaix.h"

#if TM_OPT_LEVEL == TM_OPT0

#include "arch_cpu.h"
static uint32_t k_oft[TM_MAX_KSIZE];
static mtype_t sbuf[TM_MAX_KCSIZE];

#if (TM_MDL_TYPE == TM_MDL_FP32) || (TM_MDL_TYPE == TM_MDL_FP16)
#define SUMSCALE 1.0
TM_INLINE void l_postprocess_sum(sumtype_t sum, btype_t b, int act,
                                 mtype_t *outp, sctype_t scale, sctype_t out_s,
                                 zptype_t out_zp) {
  sum += b;
  switch (act) { // activation func
  case TM_ACT_RELU:
    sum = sum > 0 ? sum : 0;
    break;
  case TM_ACT_RELU6:
    sum = sum > 0 ? sum : 0;
    sum = sum > 6 ? 6 : sum;
    break;
  default:
    break;
  }
  *outp = (mtype_t)sum;
  return;
}
#elif (TM_MDL_TYPE == TM_MDL_INT8) || (TM_MDL_TYPE == TM_MDL_INT16)

#if TM_FASTSCALE
static int32_t sumscale[TM_MAX_CSIZE];
#else
static float sumscale[TM_MAX_CSIZE];
#endif
#define SUMSCALE (sumscale[c])

#if !TM_FASTSCALE
TM_INLINE void l_postprocess_sum(sumtype_t sum, btype_t b, int act,
                                 mtype_t *outp, sctype_t scale, sctype_t out_s,
                                 zptype_t out_zp)
#else
TM_INLINE void l_postprocess_sum(sumtype_t sum, btype_t b, int act,
                                 mtype_t *outp, int32_t scale, int32_t out_s,
                                 zptype_t out_zp)
#endif
{
#if !TM_FASTSCALE
  float sumf;
#else
  sumtype_t sumf;
#endif
  sum += b;
#if !TM_FASTSCALE
  sumf = sum * scale;
#else
  sumf = (sum << TM_FASTSCALE_SHIFT) / scale;
#endif

  switch (act) { // activation func
  case TM_ACT_RELU:
    sumf = sumf > 0 ? sumf : 0;
    break;
  case TM_ACT_RELU6:
    sumf = sumf > 0 ? sumf : 0;
#if (!TM_FASTSCALE)
    sumf = sumf > 6 ? 6 : sumf;
#else
    sumf = sumf > (6 << TM_FASTSCALE_SHIFT) ? (6 << TM_FASTSCALE_SHIFT) : sumf;
#endif
    break;
  default:
    break;
  }
#if !TM_FASTSCALE
  *outp = (mtype_t)(sumf / out_s +
                    out_zp); //(mtype_t)((int)(sumf/out_s) + out_zp)
                             ////(mtype_t)((int)(sumf/out_s +0.5) + out_zp)
#else
  *outp = (mtype_t)(
      ((sumf * out_s) >> (TM_FASTSCALE_SHIFT + TM_FASTSCALE_SHIFT)) + out_zp);
#endif
  return;
}
#endif

#define BATCH_SIZE 2
// for valid or kernel in valid part, use fast method
tm_err_t TM_WEAK tml_conv2d_dwconv2d(
    tm_mat_t *in, tm_mat_t *out, wtype_t *w, btype_t *b, int kw, int kh, int sx,
    int sy, int dx, int dy, int act, int pad_top, int pad_bottom, int pad_left,
    int pad_right, int dmul, sctype_t *ws, sctype_t in_s, zptype_t in_zp,
    sctype_t out_s, zptype_t out_zp) // kernel: (cho, chi, h, w)
{
  int pad_flag;
  int maxk;
  int chi;
  int cho;
  sumtype_t sum;
  mtype_t *sptr;
  mtype_t *outp;

  int x, y, c, cc, k, oft, idx, slow_flag, _ky, _kx,i;
  sumtype_t sums[BATCH_SIZE];
#if TM_FASTSCALE
  int32_t outscale;
#else
	sctype_t outscale;
#endif

  if (dx != 1 || dy != 1)
    return TM_ERR_TODO;

  if (act >= TM_ACT_MAXCNT) {
    return TM_ERR_UNSUPPORT;
  }

  maxk = kw * kh;
  if (maxk > TM_MAX_KSIZE)
    return TM_ERR_KSIZE;
  if (maxk == 1 && (pad_flag || dmul)) {
    return TM_ERR_UNSUPPORT;
  }

  chi = in->c;
  cho = out->c;
  sum = 0;
  outp = out->dat;
  pad_flag =
      (pad_top != 0 || pad_bottom != 0 || pad_left != 0 || pad_right != 0);

#if (TM_MDL_TYPE == TM_MDL_INT8) || (TM_MDL_TYPE == TM_MDL_INT16)
#if TM_FASTSCALE
	outscale = (1<<TM_FASTSCALE_SHIFT)/out_s;
	for(c=0; c<out->c;c++) sumscale[c]=1.0/ws[c]/in_s;
#else
	outscale = out_s;
	for(c=0; c<out->c;c++) sumscale[c]=ws[c]*in_s;
#endif
#else
	outscale = out_s;
#endif
	
	
  if (maxk == 1) {
    for (y = 0; y < out->h; y++) {
      for (x = 0; x < out->w; x++) {
        mtype_t *sptr = (mtype_t *)TM_MATP(in, sy * y, sx * x, 0);
        wtype_t *kptr = (wtype_t *)w;
        c = 0;
        for (; c < out->c - BATCH_SIZE + 1;) {
          tm_dot_prod_pack2(sptr, kptr, chi, sums);
          l_postprocess_sum(sums[0], b[c], act, outp, SUMSCALE, outscale,
                            out_zp);
          c++;
          outp++;
          l_postprocess_sum(sums[1], b[c], act, outp, SUMSCALE, outscale,
                            out_zp);
          c++;
          outp++;
          kptr += chi * BATCH_SIZE; //*2;
        }
        for (; c < out->c; c++) {
          tm_dot_prod(sptr, kptr, chi, &sum); // size=maxk*chi //pw maxk==1
          l_postprocess_sum(sum, b[c], act, outp, SUMSCALE, outscale, out_zp);
          outp++;
          kptr += chi;
        }
      }
    }
    TM_PERF_ADD(t_pwconv);
    return TM_OK;
  }

  if (dmul) {
    TM_PERF_START(t_dwconv);
  } else {
    TM_PERF_START(t_conv);
  };
  oft = 0;
  idx = 0;
  for (y = 0; y < kh; y++) { // gen k_oft table
    for (x = 0; x < kw; x++) {
      k_oft[idx] = oft;
      idx += 1;
      oft += chi;
    }
    oft += (in->w - kw) * chi;
  }

  chi = dmul ? 1 : in->c; // dmul>=1 indicate depthwise; dummy chi for dwconv
                          // compatible
  slow_flag = 0;          // same pad part is slow
  for (y = 0; y < out->h; y++) {
    int src_y0 = sy * y - pad_top;
    for (x = 0; x < out->w; x++) {
      int src_x0 = sx * x - pad_left;
      sumtype_t sum;
      slow_flag = ((src_y0 < 0) + (src_x0 < 0) + (src_y0 + kh > in->h) +
                   (src_x0 + kw > in->w));
      // TM_PERF_START(t_sbuf);
      if (!slow_flag) { // valid or same valid part
        mtype_t *sptr_base =
            (mtype_t *)TM_MATP(in, src_y0, src_x0, 0); //?c/dmul:0
        mtype_t *sptr =
            sptr_base; //= (mtype_t*)TM_MATP(in, src_y0, src_x0, 0); //sbuf 不变
        uint32_t sidx = 0; // sbuf:cho,chi,maxk //dw:chi==1;
        for (cc = 0; cc < (dmul ? cho : chi); cc++) {
          for (k = 0; k < maxk; k++) {
            sbuf[sidx + k] = sptr[k_oft[k]];
          }
          sidx += maxk;
          sptr = sptr_base + (dmul ? (cc + 1) / dmul : (cc + 1));
        }
      } else { // same pad part
        int _ky0 = src_y0 < 0 ? -src_y0 : 0;
        int _kx0 = src_x0 < 0 ? -src_x0 : 0;
        int _ky1 = in->h - src_y0 > kh ? kh : in->h - src_y0;
        int _kx1 = in->w - src_x0 > kw ? kw : in->w - src_x0;
        uint32_t sidx = 0; // sbuf:cho,chi,maxk //dw:chi==1;
        uint32_t s_step = (_ky1 - _ky0) * (_kx1 - _kx0);
        mtype_t *sptr_base = (mtype_t *)TM_MATP(in, src_y0, src_x0, 0);
        mtype_t *sptr = sptr_base;
        memset(sbuf, (char)in_zp, dmul ? cho * maxk : chi * maxk); // do padding
        for (cc = 0; cc < (dmul ? cho : chi); cc++) {
          for (_ky = _ky0; _ky < _ky1; _ky++) {
            for (_kx = _kx0; _kx < _kx1; _kx++) {
              int k = _ky * kw + _kx;
              sbuf[sidx + k] = sptr[k_oft[k]];
            }
          }
          sidx += maxk;
          sptr = sptr_base + (dmul ? (cc + 1) / dmul : (cc + 1));
        }
      }
      sptr = sbuf;                   // sbuf prepare ok~
      if (maxk * chi == 9 && dmul) { // simple opt for 3x3 dwconv
        for (c = 0; c < out->c; c++) {
          wtype_t *kptr =
              (wtype_t *)w + c * chi * maxk; // TM_PERF_START(t_dotp);
          tm_dot_prod_3x3x1(sptr, kptr,
                            &sum); // TM_PERF_ADD(t_dotp);TM_PERF_START(t_post);
          l_postprocess_sum(sum, b[c], act, outp, SUMSCALE, outscale, out_zp);
          outp++;       // TM_PERF_ADD(t_post);
          sptr += maxk; // dwconv need move step
        }
      } else {
        for (c = 0; c < out->c; c++) {
          wtype_t *kptr =
              (wtype_t *)w + c * chi * maxk; // TM_PERF_START(t_dotp);
          tm_dot_prod(sptr, kptr, maxk * chi,
                      &sum); // TM_PERF_ADD(t_dotp);TM_PERF_START(t_post);
          l_postprocess_sum(sum, b[c], act, outp, SUMSCALE, outscale, out_zp);
          outp++; // TM_PERF_ADD(t_post);
          if (dmul)
            sptr += maxk; // dwconv need move step
        }
      }
      if (!slow_flag) {
        TM_PERF_ADD(t_valid);
      } else {
        TM_PERF_ADD(t_pad);
      }
    }
  }
  if (dmul) {
    TM_PERF_ADD(t_dwconv);
  } else {
    TM_PERF_ADD(t_conv);
  };
  return TM_OK;
}

/*************************** TML_GAP **********************************/
tm_err_t TM_WEAK tml_gap(tm_mat_t *in, tm_mat_t *out, sctype_t in_s,
                         zptype_t in_zp, sctype_t out_s, zptype_t out_zp) {
  mtype_t *dat;
  int c, y, x;
  for (c = 0; c < out->c; c++) {
    sumtype_t sum = 0;
    dat = in->dat + c;
    for (y = 0; y < in->h; y++) {
      for (x = 0; x < in->w; x++) {
        sum += ((sumtype_t)(*dat));
        dat += out->c;
      }
    }
#if TM_MDL_TYPE == TM_MDL_INT8 || TM_MDL_TYPE == TM_MDL_INT16
    out->dat[c] = (mtype_t)((sum / ((in->h) * (in->w)) - in_zp) * in_s / out_s +
                            out_zp); // requant
#elif TM_MDL_TYPE == TM_MDL_FP32 || TM_MDL_TYPE == TM_MDL_FP16
    out->dat[c] = (mtype_t)(sum / ((in->h) * (in->w)));
//#else //#elif TM_MDL_TYPE == TM_MDL_FP8_143 || TM_MDL_TYPE == TM_MDL_FP8_152
#endif
  }
  return TM_OK;
}

/*************************** TML_FC **********************************/
tm_err_t TM_WEAK tml_fc(tm_mat_t *in, tm_mat_t *out, wtype_t *w, btype_t *b,
                        sctype_t *ws, sctype_t in_s, zptype_t in_zp,
                        sctype_t out_s, zptype_t out_zp) {
  mtype_t *dat = in->dat;
  int c;
  for (c = 0; c < out->c; c++) {
    sumtype_t sum = 0;
    tm_dot_prod(dat, w + c * in->c, in->c, &sum);
    sum += b[c]; // fuse with zp
#if TM_MDL_TYPE == TM_MDL_INT8 || TM_MDL_TYPE == TM_MDL_INT16
    out->dat[c] = (mtype_t)(sum * in_s * ws[0] / out_s + out_zp); // requant
#else
    out->dat[c] = (mtype_t)(sum);
#endif
  }
  return TM_OK;
}

/*************************** TML_SOFTMAX **********************************/
tm_err_t TM_WEAK tml_softmax(
    tm_mat_t *in, tm_mat_t *out, sctype_t in_s, zptype_t in_zp, sctype_t out_s,
    zptype_t out_zp) { // TM_DBGT_INIT(); //note we have float size output buf
                       // even in INT8/INT16 mode
  int c;
  mtype_t *din = in->dat;
  float *dout = (float *)(out->dat);
  float dmax = -FLT_MAX;
  float sum = 0;
  for (c = 0; c < in->c; c++) {
#if TM_MDL_TYPE == TM_MDL_INT8 || TM_MDL_TYPE == TM_MDL_INT16
    dout[c] = (float)((sumtype_t)din[c] - in_zp) * in_s;
#else
    dout[c] = din[c];
#endif
    if (dout[c] > dmax)
      dmax = dout[c];
  }

  for (c = 0; c < in->c; c++) {
    dout[c] -= dmax;
    dout[c] = (float)tm_exp(dout[c]);
    sum += dout[c];
    dout[c] -= 0.000001; // prevent 1.0 value (cause 256 overflow)
  }
  for (c = 0; c < in->c; c++) { // int8/int16 <= fp32, so it is ok
#if TM_MDL_TYPE == TM_MDL_INT8 || TM_MDL_TYPE == TM_MDL_INT16
    out->dat[c] = (mtype_t)(dout[c] / sum / out_s + out_zp); // requant
#else
    out->dat[c] = (mtype_t)(dout[c] / sum);
#endif
  }
  return TM_OK;
}

/*************************** TML_RESHAPE **********************************/
tm_err_t TM_WEAK tml_reshape(tm_mat_t *in, tm_mat_t *out, sctype_t in_s,
                             zptype_t in_zp, sctype_t out_s, zptype_t out_zp) {
  return TM_OK;
}

#endif