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

#include "stdlib.h"
#include "stdint.h"
#include "math.h"
#include "tm_port.h"

#include "arm_neon.h"
//ARM NEON instruction acceleration
//https://developer.arm.com/architectures/instruction-sets/intrinsics/
//https://arm-software.github.io/acle/neon_intrinsics/advsimd.html
//https://blog.csdn.net/fengbingchun/article/details/38085781
//https://blog.csdn.net/u010580016/article/details/110943019

/********************************** TM_MDL_INT8 ***********************************************/
#if TM_MDL_TYPE==TM_MDL_INT8
#define PARALLEL_CNT 8
TM_INLINE void tm_dot_prod(mtype_t* sptr, mtype_t* kptr,uint32_t size, sumtype_t* result){
    uint32_t cnt;                       
    sumtype_t sum = 0;                                            
    cnt = size/PARALLEL_CNT;

    int32x4_t _sum = vdupq_n_s32(0);
    for(int i = 0; i < cnt; i++) {
        int8x8_t s8, k8;
        s8 = vld1_s8(sptr);//8 value
        k8 = vld1_s8(kptr);
        _sum = vmlal_s16(_sum, vget_low_s16(vmovl_s8(s8)), vget_low_s16(vmovl_s8(k8)));
        _sum = vmlal_s16(_sum, vget_high_s16(vmovl_s8(s8)),vget_high_s16( vmovl_s8(k8)));
        sptr += PARALLEL_CNT;
        kptr += PARALLEL_CNT;
    }
    int32_t _sums[4];
    vst1q_s32(_sums, _sum);
    for(int i = 0; i < 4; i++) 
        sum += _sums[i];

    cnt = size % PARALLEL_CNT;
    while (cnt > 0U){
        sum += (int32_t) ((int16_t) *sptr++ * *kptr++);
        cnt--;
    }

    *result = sum;
    return;
}

TM_INLINE void tm_dot_prod_gap_3x3x1(mtype_t* sptr, mtype_t* kptr, uint32_t* k_oft, sumtype_t* result)
{
    *result = sptr[k_oft[0]]*kptr[0] + sptr[k_oft[1]]*kptr[1] + sptr[k_oft[2]]*kptr[2] + \
        sptr[k_oft[3]]*kptr[3] + sptr[k_oft[4]]*kptr[4] + sptr[k_oft[5]]*kptr[5] + \
        sptr[k_oft[6]]*kptr[6] + sptr[k_oft[7]]*kptr[7] + sptr[k_oft[8]]*kptr[8] ;
    return;                  
}

/********************************** TM_MDL_FP32 ***********************************************/
#elif TM_MDL_TYPE==TM_MDL_FP32
#define PARALLEL_CNT 4
TM_INLINE void tm_dot_prod(mtype_t* sptr, mtype_t* kptr,uint32_t size, sumtype_t* result){
    uint32_t cnt;                       
    sumtype_t sum = 0;                                             
    cnt = size/PARALLEL_CNT;

    float32x4_t _sum = vdupq_n_f32(0);
    for(int i = 0; i < cnt; i++) {
        float32x4_t s = vld1q_f32(sptr); 
        float32x4_t k = vld1q_f32(kptr);
        _sum = vmlaq_f32(_sum, s, k);
        sptr += PARALLEL_CNT;
        kptr += PARALLEL_CNT;
    }
    float32_t _sums[PARALLEL_CNT];
    vst1q_f32(_sums, _sum);
    for(int i = 0; i < PARALLEL_CNT; i++) 
        sum += _sums[i];
    
    cnt = size % PARALLEL_CNT;
    while (cnt > 0U){
        sum += (int32_t) ((int16_t) *sptr++ * *kptr++);
        cnt--;
    }

    *result = sum;
    return;
}

TM_INLINE void tm_dot_prod_gap_3x3x1(mtype_t* sptr, mtype_t* kptr, uint32_t* k_oft, sumtype_t* result)
{
    *result = sptr[k_oft[0]]*kptr[0] + sptr[k_oft[1]]*kptr[1] + sptr[k_oft[2]]*kptr[2] + \
        sptr[k_oft[3]]*kptr[3] + sptr[k_oft[4]]*kptr[4] + sptr[k_oft[5]]*kptr[5] + \
        sptr[k_oft[6]]*kptr[6] + sptr[k_oft[7]]*kptr[7] + sptr[k_oft[8]]*kptr[8] ;
    return;                  
}

#else  
    #error "TODO"
#endif
