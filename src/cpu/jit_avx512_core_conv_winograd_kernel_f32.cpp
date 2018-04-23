/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "c_types_map.hpp"
#include "mkldnn_thread.hpp"
#include "nstl.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"
#include "cpu_memory.hpp"

#include <math.h>

#include "jit_avx512_core_conv_winograd_kernel_f32.hpp"

#define GET_OFF(field) offsetof(jit_wino_transform_call_s, field)
namespace mkldnn {
namespace impl {
namespace cpu {

namespace {

using namespace mkldnn::impl::utils;

unsigned int L1_cache_size = get_cache_size(1, true);
unsigned int L2_cache_size = get_cache_size(2, true);
unsigned int LLC_data_size = get_cache_size(3, false);

// the test funtion takes jcp, the candidate and the current best.
// it  returns true if the new candidate is better
int get_divisor_satisfying_cond(jit_conv_winograd_conf_t &jcp, int number,
        int default_best, bool (*test)(jit_conv_winograd_conf_t &, int, int))
{
    int best_divisor = default_best;
    auto test_num
            = [&best_divisor, test](jit_conv_winograd_conf_t &jcp, int num) {
                  if (test(jcp, num, best_divisor)) {
                      best_divisor = num;
                  }
              };

    for (int divisor = 1; divisor <= ::sqrt(number); divisor++) {
        if (number % divisor == 0) {
            test_num(jcp, divisor);
            test_num(jcp, number / divisor);
        }
    }

    return best_divisor;
}

/* assumes 512 bits registers */
/* TODO: add support for strides */
/* TODO: handle the prefetch distance automatically */
typedef enum cache_t_ { L1, L2, L3 } cache_t;

template <typename data_t>
struct prefetcher_t {
    prefetcher_t(jit_generator *generator, Xbyak::Reg64 reg_base_addr,
            cache_t cache_type, size_t block_size, /* in number of elements*/
            int nb_instructions_in_block, int fma_ipc)
        : cg_(generator)
        , reg_base_addr_(reg_base_addr)
        , cache_type_(cache_type)
        , cache_block_size_(block_size)
    {
        nb_cache_lines_to_prefetch_ = cache_block_size_ / (64 / sizeof(data_t));
        prefetch_spread_
                = div_up(nb_instructions_in_block, nb_cache_lines_to_prefetch_);
        prefetch_blk_
                = div_up(nb_cache_lines_to_prefetch_, nb_instructions_in_block);

        /* assumption: when fetch in Li, data is already in L(i+1) */
        int cache_latency;
        switch (cache_type_) {
        case L1: cache_latency = 14; break;
        case L2: cache_latency = 250; break;
        case L3: cache_latency = 250; break;
        }

        prefetch_distance_ = div_up(cache_latency, nb_cache_lines_to_prefetch_);
    }

    void prefetch(int instruction_number)
    {
        if (instruction_number % prefetch_spread_ == 0) {
            for (int i = 0; (i < prefetch_blk_)
                    && (prefetches_issued_ < nb_cache_lines_to_prefetch_);
                    i++, prefetches_issued_++) {
                prefetch_inst_(cg_->EVEX_compress_addr(
                        reg_base_addr_, (cache_block_size_ * prefetch_distance_)
                                        * sizeof(data_t)
                                + (prefetches_issued_ * 64)));
            }
        }
    }

private:
    void prefetch_inst_(const Xbyak::Address &addr)
    {
        switch (cache_type_) {
        case L1: cg_->prefetcht0(addr); break;
        case L2: cg_->prefetcht1(addr); break;
        case L3: cg_->prefetcht2(addr); break;
        default:
            break; // TODO: raise an exception or put an assert
        }
    }

    jit_generator *cg_;
    Xbyak::Reg64 reg_base_addr_;
    cache_t cache_type_;
    int cache_block_size_ = 0;
    int nb_cache_lines_to_prefetch_ = 0;
    int prefetches_issued_ = 0;
    int prefetch_spread_ = 0;
    int prefetch_blk_ = 0;
    int prefetch_distance_ = 0;
};

// utilities to support kernel parameter selection
bool check_L2_block_per_thread(jit_conv_winograd_conf_t &jcp,
        int dimN_block, float C2_min, float C2_max) {
    float block_size = alpha * alpha * (2*(jcp.oc + jcp.ic)
        * dimN_block * jcp.dimN_reg_block
        + div_up(jcp.ic * jcp.oc,omp_get_max_threads())) * (float)sizeof(float);
    float L2_lb = C2_min * L2_cache_size;
    float L2_ub = C2_max * L2_cache_size;
    return (block_size > L2_lb && block_size < L2_ub);
}

bool check_L1_block_gemm(jit_conv_winograd_conf_t &jcp, int dimK_block,
        int dimM_block, float C1_min, float C1_max) {
    float gemm_block_size = (dimM_block * jcp.dimM_simd_block * dimK_block
                             * jcp.dimK_reg_block * jcp.dimM_reg_block
                     + dimK_block * jcp.dimK_reg_block * jcp.dimN_reg_block
                     + dimM_block * jcp.dimM_simd_block * jcp.dimN_reg_block)
                     * (float)sizeof(float);
    float L1_lb = C1_min * L1_cache_size;
    float L1_ub = C1_max * L1_cache_size;
    return (gemm_block_size > L1_lb && gemm_block_size < L1_ub);
}
bool check_cond1(int dimN_reg_block, int dimK_block, int dimK_reg_block,
        int dimM_block, int dimM_reg_block, int dimM_simd_block, float C)
{
    float lhs = (dimM_block * dimN_reg_block * dimM_simd_block * dimM_reg_block
                        + dimM_block * dimK_block * dimK_reg_block
                                * dimM_simd_block * dimM_reg_block
                        + dimK_block * dimN_reg_block * dimK_reg_block)
            * (float)sizeof(float);
    float rhs = C * L1_cache_size;
    return (lhs < rhs);
}
bool check_cond1_bis(int dimN_reg_block, int dimK_block, int dimK_reg_block,
        int dimM_block, int dimM_reg_block, int dimM_simd_block, float C)
{
    float lhs = (dimM_block * dimM_reg_block * dimK_block * dimK_reg_block
            * dimM_simd_block + dimK_block * dimN_reg_block * dimK_reg_block)
            * (float)sizeof(float);
    float rhs = C * L1_cache_size;
    return (lhs < rhs);
}
bool check_cond2(int nb_dimN_reg_block, int dimN_reg_block, int dimK_nb_block,
        int dimK_block, int dimK_reg_block, int dimM_block, int dimM_reg_block,
        int dimM_simd_block, float C)
{
    float lhs = (nb_dimN_reg_block * dimM_block * dimN_reg_block
                              * dimM_simd_block * dimM_reg_block
                      + dimK_nb_block * dimM_block * dimK_block * dimK_reg_block
                              * dimM_simd_block * dimM_reg_block
                      + nb_dimN_reg_block * dimK_nb_block * dimK_block
                              * dimN_reg_block * dimK_reg_block)
            * (float)sizeof(float);
    float rhs = C * L2_cache_size;
    return (lhs < rhs);
}

bool check_kernel_cond(int dimM_block, int dimM_reg_block, int dimM_simd_block,
        int dimN_block, int dimN_reg_block, int dimK, float C1, float C2)
{
    float A_size = dimM_block * dimM_reg_block * dimM_simd_block * dimK
        * (float)sizeof(float);
    float B_size = dimN_block * dimN_reg_block * dimK
        * (float)sizeof(float);
    return (A_size > C1 * L2_cache_size && B_size > C2 * L2_cache_size);
}
}

using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;
using namespace Xbyak;

void _jit_avx512_core_conv_winograd_data_kernel_f32::gemm_loop_generate()
{
    // for (int dimM_block =0; dimM_block < jcp.dimM_block; dimM_block++)
    // for (int dimM_reg_block =0; dimM_reg_block < jcp.dimM_reg_block;
    //      dimM_reg_block++) // unrolled
    //     for (int dimK_block = 0; dimK_block < jcp.dimK_block; dimK_block++)
    //         for (int dimK_reg_block= 0; dimK_reg_block < jcp.dimK_reg_block;
    //              dimK_reg_block++) // unrolled
    //             for (int tile =0; tile < jcp.dimN_reg_block; tile++)
    //                 C[dimM_block][dimM_reg_block][tile] +=
    //                 A[dimM_block][dimM_reg_block][dimK_block][dimK_reg_block]
    //                 * broadcast(B[dimK_block][tile][dimK_reg_block]);
    // Notes:
    // jcp.kernel_kind defines embedded or explicit broadcast
    // dimM_reg_block=1 for embedded bcast kernel

    auto zmm_srcA = [=]() {
        return Xbyak::Zmm(0);
    };
    auto zmm_srcB = [=](int tile) {
        int idx = 1 + tile;
        assert(idx < 1 + jcp.dimN_reg_block);
        return Xbyak::Zmm(idx);
    };
    auto zmm_dstC = [=](int dimM_reg_block, int tile) {
        int idx{0};
        if (jcp.kernel_kind == embd_bcast)
            idx = 1 + tile;
        else
            idx = 1 + jcp.dimN_reg_block
                  + dimM_reg_block * jcp.dimN_reg_block + tile;
        assert(idx < 32);
        return Xbyak::Zmm(idx);
    };

    auto prepare_output = [=]() {
        for (int dimM_reg_block = 0; dimM_reg_block < jcp.dimM_reg_block;
              dimM_reg_block++) {
            for (int tile = 0; tile < jcp.dimN_reg_block; tile++) {
                Zmm zmm = zmm_dstC(dimM_reg_block, tile);
                vpxord(zmm, zmm, zmm);
            }
        }
    };
    auto store_output = [=](bool output_is_aligned) {
        Label save;
        cmp(reg_is_beta_zero, 0);
        je(save, T_NEAR);

        for (int dimM_reg_block = 0; dimM_reg_block < jcp.dimM_reg_block;
              dimM_reg_block++) {
            for (int tile = 0; tile < jcp.dimN_reg_block; tile++) {
                Zmm zmm = zmm_dstC(dimM_reg_block,tile);
                int output_offset
                    = jcp.dimN_reg_block * dimM_reg_block * 64 + tile * 64;
                vaddps(zmm, zmm, EVEX_compress_addr(reg_dstC, output_offset));
            }
        }

        L(save);
        for (int dimM_reg_block = 0; dimM_reg_block < jcp.dimM_reg_block;
              dimM_reg_block++) {
            for (int tile = 0; tile < jcp.dimN_reg_block; tile++) {
                Zmm zmm = zmm_dstC(dimM_reg_block,tile);
                int output_offset
                    = jcp.dimN_reg_block * dimM_reg_block * 64 + tile * 64;

                // In W_SGD, output will be reused.
                if (output_is_aligned
                    && jcp.dimK_nb_block == 1
                    && jcp.sched_policy == WSCHED_DATA_W_S_G_D
                    && (jcp.dimN * jcp.dimM * alpha * alpha
                        * sizeof(float) > 2 * LLC_data_size))
                    vmovntps(EVEX_compress_addr(reg_dstC, output_offset), zmm);
                else vmovups(EVEX_compress_addr(reg_dstC, output_offset), zmm);
            }
        }
    };

    auto inner_loops = [=]() {
        Label dimM_block_loop, dimK_block_loop;

        if (jcp.dimM_block > 1) {
            mov(reg_dimM_block_loop_cnt, jcp.dimM_block);
            L(dimM_block_loop);
        }

        prepare_output();

        if (jcp.dimK_block > 1) {
            mov(reg_dimK_block_loop_cnt, jcp.dimK_block);
            L(dimK_block_loop);
        }

        for (int dimK_reg_block = 0;
                dimK_reg_block < jcp.dimK_reg_block;
                dimK_reg_block ++) {

            if (jcp.kernel_kind == expl_bcast) {
                for (int tile = 0; tile < jcp.dimN_reg_block; tile++) {
                    vbroadcastss(zmm_srcB(tile),
                        ptr[reg_srcB + 64 * tile + dimK_reg_block * 4]);
                }
            }

            /* Performing the fmas */

            for (int dimM_reg_block = 0; dimM_reg_block < jcp.dimM_reg_block;
                dimM_reg_block++) {

                vmovups(zmm_srcA(),
                    zword[reg_srcA
                            + jcp.dimK_reg_block * jcp.dimK_block * 64
                              * dimM_reg_block
                            + dimK_reg_block * 64]
                    );

                for (int tile = 0; tile < jcp.dimN_reg_block; tile++) {
                    if (jcp.kernel_kind == expl_bcast)
                        vfmadd231ps(zmm_dstC(dimM_reg_block, tile), zmm_srcA(),
                            zmm_srcB(tile));
                    else
                        vfmadd231ps(zmm_dstC(dimM_reg_block, tile), zmm_srcA(),
                            EVEX_compress_addr(reg_srcB,
                                64 * tile + dimK_reg_block * 4, true));
                }
            }
        }
        add(reg_srcA, jcp.dimK_reg_block * 64);
        add(reg_srcB, jcp.dimN_reg_block * 64);
        if (jcp.dimK_block > 1) {
            sub(reg_dimK_block_loop_cnt, 1);
            jnz(dimK_block_loop);
        }

        Label unaligned_store, end_store;
        test(reg_dstC, cpu_isa_traits<avx512_core>::vlen - 1);
        jnz(unaligned_store, T_NEAR);
        store_output(true);
        jmp(end_store, T_NEAR);
        L(unaligned_store); {
            store_output(false);
        }
        L(end_store);

        if (jcp.dimM_block > 1) {
            sub(reg_srcB, jcp.dimK_block * jcp.dimN_reg_block * 64);
            add(reg_dstC, jcp.dimM_reg_block * jcp.dimN_reg_block * 64);
            if (jcp.kernel_kind == expl_bcast) {
                add(reg_srcA,
                     (jcp.dimM_reg_block-1) * jcp.dimK_reg_block * 64
                      * jcp.dimK_block);
            }
            sub(reg_dimM_block_loop_cnt, 1);
            jnz(dimM_block_loop);
        }
    };

    /* Preamble */
    // register used to handle long fma encoding
    push(reg_EVEX_max_8b_offt);
    mov(reg_EVEX_max_8b_offt, 2 * EVEX_max_8b_offt);

    /* kernel */
    inner_loops();

    /* Postamble */
    pop(reg_EVEX_max_8b_offt);
    ret();
}

void _jit_avx512_core_conv_winograd_data_kernel_f32
    ::weights_transform_data_ker_generate()
{
    bool is_fwd = one_of(jcp.prop_kind,
        mkldnn_forward_training, mkldnn_forward_inference);
    int kh = jcp.kh;
    int kw = jcp.kw;

    auto zmm_temp = Xbyak::Zmm(31);
    auto zmm_zero = Xbyak::Zmm(30);

    auto zmm_M = [=](int i) {
        return Xbyak::Zmm(i);
    };
    auto zmm_MT = [=](int i) {
        return Xbyak::Zmm(i + simd_w);
    };

    auto zmm_G = [=](int i) {
        return Xbyak::Zmm(i);
    };
    auto zmm_F = [=](int i) {
        return Xbyak::Zmm(alpha + i);
    };
    auto zmm_T = [=](int i) {
        return Xbyak::Zmm(alpha + 3 + i);
    };
    auto zmm_t = [=](int i) {
        return Xbyak::Zmm(2 * alpha + 3 + i);
    };

    auto zmm_load = [=](int i) {
        return Xbyak::Zmm(i);
    };

    auto init_G = [=]() {
        mov(wreg_temp, ptr[param1 + GET_OFF(G)]);
        for (int i = 0; i < alpha; i++) {
            vbroadcastss(zmm_G(i), ptr[wreg_temp + i * typesize]);
        }
        vpxord(zmm_zero, zmm_zero, zmm_zero);
    };

    auto trans16x16 = [=]() {
        for (int i = 0; i < simd_w; i+=2 ) {
            vmovups(zmm_M(i), ptr[wreg_M + i * simd_w * 4]);
            vmovups(zmm_M(i+1), ptr[wreg_M + (i + 1) * simd_w * 4]);
            vunpcklps(zmm_MT(i), zmm_M(i), zmm_M(i+1));
            vunpckhps(zmm_MT(i+1), zmm_M(i), zmm_M(i+1));
        }
        for (int i = 0; i < simd_w; i+=4 ) {
            vunpcklpd(zmm_M(i), zmm_MT(i), zmm_MT(i+2));
            vunpckhpd(zmm_M(i+1), zmm_MT(i), zmm_MT(i+2));
            vunpcklpd(zmm_M(i+2), zmm_MT(i+1), zmm_MT(i+3));
            vunpckhpd(zmm_M(i+3), zmm_MT(i+1), zmm_MT(i+3));
        }
        for (int i = 0; i < simd_w; i += 8) {
            vshuff32x4(zmm_MT(i), zmm_M(i), zmm_M(i + 4), 0x88);
            vshuff32x4(zmm_MT(i+1), zmm_M(i+1), zmm_M(i + 5), 0x88);
            vshuff32x4(zmm_MT(i+2), zmm_M(i+2), zmm_M(i + 6), 0x88);
            vshuff32x4(zmm_MT(i+3), zmm_M(i+3), zmm_M(i + 7), 0x88);
            vshuff32x4(zmm_MT(i+4), zmm_M(i), zmm_M(i + 4), 0xdd);
            vshuff32x4(zmm_MT(i+5), zmm_M(i+1), zmm_M(i + 5), 0xdd);
            vshuff32x4(zmm_MT(i+6), zmm_M(i+2), zmm_M(i + 6), 0xdd);
            vshuff32x4(zmm_MT(i+7), zmm_M(i+3), zmm_M(i + 7), 0xdd);
        }
        {
            int i = 0;
            int mask = 0x88;
            vshuff32x4(zmm_M(0), zmm_MT(i), zmm_MT(i + 8), mask);
            vmovups(ptr[wreg_MT + 0 * 16 * 4], zmm_M(0));
            vshuff32x4(zmm_M(1), zmm_MT(i + 1), zmm_MT(i + 9), mask);
            vmovups(ptr[wreg_MT + 1 * 16 * 4], zmm_M(1));
            vshuff32x4(zmm_M(2), zmm_MT(i + 2), zmm_MT(i + 10), mask);
            vmovups(ptr[wreg_MT + 2 * 16 * 4], zmm_M(2));
            vshuff32x4(zmm_M(3), zmm_MT(i + 3), zmm_MT(i + 11), mask);
            vmovups(ptr[wreg_MT + 3 * 16 * 4], zmm_M(3));
            vshuff32x4(zmm_M(4), zmm_MT(i + 4), zmm_MT(i + 12), mask);
            vmovups(ptr[wreg_MT + 4 * 16 * 4], zmm_M(4));
            vshuff32x4(zmm_M(5), zmm_MT(i + 5), zmm_MT(i + 13), mask);
            vmovups(ptr[wreg_MT + 5 * 16 * 4], zmm_M(5));
            vshuff32x4(zmm_M(6), zmm_MT(i + 6), zmm_MT(i + 14), mask);
            vmovups(ptr[wreg_MT + 6 * 16 * 4], zmm_M(6));
            vshuff32x4(zmm_M(7), zmm_MT(i + 7), zmm_MT(i + 15), mask);
            vmovups(ptr[wreg_MT + 7 * 16 * 4], zmm_M(7));
            mask = 0xdd;
            vshuff32x4(zmm_M(8), zmm_MT(i), zmm_MT(i + 8), mask);
            vmovups(ptr[wreg_MT + 8 * 16 * 4], zmm_M(8));
            vshuff32x4(zmm_M(9), zmm_MT(i + 1), zmm_MT(i + 9), mask);
            vmovups(ptr[wreg_MT + 9 * 16 * 4], zmm_M(9));
            vshuff32x4(zmm_M(10), zmm_MT(i + 2), zmm_MT(i + 10), mask);
            vmovups(ptr[wreg_MT + 10 * 16 * 4], zmm_M(10));
            vshuff32x4(zmm_M(11), zmm_MT(i + 3), zmm_MT(i + 11), mask);
            vmovups(ptr[wreg_MT + 11 * 16 * 4], zmm_M(11));
            vshuff32x4(zmm_M(12), zmm_MT(i + 4), zmm_MT(i + 12), mask);
            vmovups(ptr[wreg_MT + 12 * 16 * 4], zmm_M(12));
            vshuff32x4(zmm_M(13), zmm_MT(i + 5), zmm_MT(i + 13), mask);
            vmovups(ptr[wreg_MT + 13 * 16 * 4], zmm_M(13));
            vshuff32x4(zmm_M(14), zmm_MT(i + 6), zmm_MT(i + 14), mask);
            vmovups(ptr[wreg_MT + 14 * 16 * 4], zmm_M(14));
            vshuff32x4(zmm_M(15), zmm_MT(i + 7), zmm_MT(i + 15), mask);
            vmovups(ptr[wreg_MT + 15 * 16 * 4], zmm_M(15));
        }
    };

    auto load_src = [=]() {
        mov(wreg_src, ptr[param1 + GET_OFF(src)]);
        mov(wreg_F, ptr[param1 + GET_OFF(M)]);
        for (int j = 0; j < kh; j++) {
            for (int i = 0; i < kw; i++) {
                if (is_fwd) {
                    for (int v1 = 0; v1 < simd_w; v1++) {
                        int offset_src = (j * kw * simd_w * simd_w
                            + i * simd_w * simd_w + v1 * simd_w) * typesize;
                        int offset_F = (j * kw * simd_w * simd_w
                            + i * simd_w * simd_w  + v1 * simd_w) * typesize;
                        vmovups(zmm_temp, ptr[wreg_src + offset_src]);
                        vmovups(ptr[wreg_F + offset_F], zmm_temp);
                    }
                } else {
                    int offset_src = ((2 - j) * kw * simd_w * simd_w
                        + (2 - i) * simd_w * simd_w) * typesize;
                    int offset_F = (j * kw * simd_w * simd_w
                        + i * simd_w * simd_w) * typesize;
                    lea(wreg_M, ptr[wreg_src + offset_src]);
                    lea(wreg_MT, ptr[wreg_F + offset_F]);
                    trans16x16();
                }
            }
        }
    };

    auto store_dst = [=]() {
        mov(wreg_dst, ptr[param1 + GET_OFF(dst)]);
        mov(wreg_Fw, ptr[param1 + GET_OFF(Mw)]);

        Label Loop_j;
        mov(wreg_cnt_j, 0);
        mov(wreg_dst_aux, wreg_dst);
        mov(wreg_Fw_aux, wreg_Fw);

        int dim5 = jcp.dimK_nb_block * (jcp.dimM_block * jcp.dimM_reg_block)
            * jcp.dimK_block * simd_w * simd_w;

        L(Loop_j);
        {
            for (int i = 0; i < alpha; i++) {
                // touch pages
                vmovups(zmm_load(0), ptr[wreg_Fw_aux
                    + (i * simd_w * simd_w) * typesize]);
                mov(wreg_dst_idx, i * dim5 * typesize);
                vmovntps(ptr[wreg_dst_aux + wreg_dst_idx], zmm_load(0));
            }
            for (int i = 0; i < alpha; i++) {
                for (int v1 = 1; v1 < simd_w; v1++) {
                    int offset_Fw = (i * simd_w * simd_w  + v1 * simd_w)
                        * typesize;
                    vmovups(zmm_load(v1), ptr[wreg_Fw_aux + offset_Fw]);
                }
                mov(wreg_dst_idx, i * dim5 * typesize);
                for (int v1 = 1; v1 < simd_w; v1++) {
                    int offset_dst = v1 * simd_w * typesize;
                    vmovntps(ptr[wreg_dst_aux + wreg_dst_idx + offset_dst],
                        zmm_load(v1));
                }
            }
            add(wreg_Fw_aux, alpha * simd_w * simd_w * typesize);
            add(wreg_dst_aux, alpha * dim5 * typesize);
            add(wreg_cnt_j, 1);
            cmp(wreg_cnt_j, alpha);
            jl(Loop_j, T_NEAR);
        }
    };

    auto trans_W_4x4_3x3 = [=]() {
        auto fma4 = [=](Zmm dst, Zmm a, Zmm b, Zmm c) {
            vmovups(dst, a);
            vfmadd231ps(dst, b, c);
        };
        auto fms4 = [=](Zmm dst, Zmm a, Zmm b, Zmm c) {
            vmulps(zmm_temp, b, c);
            vsubps(dst, a, zmm_temp);
        };
        auto fnms4 = [=](Zmm dst, Zmm a, Zmm b, Zmm c) {
            vsubps(dst, zmm_zero, a);
            vfnmadd231ps(dst, b, c);
        };

        mov(wreg_Fw, ptr[param1 + GET_OFF(Mw)]);
        mov(wreg_F, ptr[param1 + GET_OFF(M)]);
        mov(wreg_T, ptr[param1 + GET_OFF(T)]);

        Label Loop_j;
        mov(wreg_cnt_j, 0);
        L(Loop_j);
            mov(wreg_F_aux, wreg_F);
            mov(wreg_Fw_aux, wreg_Fw);
            mov(wreg_temp, wreg_cnt_j);
            shl(wreg_temp, 4 + 2);
            lea(wreg_F_aux, ptr[wreg_F + wreg_temp]);
            lea(wreg_Fw_aux, ptr[wreg_Fw + wreg_temp]);

            for (int i = 0; i < 3; i++) {
                for (int idx = 0; idx < 3; idx ++) {
                    vmovups(zmm_F(idx), ptr[wreg_F_aux + (idx * 3 * simd_w
                        * simd_w + i * simd_w * simd_w) * typesize]);
                }
                vmulps(zmm_t(0), zmm_G(0), zmm_F(2));
                fnms4(zmm_t(1), zmm_t(0), zmm_G(1), zmm_F(0));
                fma4(zmm_t(2), zmm_t(0), zmm_G(2), zmm_F(0));

                vmulps(zmm_T(0), zmm_G(3), zmm_F(0));
                fms4(zmm_T(1), zmm_t(1), zmm_G(4), zmm_F(1));
                fma4(zmm_T(2), zmm_t(1), zmm_G(4), zmm_F(1));
                fma4(zmm_T(3), zmm_t(2), zmm_G(5), zmm_F(1));
                fms4(zmm_T(4), zmm_t(2), zmm_G(5), zmm_F(1));
                vmovaps(zmm_T(5), zmm_F(2));

                for (int idx = 0; idx < 6; idx ++) {
                    vmovups(ptr[wreg_T + (idx * 3 * simd_w + i * simd_w)
                        * typesize], zmm_T(idx));
                }
            }
            for (int i = 0; i < 6; i++) {

                for (int idx = 0; idx < 3; idx ++) {
                    vmovups(zmm_T(idx), ptr[wreg_T
                        + (i * 3 * simd_w + idx * simd_w) * typesize]);
                }
                vmulps(zmm_t(0), zmm_G(0), zmm_T(2));
                fnms4(zmm_t(1), zmm_t(0), zmm_G(1), zmm_T(0));
                fma4(zmm_t(2), zmm_t(0), zmm_G(2), zmm_T(0));

                vmulps(zmm_F(0), zmm_G(3), zmm_T(0));
                fms4(zmm_F(1), zmm_t(1), zmm_G(4), zmm_T(1));
                fma4(zmm_F(2), zmm_t(1), zmm_G(4), zmm_T(1));
                fma4(zmm_F(3), zmm_t(2), zmm_G(5), zmm_T(1));
                fms4(zmm_F(4), zmm_t(2), zmm_G(5), zmm_T(1));
                vmovaps(zmm_F(5), zmm_T(2));

                for (int l = 0; l < 6; l++) {
                    vmovups(ptr[wreg_Fw_aux + (i * 6 * simd_w * simd_w
                        + l * simd_w * simd_w) * typesize], zmm_F(l));
                }
            }
        add(wreg_cnt_j, 1);
        cmp(wreg_cnt_j, 16);
        jl(Loop_j, T_NEAR);
    };

    auto inner_loops = [=]() {
        load_src();
        init_G();
        trans_W_4x4_3x3();
        store_dst();
    };

    preamble();
    inner_loops();
    postamble();
}

void _jit_avx512_core_conv_winograd_data_kernel_f32
    ::output_transform_data_ker_generate()
{
    bool is_fwd = one_of(jcp.prop_kind,
        mkldnn_forward_training, mkldnn_forward_inference);
    int outw = is_fwd ? jcp.ow : jcp.iw;
    int outh = is_fwd ? jcp.oh : jcp.ih;
    bool not_tiled = jcp.sched_policy == WSCHED_DATA_W_S_G_D;
    bool with_bias = jcp.with_bias;
    bool with_relu = jcp.with_relu;
    bool with_relu_postsum = jcp.with_relu_postsum;
    bool with_sum = jcp.with_sum;

    auto zmm_zero = Xbyak::Zmm(0);
    auto zmm_temp = Xbyak::Zmm(31);
    auto zmm_G = [=](int i) {
        return Xbyak::Zmm(1 + i);
    };
    auto zmm_O = [=](int i) {
        return Xbyak::Zmm(1 + alpha + i);
    };
    auto zmm_T = [=](int i) {
        return Xbyak::Zmm(1 + 2 * alpha + i);
    };
    auto zmm_t = [=](int i) {
        return Xbyak::Zmm(1 + 3 * alpha + i);
    };

    auto init_G = [=]() {
        mov(oreg_temp, ptr[param1 + GET_OFF(G)]);
        for (int i = 0; i < 6; i++) {
            vbroadcastss(zmm_G(i), ptr[oreg_temp + i * typesize]);
        }
    };

    auto load_src = [=]() {
        mov(oreg_Ow, ptr[param1 + GET_OFF(Mw)]);
        mov(oreg_src, ptr[param1 + GET_OFF(src)]);

        mov(oreg_nb_tile_block_ur, ptr[param1 + GET_OFF(nb_tile_block_ur)]);
        imul(oreg_nb_tile_block_ur, oreg_nb_tile_block_ur,
            (jcp.dimM_block * jcp.dimM_reg_block) * jcp.dimN_reg_block
            * jcp.dimM_simd_block * typesize);
        add(oreg_src, oreg_nb_tile_block_ur);

        mov(oreg_tile_block_ur, ptr[param1 + GET_OFF(tile_block_ur)]);
        imul(oreg_tile_block_ur, oreg_tile_block_ur,
            jcp.dimM_simd_block * typesize);
        add(oreg_src, oreg_tile_block_ur);

        if (not_tiled) {
            mov(oreg_tile_block, ptr[param1 + GET_OFF(tile_block)]);
            imul(oreg_tile_block, oreg_tile_block,
                jcp.dimM_nb_block * alpha * alpha * jcp.dimN_block
                * (jcp.dimM_block * jcp.dimM_reg_block) * jcp.dimN_reg_block
                * jcp.dimM_simd_block * typesize);
            add(oreg_src, oreg_tile_block);
        }

        int last4dim = jcp.dimN_block * (jcp.dimM_block * jcp.dimM_reg_block)
            * jcp.dimN_reg_block * jcp.dimM_simd_block * typesize;
        for (int j = 0; j < alpha; j++) {
            for (int i = 0; i < alpha; i++) {
                int j_base_offset = j * alpha * last4dim;
                int i_base_offset = i * last4dim;
                vmovups(zmm_temp, ptr[oreg_src + j_base_offset + i_base_offset]);
                vmovups(ptr[oreg_Ow + (j * alpha * simd_w + i * simd_w)
                    * typesize], zmm_temp);
            }
        }
    };

    auto store_dst = [=]() {
        vpxord(zmm_zero, zmm_zero, zmm_zero);
        mov(oreg_dst, ptr[param1 + GET_OFF(dst)]);
        mov(oreg_O, ptr[param1 + GET_OFF(M)]);
        mov(oreg_ydim, ptr[param1 + GET_OFF(tj)]);
        shl(oreg_ydim, 2); // tj * tile_size (==4)
        mov(oreg_xdim, ptr[param1 + GET_OFF(ti)]);
        shl(oreg_xdim, 2); // ti * tilesize (==4)

        if (with_bias)
            mov(oreg_bias, ptr[param1 + GET_OFF(bias)]);

        auto store_one = [=](int j, int i, bool is_aligned) {
            auto zmm_O = Xbyak::Zmm(31);
            auto zmm_relu_ns = Xbyak::Zmm(30);
            auto xmm_relu_ns = Xbyak::Xmm(30);
            int offset = (j * tile_size * simd_w + i * simd_w) * typesize;

            vmovups(zmm_O, ptr[oreg_O + offset]);
            if (is_fwd) {
                if (with_bias) {
                    vaddps(zmm_O, zmm_O, ptr[oreg_bias]);
                }
                if (with_relu) {
                    Opmask kmask = Opmask(7);
                    if (jcp.relu_negative_slope == 0) {
                        zmm_relu_ns = zmm_zero;
                    } else {
                        mov(imm_addr64, float2int(jcp.relu_negative_slope));
                        vmovq(xmm_relu_ns, imm_addr64);
                        vbroadcastss(zmm_relu_ns, xmm_relu_ns);
                    }
                    vcmpps(kmask, zmm_O, zmm_zero, _cmp_lt_os);
                    vmulps(zmm_O | kmask, zmm_O, zmm_relu_ns);
                }
            }
            if (with_sum) {
                vaddps(zmm_O, zmm_O, ptr[oreg_out_j + oreg_temp]);
                if (with_relu_postsum) // orig: with_relu_postsum
                    vmaxps(zmm_O, zmm_O, zmm_zero);
            }
            if (is_aligned)
                vmovntps(ptr[oreg_out_j + oreg_temp], zmm_O);
            else
                vmovups(ptr[oreg_out_j + oreg_temp], zmm_O);
        };

        auto i_loop = [=](int j, bool is_aligned) {
            for (int i = 0; i < tile_size; i++) {
                Label next;
                mov(oreg_temp, oreg_xdim);
                add(oreg_temp, i);
                cmp(oreg_temp, outw);
                jge(next, T_NEAR);
                shl(oreg_temp, 4 + 2); // * 16 * 4

                store_one(j, i, is_aligned);

                L(next);
            }
        };


        for (int j = 0; j < tile_size; j++) {
            Label next, unaligned;
            mov(oreg_temp, oreg_ydim);
            add(oreg_temp, j);
            cmp(oreg_temp, outh);
            jge(next, T_NEAR);

            mov(oreg_out_j, oreg_dst);
            imul(oreg_temp, oreg_temp, outw * simd_w * typesize);
            add(oreg_out_j, oreg_temp);

            test(oreg_dst, 63);
            jnz(unaligned, T_NEAR);

            i_loop(j, true);
            jmp(next, T_NEAR);

            L(unaligned);
            i_loop(j, false);

            L(next);
        }
    };

    auto trans_O_4x4_3x3 = [=]() {
        auto fma2 = [=](Zmm dst, Zmm v1, Zmm u1, Zmm v2, Zmm u2){
            vmulps(dst, v1, u1);
            vfmadd231ps(dst, v2, u2);
        };
        mov(oreg_Ow, ptr[param1 + GET_OFF(Mw)]);
        mov(oreg_T, ptr[param1 + GET_OFF(T)]);
        mov(oreg_O, ptr[param1 + GET_OFF(M)]);

        for (int i = 0; i < alpha; i++) {
            for (int j = 0; j < alpha; j++) {
                vmovups(zmm_O(j), ptr[oreg_Ow + (j * alpha * simd_w
                    + i * simd_w) * typesize]);
            }

            vaddps(zmm_t(0), zmm_O(1), zmm_O(2));
            vaddps(zmm_t(1), zmm_O(3), zmm_O(4));
            vsubps(zmm_t(2), zmm_O(1), zmm_O(2));
            vsubps(zmm_t(3), zmm_O(3), zmm_O(4));

            vaddps(zmm_T(0), zmm_t(0), zmm_t(1));
            vaddps(zmm_T(0), zmm_T(0), zmm_O(0));
            fma2(zmm_T(1), zmm_t(2), zmm_G(0), zmm_t(3), zmm_G(1));
            fma2(zmm_T(2), zmm_t(0), zmm_G(2), zmm_t(1), zmm_G(3));
            fma2(zmm_T(3), zmm_t(2), zmm_G(4), zmm_t(3), zmm_G(5));
            vaddps(zmm_T(3), zmm_T(3), zmm_O(5));

            for (int j = 0; j < tile_size; j++) {
                vmovups(ptr[oreg_T + (j * alpha * simd_w
                    + i * simd_w) * typesize], zmm_T(j));
            }
        }
        for (int j = 0; j < tile_size; j++) {
            for (int i = 0; i < alpha; i++) {
                vmovups(zmm_T(i), ptr[oreg_T + (j * alpha * simd_w
                    + i * simd_w) * typesize]);
            }
            vaddps(zmm_t(0), zmm_T(1), zmm_T(2));
            vaddps(zmm_t(1), zmm_T(3), zmm_T(4));
            vsubps(zmm_t(2), zmm_T(1), zmm_T(2));
            vsubps(zmm_t(3), zmm_T(3), zmm_T(4));

            vaddps(zmm_O(0), zmm_t(0), zmm_t(1));
            vaddps(zmm_O(0), zmm_O(0), zmm_T(0));
            fma2(zmm_O(1), zmm_t(2), zmm_G(0), zmm_t(3), zmm_G(1));
            fma2(zmm_O(2), zmm_t(0), zmm_G(2), zmm_t(1), zmm_G(3));
            fma2(zmm_O(3), zmm_t(2), zmm_G(4), zmm_t(3), zmm_G(5));
            vaddps(zmm_O(3), zmm_O(3), zmm_T(5));

            for (int i = 0; i < tile_size; i++) {
                vmovups(ptr[oreg_O + (j * tile_size * simd_w
                    + i * simd_w) * typesize], zmm_O(i));
            }
        }
    };

    auto inner_loops = [=]() {
        init_G();
        load_src();
        trans_O_4x4_3x3();
        store_dst();
    };

    preamble();
    inner_loops();
    postamble();
}

void _jit_avx512_core_conv_winograd_data_kernel_f32
    ::input_transform_data_ker_generate()
{
    bool is_fwd = one_of(jcp.prop_kind,
        mkldnn_forward_training, mkldnn_forward_inference);
    int inpw = is_fwd ? jcp.iw : jcp.ow;
    int inph = is_fwd ? jcp.ih : jcp.oh;
    int l_pad = is_fwd ? jcp.l_pad : jcp.iw + jcp.r_pad - jcp.ow;
    int t_pad = is_fwd ? jcp.t_pad : jcp.ih + jcp.t_pad - jcp.oh;
    int wp_max = inpw + l_pad;
    int hp_max = inph + t_pad;
    bool not_tiled = jcp.sched_policy == WSCHED_DATA_W_S_G_D;
    int G_size = 9;

    auto zmm_zero = Xbyak::Zmm(0);
    auto zmm_temp = Xbyak::Zmm(31);
    auto zmm_G = [=](int i) {
        return Xbyak::Zmm(1 + i);
    };
    auto zmm_I = [=](int i) {
        return Xbyak::Zmm(1 + G_size + i);
    };
    auto zmm_T = [=](int i) {
        return Xbyak::Zmm(1 + G_size + alpha + i);
    };
    auto zmm_t = [=](int i) {
        return Xbyak::Zmm(1 + G_size + 2 * alpha + i);
    };

    auto init_G = [=]() {
        mov(ireg_temp, ptr[param1 + GET_OFF(G)]);
        for (int i = 0; i < G_size; i++) {
            vbroadcastss(zmm_G(i), ptr[ireg_temp + i * typesize]);
        }
    };

    auto load_src = [=]() {
        mov(ireg_src, ptr[param1 + GET_OFF(src)]); // base addr of inp
        mov(ireg_I, ptr[param1 + GET_OFF(M)]);

        xor_(ireg_zero,  ireg_zero);
        vpxord(zmm_zero, zmm_zero, zmm_zero);

        mov(ireg_ydim, ptr[param1 + GET_OFF(tj)]);
        shl(ireg_ydim, 2); // tj * tile_size (==4)
        mov(ireg_xdim, ptr[param1 + GET_OFF(ti)]);
        shl(ireg_xdim, 2); // ti * tilesize (==4)

        for (int j = 0; j < alpha; j++) {
            mov(ireg_temp, ireg_ydim);
            add(ireg_temp, j);

            mov(ireg_mask_j, 0xffff);
            cmp(ireg_temp, t_pad);
            cmovl(ireg_mask_j, ireg_zero);
            cmp(ireg_temp, hp_max);
            cmovge(ireg_mask_j, ireg_zero);

            sub(ireg_temp, t_pad);
            imul(ireg_temp, ireg_temp, inpw * simd_w * typesize);
            mov(ireg_inp_j, ireg_src);
            add(ireg_inp_j, ireg_temp);

            for (int i = 0; i < alpha; i++) {

                mov(ireg_temp, ireg_xdim);
                add(ireg_temp, i);

                mov(ireg_mask, 0xffff);
                cmp(ireg_temp, l_pad);
                cmovl(ireg_mask, ireg_zero);
                cmp(ireg_temp, wp_max);
                cmovge(ireg_mask, ireg_zero);
                and_(ireg_mask, ireg_mask_j);

                sub(ireg_temp, l_pad);
                shl(ireg_temp, 4 + 2);

                vpxord(zmm_temp, zmm_temp, zmm_temp);
                Opmask kmask = Opmask(7);
                kmovw(kmask, ireg_mask_32);
                vmovups(zmm_temp | kmask, ptr[ireg_inp_j + ireg_temp]);
                vmovups(ptr[ireg_I + (j * alpha * simd_w + i * simd_w)
                    * typesize], zmm_temp);
            }
        }
    };

    auto store_Iw = [=]() {

        mov(ireg_Iw, ptr[param1 + GET_OFF(Mw)]);
        mov(ireg_output, ptr[param1 + GET_OFF(dst)]);

       bool streamout
          = jcp.dimN * jcp.dimK * alpha * alpha * sizeof(float)
            > 2 * LLC_data_size
            ? true : false;

        if (not_tiled) {
            mov(ireg_tile_block, ptr[param1 + GET_OFF(tile_block)]);
            imul(ireg_tile_block, ireg_tile_block,
                alpha * alpha * jcp.dimN_block * jcp.dimK_nb_block
                * jcp.dimK_block * jcp.dimN_reg_block * jcp.dimK_reg_block
                * typesize);
        }

        mov(ireg_nb_tile_block_ur, ptr[param1 + GET_OFF(nb_tile_block_ur)]);
        imul(ireg_nb_tile_block_ur, ireg_nb_tile_block_ur,
            jcp.dimK_nb_block * jcp.dimK_block * jcp.dimN_reg_block
            * jcp.dimK_reg_block * typesize);

        mov(ireg_tile_block_ur, ptr[param1 + GET_OFF(tile_block_ur)]);
        imul(ireg_tile_block_ur, ireg_tile_block_ur,
            jcp.dimK_reg_block * typesize);

        add(ireg_output, ireg_nb_tile_block_ur);
        add(ireg_output, ireg_tile_block_ur);
        if (not_tiled)
            add(ireg_output, ireg_tile_block);

        for (int j = 0; j < alpha; j++) {
            for (int i = 0; i < alpha; i++) {
                vmovups(zmm_temp,ptr[ireg_Iw + (j * alpha * simd_w
                    + i * simd_w) * typesize]);

                int j_base_offset =
                    j * alpha * jcp.dimN_block * jcp.dimK_nb_block
                    * jcp.dimK_block * jcp.dimN_reg_block * jcp.dimK_reg_block
                    * typesize;
                int i_base_offset =
                    i * jcp.dimN_block * jcp.dimK_nb_block * jcp.dimK_block
                    * jcp.dimN_reg_block * jcp.dimK_reg_block * typesize;

                if (not_tiled && streamout)
                    vmovntps(ptr[ireg_output + j_base_offset + i_base_offset],
                        zmm_temp);
                else
                    vmovups(ptr[ireg_output + j_base_offset + i_base_offset],
                        zmm_temp);
            }
        }
    };

    auto fma4 = [=](Zmm dst, Zmm a, Zmm b, Zmm c) {
        vmulps(zmm_temp, a, b);
        vaddps(dst, zmm_temp, c);
    };

    auto trans_I_4x4_3x3 = [=]() {
        mov(ireg_Iw, ptr[param1 + GET_OFF(Mw)]);
        mov(ireg_T, ptr[param1 + GET_OFF(T)]);
        mov(ireg_I, ptr[param1 + GET_OFF(M)]);

        mov(ireg_output, ptr[param1 + GET_OFF(dst)]); // for prefetch
        for (int i = 0; i < alpha; i++) {
            for (int idx = 0; idx < alpha; idx++) {
                vmovups(zmm_I(idx), ptr[ireg_I + (idx * alpha * simd_w
                    + i * simd_w) * typesize]);
                int j_base_offset =
                    i * alpha * jcp.dimN_block * jcp.dimK_nb_block
                    * jcp.dimK_block * jcp.dimN_reg_block * jcp.dimK_reg_block
                    * typesize;
                int idx_base_offset =
                    idx * jcp.dimN_block * jcp.dimK_nb_block * jcp.dimK_block
                    * jcp.dimN_reg_block * jcp.dimK_reg_block * typesize;
                prefetcht0(ptr[ireg_output + j_base_offset + idx_base_offset]);
            }

            fma4(zmm_t(0), zmm_I(2), zmm_G(0), zmm_I(4));
            fma4(zmm_t(1), zmm_I(1), zmm_G(0), zmm_I(3));
            fma4(zmm_t(2), zmm_I(2), zmm_G(1), zmm_I(4));
            fma4(zmm_t(3), zmm_I(1), zmm_G(1), zmm_I(3));
            fma4(zmm_t(4), zmm_I(0), zmm_G(2), zmm_I(4));
            fma4(zmm_t(5), zmm_I(1), zmm_G(2), zmm_I(5));

            fma4(zmm_T(0), zmm_I(2), zmm_G(3), zmm_t(4));
            fma4(zmm_T(1), zmm_t(1), zmm_G(4), zmm_t(0));
            fma4(zmm_T(2), zmm_t(1), zmm_G(5), zmm_t(0));
            fma4(zmm_T(3), zmm_t(3), zmm_G(6), zmm_t(2));
            fma4(zmm_T(4), zmm_t(3), zmm_G(7), zmm_t(2));
            fma4(zmm_T(5), zmm_I(3), zmm_G(8), zmm_t(5));

            for (int idx = 0; idx < alpha; idx++) {
                vmovups(ptr[ireg_T + (idx * alpha * simd_w + i * simd_w)
                    * typesize],zmm_T(idx));
            }
        }
        for (int i = 0; i < alpha; i++) {
            for (int idx = 0; idx < alpha; idx++) {
                vmovups(zmm_T(idx), ptr[ireg_T + (i * alpha * simd_w + idx
                    * simd_w) * typesize]);
            }

            fma4(zmm_t(0), zmm_T(2), zmm_G(0), zmm_T(4));
            fma4(zmm_t(1), zmm_T(1), zmm_G(0), zmm_T(3));
            fma4(zmm_t(2), zmm_T(2), zmm_G(1), zmm_T(4));
            fma4(zmm_t(3), zmm_T(1), zmm_G(1), zmm_T(3));
            fma4(zmm_t(4), zmm_T(0), zmm_G(2), zmm_T(4));
            fma4(zmm_t(5), zmm_T(1), zmm_G(2), zmm_T(5));

            fma4(zmm_I(0), zmm_T(2), zmm_G(3), zmm_t(4));
            fma4(zmm_I(1), zmm_t(1), zmm_G(4), zmm_t(0));
            fma4(zmm_I(2), zmm_t(1), zmm_G(5), zmm_t(0));
            fma4(zmm_I(3), zmm_t(3), zmm_G(6), zmm_t(2));
            fma4(zmm_I(4), zmm_t(3), zmm_G(7), zmm_t(2));
            fma4(zmm_I(5), zmm_T(3), zmm_G(8), zmm_t(5));

            for (int idx = 0; idx < alpha; idx++) {
                vmovups(ptr[ireg_Iw + (i * alpha * simd_w + idx * simd_w)
                    * typesize],zmm_I(idx));
            }
        }
    };

    auto inner_loops = [=]() {
        init_G();
        load_src();
        trans_I_4x4_3x3();
        store_Iw();
    };

    preamble();
    inner_loops();
    postamble();
}

status_t _jit_avx512_core_conv_winograd_data_kernel_f32::init_conf_common(
        jit_conv_winograd_conf_t &jcp, const convolution_desc_t &cd,
        const memory_desc_wrapper &src_d, const memory_desc_wrapper &weights_d,
        const memory_desc_wrapper &dst_d)
{
    if (!mayiuse(avx512_core)) {
        return status::unimplemented;
    }
    jcp.ver = ver_avx512_core;
    jcp.prop_kind = cd.prop_kind;

    const bool with_groups = weights_d.ndims() == src_d.ndims() + 1;

    jcp.ngroups = with_groups ? weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];
    jcp.oc = dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;
    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = dst_d.dims()[2];
    jcp.ow = dst_d.dims()[3];
    jcp.kh = weights_d.dims()[with_groups + 2];
    jcp.kw = weights_d.dims()[with_groups + 3];
    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];
    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];
    jcp.dilate_h = cd.dilates[0];
    jcp.dilate_w = cd.dilates[1];
    jcp.r_pad = nstl::max(
            0, (jcp.ow - 1) * jcp.stride_w + jcp.kw - jcp.iw - jcp.l_pad);
    jcp.b_pad = nstl::max(
            0, (jcp.oh - 1) * jcp.stride_h + jcp.kh - jcp.ih - jcp.t_pad);
    jcp.ihp = jcp.ih + jcp.t_pad + jcp.b_pad;
    jcp.iwp = jcp.iw + jcp.l_pad + jcp.r_pad;
    jcp.ohp = jcp.oh;
    jcp.owp = jcp.ow;

    // Checking conditions not supported by these kernels
    if (jcp.ngroups != 1)
        return status::unimplemented;
    if ((jcp.kh != 3) || (jcp.kw != 3))
        return status::unimplemented;
    if ((jcp.dilate_h != 0) || (jcp.dilate_w != 0))
        return status::unimplemented;
    if ((jcp.stride_h != 1) || (jcp.stride_w != 1))
        return status::unimplemented;
    if ((jcp.ic % simd_w) != 0 || (jcp.oc % simd_w) != 0)
        return status::unimplemented;

    if (src_d.format() != nChw16c)
        return status::unimplemented;
    if (weights_d.format() != (with_groups ? gOIhw16i16o : OIhw16i16o))
        return status::unimplemented;
    if (dst_d.format() != nChw16c)
        return status::unimplemented;

    return status::success;
}

void set_kernel_dims_reg_block(jit_conv_winograd_conf_t &jcp) {

    /* ----------- dimM reg block ---------------------*/
    auto test_cond_dimM_reg_block = [](jit_conv_winograd_conf_t &jcp,
            int dimM_reg_block, int current_best) {
        int max_dimM_reg_block = jcp.kernel_kind == embd_bcast ? 1 : 4;
        return (dimM_reg_block >= 1)
                && (dimM_reg_block <= max_dimM_reg_block )
                && (dimM_reg_block > current_best);
    };
    jcp.dimM_reg_block = get_divisor_satisfying_cond(jcp,
        jcp.dimM/jcp.dimM_simd_block, 1, test_cond_dimM_reg_block);

    /* ----------- dimN reg block ---------------------*/

    auto test_cond_dimN_reg_block = [](jit_conv_winograd_conf_t &jcp,
            int dimN_reg_block, int current_best) {
        return jcp.kernel_kind == embd_bcast
            ? dimN_reg_block < jcp.nb_reg && dimN_reg_block > current_best
            : dimN_reg_block >= 1
              && (dimN_reg_block * jcp.dimM_reg_block + dimN_reg_block)
                 < jcp.nb_reg
              && dimN_reg_block > current_best;
    };
    jcp.dimN_reg_block = get_divisor_satisfying_cond(jcp,
        jcp.dimN, 1, test_cond_dimN_reg_block);
}

status_t set_wsched_DATA_W_SGD_avx512_core(jit_conv_winograd_conf_t &jcp) {
    if (jcp.ver != ver_avx512_core)
        return status::unimplemented;

    jcp.kernel_kind = embd_bcast;

    set_kernel_dims_reg_block(jcp);

    /*-------------- L2 blocking for dimN block ---------*/

    auto test_cond_dimN_block = [](jit_conv_winograd_conf_t &jcp,
        int dimN_block, int current_best) {
        return check_L2_block_per_thread(jcp, dimN_block, 0.1, 2.0)
            && (dimN_block > current_best)
            && ((jcp.dimN / dimN_block / jcp.dimN_reg_block)
            >= 1.5 * omp_get_max_threads());
    };

    jcp.dimN_block = get_divisor_satisfying_cond(
            jcp, jcp.dimN / jcp.dimN_reg_block, 1, test_cond_dimN_block);
    jcp.dimN_nb_block = jcp.dimN / jcp.dimN_block / jcp.dimN_reg_block;

    if (check_L2_block_per_thread(jcp, jcp.dimN_block, 0.1, 3.2)
        && (jcp.dimN_nb_block >= 1.5 * omp_get_max_threads())) {

        /* ------------------- L1 blocking for GEMM --------------*/
        /* -------------------- Choose dimK block ----------------*/

        auto test_cond_dimK_block = [](jit_conv_winograd_conf_t &jcp,
                int dimK_block, int current_best) {
            return check_L1_block_gemm(jcp, dimK_block, 1, 0.1, 0.5)
                && (dimK_block > current_best);
        };

        jcp.dimK_block = get_divisor_satisfying_cond(
                jcp, jcp.dimK / jcp.dimK_reg_block, 1, test_cond_dimK_block);

        if (check_L1_block_gemm(jcp, jcp.dimK_block, 1, 0.1, 1.0)) {
            jcp.dimK_nb_block = jcp.dimK / jcp.dimK_block / jcp.dimK_reg_block;

            /* -------------- Choose dimM block -------------------*/
            auto test_cond_dimM_block = [](jit_conv_winograd_conf_t &jcp,
                    int dimM_block, int current_best) {
                return check_L1_block_gemm(jcp, jcp.dimK_block, dimM_block,
                    0.2, 0.5) && (dimM_block > current_best);
            };

            jcp.dimM_block = get_divisor_satisfying_cond(jcp,
                jcp.dimM / (jcp.dimM_simd_block * jcp.dimM_reg_block), 1,
                test_cond_dimM_block);
            jcp.dimM_nb_block = jcp.dimM / jcp.dimM_block / jcp.dimM_reg_block
                / jcp.dimM_simd_block;

            jcp.sched_policy = WSCHED_DATA_W_SGD;
            return status::success;
        }

    }
    return status::unimplemented;
}

void set_kernel_blocking_DATA_W_S_G_D(jit_conv_winograd_conf_t &jcp) {

    set_kernel_dims_reg_block(jcp);

    //********************* Choosing dimK_block **********************//
    auto test_cond1_dimK_block = [](
            jit_conv_winograd_conf_t &jcp, int dimK_block, int current_best) {
        return check_cond1(jcp.dimN_reg_block, dimK_block, jcp.dimK_reg_block,
                       1, jcp.dimM_reg_block, jcp.dimM_simd_block, .75f)
                && (dimK_block > current_best);
    };

    auto test_cond1_bis_dimK_block = [](
            jit_conv_winograd_conf_t &jcp, int dimK_block, int current_best) {
        return check_cond1_bis(jcp.dimN_reg_block, dimK_block,
                   jcp.dimK_reg_block, 1, jcp.dimM_reg_block,
                   jcp.dimM_simd_block, .9f)
                && (dimK_block > current_best);
    };

    jcp.dimK_block = get_divisor_satisfying_cond(
            jcp, jcp.dimK / jcp.dimK_reg_block, 1, test_cond1_bis_dimK_block);
    // If we are not able to use streams, we fall back to condition [1]
    if (jcp.dimK_block < jcp.dimK / jcp.dimK_reg_block)
        jcp.dimK_block = get_divisor_satisfying_cond(
                jcp, jcp.dimK / jcp.dimK_reg_block, 1, test_cond1_dimK_block);
    jcp.dimK_nb_block = (jcp.dimK / jcp.dimK_reg_block) / jcp.dimK_block;

    //********************* Choosing dimM_block **********************//
    auto test_cond1_dimM_block = [](
            jit_conv_winograd_conf_t &jcp, int dimM_block, int current_best) {
        return check_cond1(jcp.dimN_reg_block, jcp.dimK_block,
                   jcp.dimK_reg_block, dimM_block, jcp.dimM_reg_block,
                   jcp.dimM_simd_block, .5f)
                && (dimM_block > current_best);
    };

    auto test_cond1_bis_dimM_block = [](
            jit_conv_winograd_conf_t &jcp, int dimM_block, int current_best) {
        return check_cond1_bis(jcp.dimN_reg_block, jcp.dimK_block,
                   jcp.dimK_reg_block, dimM_block, jcp.dimM_reg_block,
                   jcp.dimM_simd_block, .3f)
                && (dimM_block > current_best);
    };

    if (jcp.dimK_block < jcp.dimK / jcp.dimK_reg_block)
        jcp.dimM_block = get_divisor_satisfying_cond(
                jcp, jcp.dimM / (jcp.dimM_simd_block*jcp.dimM_reg_block), 1,
                test_cond1_dimM_block);
    else
        jcp.dimM_block = get_divisor_satisfying_cond(jcp,
                jcp.dimM / (jcp.dimM_simd_block*jcp.dimM_reg_block), 1,
                test_cond1_bis_dimM_block);
    jcp.dimM_nb_block = jcp.dimM / (jcp.dimM_simd_block * jcp.dimM_block
                        * jcp.dimM_reg_block);

    //******************* Choosing dimN_block *******************//
    auto test_cond2_dimN_block = [](
            jit_conv_winograd_conf_t &jcp, int dimN_block, int current_best) {
        return check_cond2(dimN_block, jcp.dimN_reg_block, jcp.dimK_nb_block,
                       jcp.dimK_block, jcp.dimK_reg_block, jcp.dimM_block,
                       jcp.dimM_reg_block, jcp.dimM_simd_block, .9f)
                && (dimN_block > current_best);
    };

    jcp.dimN_block = get_divisor_satisfying_cond(
            jcp, jcp.dimN / jcp.dimN_reg_block, 1, test_cond2_dimN_block);
    jcp.dimN_nb_block = jcp.dimN / (jcp.dimN_reg_block * jcp.dimN_block);
}

status_t set_wsched_DATA_W_S_G_D_avx512_core(jit_conv_winograd_conf_t &jcp) {

    jcp.kernel_kind = expl_bcast;
    set_kernel_blocking_DATA_W_S_G_D(jcp);
    if (!(check_kernel_cond(jcp.dimM_block, jcp.dimM_reg_block,
        jcp.dimM_simd_block, jcp.dimN_block, jcp.dimN_reg_block, jcp.dimK,
        .1f, .35f))) {
        jcp.kernel_kind = embd_bcast;
        set_kernel_blocking_DATA_W_S_G_D(jcp);
    }
    jcp.sched_policy = WSCHED_DATA_W_S_G_D;
    return status::success;
}

status_t _jit_avx512_core_conv_winograd_data_kernel_f32::init_conf_kernel(
        jit_conv_winograd_conf_t &jcp, int dimM, int dimN, int dimK)
{
    jcp.nb_reg = 32;
    jcp.dimN = dimN;
    jcp.dimK = dimK;
    jcp.dimM = dimM;
    jcp.sched_policy = WSCHED_INVALID;

    jcp.dimK_reg_block = 16;
    jcp.dimM_simd_block = 16;

    if (jcp.kernel_kind == embd_bcast) {
        jcp.dimM_reg_block = 1;
    }

    if (!(set_wsched_DATA_W_SGD_avx512_core(jcp) == status::success))
        set_wsched_DATA_W_S_G_D_avx512_core(jcp);

    assert(jcp.sched_policy != WSCHED_INVALID);
    return status::success;
}

bool jit_avx512_core_conv_winograd_fwd_kernel_f32::post_ops_ok(
        jit_conv_conf_t &jcp, const primitive_attr_t &attr) {
    const auto &p = attr.post_ops_;

    auto is_relu = [&](int idx) { return p.entry_[idx].is_relu(); };
    auto is_sum = [&](int idx) { return p.entry_[idx].is_sum(); };

    switch (p.len_) {
    case 0:
        return true; // no post_ops
    case 1:
        return true // relu or sum
                && implication(jcp.with_relu, is_sum(0))
                && implication(!jcp.with_relu, is_relu(0) || is_sum(0));
    case 2:
        return true // sum->relu or relu->sum
                && implication(jcp.with_relu, is_sum(0) && is_relu(1))
                && implication(!jcp.with_relu, false
                                   || (is_sum(0) && is_relu(1))
                                   || (is_relu(0) && is_sum(1)));
    case 3:
        return true // relu->sum->relu
                && jcp.with_relu == false
                && (is_relu(0) && is_sum(1) && is_relu(2));
    default:
        return false;
    }

    return false;
}

status_t jit_avx512_core_conv_winograd_fwd_kernel_f32::init_conf(
        jit_conv_winograd_conf_t &jcp, const convolution_desc_t &cd,
        const memory_desc_wrapper &src_d, const memory_desc_wrapper &weights_d,
        const memory_desc_wrapper &dst_d, const primitive_attr_t &attr,
        bool with_relu, float relu_negative_slope) {
    status_t st = init_conf_common(jcp, cd, src_d, weights_d, dst_d);

    if (st != status::success)
        return st;

    // Winograd specific initialization
    jcp.itiles = (jcp.ow + tile_size - 1) / tile_size;
    jcp.jtiles = (jcp.oh + tile_size - 1) / tile_size;
    jcp.ntiles = jcp.mb * jcp.itiles * jcp.jtiles;

    jcp.with_bias = cd.bias_desc.format != memory_format::undef;
    jcp.with_relu = with_relu;
    jcp.relu_negative_slope = relu_negative_slope;

    if (!post_ops_ok(jcp, attr))
        return status::unimplemented;

    const auto &p = attr.post_ops_;
    if (!jcp.with_relu) {
        /* PostOps ReLU before SUM is handled the same as ReLU primitive */
        jcp.with_relu = p.find(primitive_kind::eltwise, 0, 1) != -1;
        jcp.relu_negative_slope = 0.f;
    }
    jcp.with_sum = p.find(primitive_kind::sum, 0) != -1;
    jcp.with_relu_postsum = p.find(primitive_kind::eltwise, 1) != -1;

    status_t res = init_conf_kernel(jcp, jcp.oc, jcp.ntiles, jcp.ic);

    jcp.ic_simd_block = jcp.dimK_reg_block;
    jcp.ic_block = jcp.dimK_block;
    jcp.nb_ic = jcp.dimK_nb_block;
    jcp.oc_simd_block = jcp.dimM_simd_block;
    jcp.oc_block = jcp.dimM_block;
    jcp.oc_reg_block = jcp.dimM_reg_block;
    jcp.ic_reg_block = 1;
    jcp.nb_oc = jcp.dimM_nb_block;
    jcp.tile_block_ur = jcp.dimN_reg_block;
    jcp.nb_tile_block_ur = jcp.dimN_block;
    jcp.tile_block = jcp.dimN_nb_block;

    return res;
}

status_t jit_avx512_core_conv_winograd_bwd_data_kernel_f32::init_conf(
        jit_conv_winograd_conf_t &jcp, const convolution_desc_t &cd,
        const memory_desc_wrapper &diff_src_d,
        const memory_desc_wrapper &weights_d,
        const memory_desc_wrapper &diff_dst_d)
{
    status_t st = init_conf_common(jcp, cd, diff_src_d, weights_d, diff_dst_d);

    if (st != status::success)
        return st;

    jcp.itiles = (jcp.iw + tile_size - 1) / tile_size;
    jcp.jtiles = (jcp.ih + tile_size - 1) / tile_size;
    jcp.ntiles = jcp.mb * jcp.itiles * jcp.jtiles;

    status_t res = init_conf_kernel(jcp, jcp.ic, jcp.ntiles, jcp.oc);

    jcp.oc_simd_block = jcp.dimK_reg_block;
    jcp.oc_block = jcp.dimK_block;
    jcp.nb_oc = jcp.dimK_nb_block;
    jcp.ic_simd_block = jcp.dimM_simd_block;
    jcp.ic_block = jcp.dimM_block;
    jcp.ic_reg_block = jcp.dimM_reg_block;
    jcp.oc_reg_block = 1;
    jcp.nb_ic = jcp.dimM_nb_block;
    jcp.tile_block_ur = jcp.dimN_reg_block;
    jcp.nb_tile_block_ur = jcp.dimN_block;
    jcp.tile_block = jcp.dimN_nb_block;

    return res;
}

void jit_avx512_core_conv_winograd_bwd_weights_kernel_f32::transpose_ker_generate()
{
    auto load_B = [=](int reg_idx, int offset) {
        for (int i = 0; i < 4; i++) {
            vmovups(Zmm(reg_idx + i),
                zword[reg_origB
                      + (offset + i) * jcp.dimN_reg_block * sizeof(float)]);
        }
    };

    int curr = 0;
    for (int j = 0; j < alpha; j++) {
        for (int i = 0; i < alpha; i++) {
            int origB_offset = (j * alpha + i) * jcp.dimK_4fma;
            int transB_offset = (j * alpha + i) * jcp.dimK_nb_block *
                jcp.dimN_block * jcp.dimK_block * jcp.dimK_reg_block *
                jcp.dimK_4fma * jcp.dimN_reg_block;
            for (int tb = 0; tb < jcp.dimK_4fma; tb+=4) {
                /*double buffering to hide load latencies*/
                int next = (curr + 4) % 8;
                if (i == 0 && tb == 0) {
                    load_B(0, origB_offset);
                }
                if (tb + 4 < (jcp.dimK_4fma -1)) {
                    load_B(next, origB_offset + 4);
                } else if (i < alpha - 1) {
                    load_B(next, origB_offset + jcp.dimK_4fma);
                }

                vunpcklps(Zmm(8), Zmm(curr), Zmm(curr + 1));
                vunpcklps(Zmm(9), Zmm(curr + 2), Zmm(curr + 3));
                vunpckhps(Zmm(curr), Zmm(curr), Zmm(curr + 1));
                vunpckhps(Zmm(curr + 1), Zmm(curr + 2), Zmm(curr + 3));

                vunpcklpd(Zmm(curr + 2), Zmm(8), Zmm(9));
                vunpckhpd(Zmm(curr + 3), Zmm(8), Zmm(9));

                vunpcklpd(Zmm(8), Zmm(curr), Zmm(curr + 1));
                vunpckhpd(Zmm(9), Zmm(curr), Zmm(curr + 1));

                vmovntps(zword[reg_transB
                    + sizeof(float) * (transB_offset + tb * jcp.dimN_reg_block)],
                    Zmm(curr+2));
                vmovntps(zword[reg_transB
                    + sizeof(float) * (transB_offset + (tb + 1) * jcp.dimN_reg_block)],
                    Zmm(curr+3));
                vmovntps(zword[reg_transB
                    + sizeof(float) * (transB_offset + (tb + 2) * jcp.dimN_reg_block)],
                    Zmm(8));
                vmovntps(zword[reg_transB
                    + sizeof(float) * (transB_offset + (tb + 3) * jcp.dimN_reg_block)],
                    Zmm(9));
                curr = next;

            }
        }
    }
    ret();
}
void jit_avx512_core_conv_winograd_bwd_weights_kernel_f32::gemm_loop_generate(
        bool is_first_tile)
{
// for (int ofm2 = 0; ofm2 < jcp.oc_block; ofm2++)
//     for (int ifm2 = 0; ifm2 < jcp.ic_block; ifm2++)
//         for (int nb_tile_block_ur = 0; nb_tile_block_ur <
//         jcp.nb_tile_block_ur; nb_tile_block_ur++)
//             for (int tile_block_ur = 0; tile_block_ur <
//             jcp.tile_block_ur; tile_block_ur++)
//                 for (int ifm3 = 0; ifm3 < jcp.ic_reg_block; ++ifm3)
//                     U[ofm2][ifm2][ofm3][ifm3][0:oc_simd_block] +=
//                         M[ofm2][ofm3][nb_tile_block_ur][tile_block_ur][0:oc_simd_block]
//                          *
//                          broadcast(V[ifm2][nb_tile_block_ur][ifm3][tile_block_ur])
    auto inner_loops = [=]() {
        int inc_fma = jcp.ver == ver_4fma ? 4 : 1;
        const int fma_ipc = jcp.ver == ver_4fma ? 1 : 2;
        prefetcher_t<float> L1_pf(this, reg_srcB, L1,
                jcp.dimK_reg_block * jcp.dimN_reg_block * jcp.dimK_4fma,
                jcp.dimK_reg_block * jcp.dimN_reg_block * jcp.dimK_4fma
                        / inc_fma,
                fma_ipc);
        prefetcher_t<float> L2_pf(this, reg_srcB, L2,
                jcp.dimK_reg_block * jcp.dimN_reg_block * jcp.dimK_4fma,
                jcp.dimK_reg_block * jcp.dimN_reg_block * jcp.dimK_4fma
                        / inc_fma,
                fma_ipc);

        auto load_A = [=](int reg_idx, int offset) {
            for (int i = 0; i < inc_fma; i++) {
                vmovups(Zmm(reg_idx + i),
                        zword[reg_srcA +
                        sizeof(float) * jcp.dimM_simd_block * (offset + i)]);
            }
        };

        Label dimM_block_loop, dimK_block_loop, dimN_block_loop;
        if (jcp.dimM_block > 1) {
            mov(reg_dimM_block_loop_cnt, jcp.dimM_block);
            L(dimM_block_loop);
        }
        { /************* OC_block (M) loop ***********/
            if (jcp.dimN_block > 1) {
                mov(reg_dimN_block_loop_cnt, jcp.dimN_block);
                L(dimN_block_loop);
            }
            { /*************** IC_block (N) loop *********/
                for (int dimN_reg_block = 0;
                        dimN_reg_block < jcp.dimN_reg_block; ++dimN_reg_block) {
                    Zmm zmm(jcp.zmm_start + dimN_reg_block);
                    if (is_first_tile)
                        vpxord(zmm, zmm, zmm);
                    else
                        vmovups(zmm, zword[reg_dstC +
                                dimN_reg_block * jcp.dimM_simd_block *
                                sizeof(float)]);
                }

                if (jcp.dimK_block > 1) {
                    mov(reg_dimK_block_loop_cnt, jcp.dimK_block);
                    L(dimK_block_loop);
                }
                { /************* nb_tile_ur(K) loop ********/
                    int next = 0;
                    if (jcp.double_buffering) {
                        load_A(next, 0);
                    }
                    for (int dimK_reg_block = 0;
                            dimK_reg_block < jcp.dimK_reg_block;
                            dimK_reg_block++) {
                        int srcB_offset = dimK_reg_block * jcp.dimK_4fma
                                * jcp.dimN_reg_block;
                        for (int dimK_4fma = 0; dimK_4fma < jcp.dimK_4fma;
                                dimK_4fma += inc_fma) {
                            int current = next;
                            if (jcp.double_buffering) {
                                next = (dimK_reg_block * jcp.dimK_4fma
                                               + dimK_4fma + inc_fma)
                                        % (2 * inc_fma);
                                load_A(next, dimK_reg_block * jcp.dimK_4fma
                                                + dimK_4fma + inc_fma);
                            } else {
                                next = 0;
                                load_A(next, dimK_reg_block * jcp.dimK_4fma
                                                + dimK_4fma);
                            }
                            for (int dimN_reg_block = 0;
                                    dimN_reg_block < jcp.dimN_reg_block;
                                    ++dimN_reg_block) {
                                L1_pf.prefetch(srcB_offset / inc_fma
                                        + dimK_4fma / inc_fma
                                                * jcp.dimN_reg_block
                                        + dimN_reg_block);
                                L2_pf.prefetch(srcB_offset / inc_fma
                                        + dimK_4fma / inc_fma
                                                * jcp.dimN_reg_block
                                        + dimN_reg_block);
                                if (jcp.ver == ver_4fma) {
                                    int srcB_trans_offset = (dimK_4fma / 4) * 64
                                            + dimK_4fma % 4;
                                    v4fmaddps(
                                            Zmm(jcp.zmm_start + dimN_reg_block),
                                            Zmm(current),
                                            EVEX_compress_addr(reg_srcB,
                                                sizeof(float) * (
                                                    srcB_offset +
                                                    srcB_trans_offset +
                                                    (dimN_reg_block % 4) * 16 +
                                                    (dimN_reg_block / 4) * 4)));
                                } else {
                                    vfmadd231ps(
                                            Zmm(jcp.zmm_start + dimN_reg_block),
                                            Zmm(current),
                                            EVEX_compress_addr(reg_srcB,
                                                sizeof(float) * (srcB_offset + dimN_reg_block),
                                                    true));
                                }
                            }
                        }
                    }
                }

                add(reg_srcA, jcp.dimK_reg_block * jcp.dimK_4fma
                                * jcp.dimM_simd_block * sizeof(float));
                add(reg_srcB, jcp.dimK_reg_block * jcp.dimN_reg_block
                                * jcp.dimK_4fma * sizeof(float));
                if (jcp.dimK_block > 1) {
                    sub(reg_dimK_block_loop_cnt, 1);
                    jnz(dimK_block_loop);
                }

                /******** Write C back to memory *******/
                for (int dimN_reg_block = 0;
                        dimN_reg_block < jcp.dimN_reg_block; ++dimN_reg_block) {
                    Zmm zmm(jcp.zmm_start + dimN_reg_block);
                    vmovups(zword[reg_dstC +
                            dimN_reg_block * jcp.dimM_simd_block * sizeof(float)],
                            zmm);
                }

                sub(reg_srcA, jcp.dimK_block * jcp.dimK_reg_block *
                        jcp.dimK_4fma * jcp.dimM_simd_block * sizeof(float));
                add(reg_dstC, jcp.dimN_reg_block * jcp.dimM_simd_block
                        * sizeof(float));
                if (jcp.dimN_block > 1) {
                    sub(reg_dimN_block_loop_cnt, 1);
                    jnz(dimN_block_loop);
                }
            }

            if (jcp.dimM_block > 1) {
                sub(reg_srcB, jcp.dimN_block * jcp.dimK_block
                                * jcp.dimK_reg_block * jcp.dimN_reg_block
                                * jcp.dimK_4fma * sizeof(float));
                add(reg_srcA, jcp.dimK_block * jcp.dimK_reg_block
                                * jcp.dimK_4fma * jcp.dimM_simd_block * sizeof(float));
                sub(reg_dimM_block_loop_cnt, 1);
                jnz(dimM_block_loop);
            }
        }
    };

    /* Preamble */
    // register used to handle long fma encoding
    push(reg_EVEX_max_8b_offt);
    push(reg_dimK_block_loop_cnt);
    mov(reg_EVEX_max_8b_offt, 2 * EVEX_max_8b_offt);
    mov(reg_srcA, reg_srcA_const);
    inner_loops();

    /* Postamble */
    pop(reg_dimK_block_loop_cnt);
    pop(reg_EVEX_max_8b_offt);
    ret();
}

namespace {
bool check_cond1_wu(int dimM_block, int dimM_simdw, int dimK_block,
        int dimK_reg_block, int dimK_4fma, int dimN_reg_block, float C)
{
    float lhs = 1.0f * dimM_block * dimN_reg_block * dimM_simdw;
    lhs += dimM_block * dimK_block * dimK_reg_block * dimK_4fma * dimM_simdw;
    lhs += dimK_block * dimN_reg_block * dimK_reg_block * dimK_4fma;
    lhs *= sizeof(float);
    float rhs = C * L1_cache_size;
    return (lhs <= rhs);
}

bool check_cond1bis_wu(int dimM_block, int dimM_simdw, int dimK_block,
        int dimK_reg_block, int dimK_4fma, int dimN_reg_block, float C)
{
    float lhs = 1.0f * dimM_block * dimK_block * dimK_reg_block * dimK_4fma
            * dimM_simdw;
    lhs += dimK_block * dimN_reg_block * dimK_reg_block * dimK_4fma;
    lhs *= sizeof(float);
    float rhs = C * L1_cache_size;
    return (lhs <= rhs);
}

bool check_cond2bis_wu(int dimM_block, int dimM_simdw, int dimK_block,
        int dimK_reg_block, int dimK_4fma, int dimN_block, int dimN_reg_block,
        float C)
{
    float lhs = 1.0f * dimM_block * dimM_simdw * dimK_block * dimK_reg_block
            * dimK_4fma;
    lhs += dimK_block * dimK_reg_block * dimK_4fma * dimN_block
            * dimN_reg_block;
    lhs *= sizeof(float);
    float rhs = C * L2_cache_size;
    return (lhs <= rhs);
}

bool check_cond2_wu(int dimM_block, int dimM_simdw, int dimK_block,
        int dimK_reg_block, int dimK_4fma, int dimN_block, int dimN_reg_block,
        float C)
{
    float lhs = 1.0f * dimM_block * dimM_simdw * dimN_block * dimN_reg_block;
    lhs += dimM_block * dimM_simdw * dimK_block * dimK_reg_block * dimK_4fma;
    lhs += dimK_block * dimK_reg_block * dimK_4fma * dimN_block
            * dimN_reg_block;
    lhs *= sizeof(float);
    float rhs = C * L2_cache_size;
    return (lhs <= rhs);
}
}

bool set_wsched_WEI_S_D_G_W_avx512_core(jit_conv_winograd_conf_t &jcp)
{
    /*************** Choose dimN_reg_block (ic_simd_block)
     * *******************************/
    jcp.dimN = jcp.ic;
    /*Hardcoded to 16 because N = ic for bwd weights and
     innermost dimension for ic is assumed 16 in src transforms. This
     choice covers load latencies while maintaining simplicity of kernel
     for POR topologies. FIXME in future??: Will not work for future topologies
     when ic%16 != 0*/
    jcp.dimN_reg_block = jcp.ic_simd_block;

    /****************************** Choose dimK_block
     * **************************/
    // No freedom for choosing dimM_simd_block because ic_simd_block
    // is determined by input data format
    jcp.dimM_simd_block = jcp.oc_simd_block;

    auto test_cond1bis_dimK_block = [](
            jit_conv_winograd_conf_t &jcp, int dimK_block, int current_best) {
        return check_cond1bis_wu(1, jcp.dimM_simd_block, dimK_block, 1,
                       jcp.dimK_4fma, jcp.dimN_reg_block, 0.4f)
                && (dimK_block > current_best);
    };

    auto test_cond1_dimK_block = [](
            jit_conv_winograd_conf_t &jcp, int dimK_block, int current_best) {
        return check_cond1_wu(1, jcp.dimM_simd_block, dimK_block, 1,
                       jcp.dimK_4fma, jcp.dimN_reg_block, 0.4f)
                && (dimK_block > current_best);
    };

    auto test_cond2bis_dimK_block = [](
            jit_conv_winograd_conf_t &jcp, int dimK_block, int current_best) {
        return check_cond2bis_wu(1, jcp.dimM_simd_block, dimK_block, 1,
                       jcp.dimK_4fma, 1, jcp.dimN_reg_block, 0.5f)
                && (dimK_block > current_best);
    };

    auto test_cond2_dimK_block = [](
            jit_conv_winograd_conf_t &jcp, int dimK_block, int current_best) {
        return check_cond2_wu(1, jcp.dimM_simd_block, dimK_block, 1,
                       jcp.dimK_4fma, 1, jcp.dimN_reg_block, 0.1f)
                && (dimK_block > current_best);
    };

    jcp.dimK_block = get_divisor_satisfying_cond(
            jcp, jcp.dimK / jcp.dimK_4fma, 1, test_cond2bis_dimK_block);
    if (jcp.dimK_block < jcp.dimK / jcp.dimK_4fma)
        jcp.dimK_block = get_divisor_satisfying_cond(
                jcp, jcp.dimK / jcp.dimK_4fma, 1, test_cond2_dimK_block);

    jcp.dimK_reg_block = get_divisor_satisfying_cond(
            jcp, jcp.dimK_block, 1, test_cond1bis_dimK_block);
    if (jcp.dimK_reg_block < jcp.dimK_block) {
        jcp.dimK_reg_block = get_divisor_satisfying_cond(
                jcp, jcp.dimK_block, 1, test_cond1_dimK_block);
    }
    jcp.dimK_block /= jcp.dimK_reg_block;
    jcp.dimK_nb_block
            = jcp.dimK / jcp.dimK_4fma / jcp.dimK_reg_block / jcp.dimK_block;
    jcp.tile_block_ur = jcp.dimK_reg_block;
    jcp.nb_tile_block_ur = jcp.dimK_block;
    jcp.tile_block = jcp.dimK_nb_block;

    /***************************** Chose dimN block
     * ****************************/
    auto test_cond2_dimN_block = [](
            jit_conv_winograd_conf_t &jcp, int dimN_block, int current_best) {
        return check_cond2_wu(1, jcp.dimM_simd_block, jcp.dimK_block,
                       jcp.dimK_reg_block, jcp.dimK_4fma, dimN_block,
                       jcp.dimN_reg_block, 0.5f)
                && (dimN_block > current_best);
    };

    jcp.dimN_block = get_divisor_satisfying_cond(
            jcp, jcp.dimN / jcp.dimN_reg_block, 1, test_cond2_dimN_block);
    jcp.ic_block = jcp.dimN_block;
    jcp.dimN_nb_block = jcp.dimN / jcp.dimN_reg_block / jcp.dimN_block;
    jcp.nb_ic = jcp.dimN_nb_block;

    /********************************* Choose dimM block
     * ************************/
    jcp.dimM = jcp.oc;

    auto test_cond1_dimM_block = [](
            jit_conv_winograd_conf_t &jcp, int dimM_block, int current_best) {
        return check_cond1_wu(dimM_block, jcp.dimM_simd_block, 1,
                       jcp.dimK_reg_block, jcp.dimK_4fma, jcp.dimN_reg_block,
                       1.0f)
                && (dimM_block > current_best)
                && (jcp.dimM / jcp.dimM_simd_block / dimM_block) >= 2;
    };

    jcp.dimM_block = get_divisor_satisfying_cond(
            jcp, jcp.dimM / jcp.dimM_simd_block, 1, test_cond1_dimM_block);
    jcp.dimM_nb_block = (jcp.dimM / jcp.dimM_simd_block) / jcp.dimM_block;

    jcp.sched_policy = WSCHED_WEI_S_D_G_W;
    return true;
}

namespace {
bool is_in_L1_range(int v, float C1, float C2)
{
    return ((v > C1 * L1_cache_size) && (v < C2 * L1_cache_size));
}

bool is_in_L2_range(int v, float C1, float C2)
{
    return ((v > C1 * L2_cache_size) && (v < C2 * L2_cache_size));
}

void set_jcp_WEI_params(jit_conv_winograd_conf_t &jcp, int tile_block_ur,
        int tile_block, int nb_ic, int nb_oc)
{
    jcp.tile_block_ur = tile_block_ur;
    jcp.tile_block = tile_block;
    jcp.nb_ic = nb_ic;
    jcp.nb_oc = nb_oc;

    jcp.nb_tile_block_ur = jcp.ntiles / jcp.tile_block / jcp.tile_block_ur;
    jcp.ic_block = jcp.ic / jcp.ic_simd_block / jcp.nb_ic;
    jcp.oc_block = jcp.oc / jcp.oc_simd_block / jcp.nb_oc;

    jcp.dimK_reg_block = jcp.tile_block_ur;
    jcp.dimK_block = jcp.nb_tile_block_ur;
    jcp.dimK_nb_block = jcp.tile_block;
    jcp.dimN_reg_block = jcp.ic_simd_block;
    jcp.dimN_block = jcp.ic_block;
    jcp.dimN_nb_block = jcp.nb_ic;
    jcp.dimM_simd_block = jcp.oc_simd_block;
    jcp.dimM_block = jcp.oc_block;
    jcp.dimM_nb_block = jcp.nb_oc;
}
}

bool set_wsched_WEI_SDGt_W_avx512_core(jit_conv_winograd_conf_t &jcp)
{
    jcp.ic_simd_block = jcp.oc_simd_block = 16;
    int nb_ic_simd_block = jcp.ic / jcp.ic_simd_block;
    int nb_oc_simd_block = jcp.oc / jcp.oc_simd_block;

    int min_tile_block_ur = 8;
    int max_tile_block_ur = 64;
    int max_tile_block = jcp.ntiles / min_tile_block_ur;

    // Consider L2 + L3 together on SKX
    const float C1_min = .1, C1_0 = .4, C1_max = .5;
    const float C2_0 = .4, C2_max = .5;
    const float TC2_0 = .7, TC2_max = 1.2;
    const int T_min = 2, T0 = 20;
    float C1, C2, TC2;
    int T, tile_block, tile_block_ur, nb_oc, nb_ic;

    auto blocking_ok = [&]() -> bool {
        // V:tile_block + M:tile_block + U
        int thread_size = alpha * alpha * jcp.oc
                        * (jcp.ntiles / tile_block) * sizeof(float)
                + alpha * alpha * jcp.ic * (jcp.ntiles / tile_block)
                        * sizeof(float)
                + alpha * alpha * jcp.ic * jcp.oc * sizeof(float);
        // V:tile_block + M:tile_block
        int L2_reuse = alpha * alpha * jcp.oc
                        * (jcp.ntiles / tile_block) * sizeof(float)
                + alpha * alpha * jcp.ic * (jcp.ntiles / tile_block)
                        * sizeof(float);
        // V:nb_ic + M:nb_tile_block_ur
        // Use M:nb_oc + V:nb_ic as an superset estimation
        int L1_reuse
                = (jcp.ic / nb_ic) * (jcp.ntiles / tile_block) * sizeof(float)
                + (jcp.oc / nb_oc) * (jcp.ntiles / tile_block) * sizeof(float);

        return jcp.ntiles % tile_block == 0
                && (jcp.ntiles / tile_block) % tile_block_ur == 0
                && is_in_L2_range(thread_size, TC2, TC2_max)
                && is_in_L2_range(L2_reuse, C2, C2_max)
                && tile_block > T * omp_get_max_threads()
                && nb_oc_simd_block % nb_oc == 0
                && nb_ic_simd_block % nb_ic == 0
                && is_in_L1_range(L1_reuse, C1, C1_max);
    };

    for (C1 = C1_0, C2 = C2_0, TC2 = TC2_0; C1 > C1_min;
            C1 -= .02, C2 -= .02, TC2 -= .04) {
        for (T = T0; T >= T_min; --T) {
            for (tile_block = 1; tile_block <= max_tile_block; ++tile_block) {
                for (tile_block_ur = max_tile_block_ur;
                        tile_block_ur >= min_tile_block_ur; --tile_block_ur) {
                    for (nb_oc = 1; nb_oc <= nb_oc_simd_block; ++nb_oc) {
                        for (nb_ic = nb_ic_simd_block; nb_ic >= 1; --nb_ic) {
                            if (blocking_ok()) {
                                set_jcp_WEI_params(jcp, tile_block_ur,
                                        tile_block, nb_ic, nb_oc);
                                jcp.sched_policy = WSCHED_WEI_SDGt_W;
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool set_wsched_WEI_SDGtWo_avx512_core(jit_conv_winograd_conf_t &jcp)
{
    jcp.ic_simd_block = jcp.oc_simd_block = 16;
    int nb_ic_simd_block = jcp.ic / jcp.ic_simd_block;
    int nb_oc_simd_block = jcp.oc / jcp.oc_simd_block;

    int min_tile_block_ur = 12;
    int max_tile_block_ur = 64;
    int max_tile_block = jcp.ntiles / min_tile_block_ur;

    const float C1_min = .1, C1_0 = .4, C1_max = .5;
    const float C2_0 = .4, C2_max = .6;
    const float TC2_0 = .7, TC2_max = 1.6;

    const int max_nb_oc = 2; // Limit the # of sequential execution
    const int T0 = 12, T_min = 8;
    float C1, C2, TC2;
    int T, tile_block, tile_block_ur, nb_oc, nb_ic;

    auto blocking_ok = [&]() -> bool {
        // M:tile_block:nb_oc + V:tile_block + U:nb_oc
        int thread_size = alpha * alpha * (jcp.oc / nb_oc)
                        * (jcp.ntiles / tile_block) * sizeof(float)
                + alpha * alpha * jcp.ic * (jcp.ntiles / tile_block)
                        * sizeof(float)
                + alpha * alpha * jcp.ic * (jcp.oc / nb_oc)
                        * sizeof(float);
        // M:tile_block:nb_oc + V:tile_block
        int L2_reuse = alpha * alpha * (jcp.oc / nb_oc)
                        * (jcp.ntiles / tile_block) * sizeof(float)
                + alpha * alpha * jcp.ic * (jcp.ntiles / tile_block)
                        * sizeof(float);
        // V:nb_ic + M:nb_tile_block_ur
        // Use M:nb_oc + V:nb_ic as an superset estimation
        int L1_reuse
                = (jcp.ic / nb_ic) * (jcp.ntiles / tile_block) * sizeof(float)
                + (jcp.oc / nb_oc) * (jcp.ntiles / tile_block) * sizeof(float);

        return jcp.ntiles % tile_block == 0
                && (jcp.ntiles / tile_block) % tile_block_ur == 0
                && is_in_L2_range(thread_size, TC2, TC2_max)
                && is_in_L2_range(L2_reuse, C2, C2_max)
                && tile_block > T * omp_get_max_threads()
                && nb_oc_simd_block % nb_oc == 0
                && nb_ic_simd_block % nb_ic == 0
                && is_in_L1_range(L1_reuse, C1, C1_max);
    };

    for (T = T0; T >= T_min; --T) {
        for (C1 = C1_0, C2 = C2_0, TC2 = TC2_0; C1 > C1_min;
                C1 -= .02, C2 -= .02, TC2 -= .04) {
            for (nb_oc = 1; nb_oc <= max_nb_oc; ++nb_oc) {
                for (tile_block = max_tile_block; tile_block >= 1;
                        --tile_block) {
                    for (tile_block_ur = min_tile_block_ur;
                            tile_block_ur <= max_tile_block_ur;
                            ++tile_block_ur) {
                        for (nb_ic = 1; nb_ic <= nb_ic_simd_block; ++nb_ic) {
                            if (blocking_ok()) {
                                set_jcp_WEI_params(jcp, tile_block_ur,
                                        tile_block, nb_ic, nb_oc);
                                jcp.sched_policy = WSCHED_WEI_SDGtWo;
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}

bool set_wsched_WEI_S_D_Giot_W_avx512_core(jit_conv_winograd_conf_t &jcp)
{
    jcp.ic_simd_block = jcp.oc_simd_block = 16;
    int nb_ic_simd_block = jcp.ic / jcp.ic_simd_block;

    int min_tile_block_ur = 8;
    int max_tile_block_ur = 64;
    const float C1_min = .2, C1_0 = .4, C1_max = .9;
    const float C2_min = .1, C2_0 = .4, C2_max = .5;
    const int T0 = 16, T_min = 12;
    float C1, C2;
    int T, tile_block, tile_block_ur, nb_ic;
    int nb_oc = 1; // Keep nb_oc small to increase
                   // oc_block, for better reuse of V in
                   // L2

    auto blocking_ok = [&]() -> bool {
        // V[:ic_block][][][]
        int L2_reuse
                = (jcp.ic / nb_ic) * (jcp.ntiles / tile_block) * sizeof(float);
        // M[:nb_tile_block_ur][][] + V[:nb_tile_block_ur][][]
        int L1_reuse
                = (jcp.ntiles / tile_block) * jcp.oc_simd_block * sizeof(float);

        int work_amount = tile_block * nb_ic * nb_oc * alpha * alpha;

        return (jcp.ntiles / tile_block_ur) % tile_block == 0
                && jcp.ntiles % tile_block_ur == 0
                && nb_ic_simd_block % nb_ic == 0
                && is_in_L2_range(L2_reuse, C2, C2_max)
                && is_in_L1_range(L1_reuse, C1, C1_max)
                && work_amount > T * omp_get_max_threads();
    };

    for (T = T0; T >= T_min; --T) {
        for (C1 = C1_0; C1 > C1_min; C1 -= .02) {
            for (C2 = C2_0; C2 > C2_min; C2 -= .02) {
                for (nb_ic = 1; nb_ic <= nb_ic_simd_block; ++nb_ic) {
                    for (tile_block_ur = min_tile_block_ur;
                            tile_block_ur <= max_tile_block_ur;
                            ++tile_block_ur) {
                        for (tile_block = 1;
                                tile_block <= jcp.ntiles / min_tile_block_ur;
                                ++tile_block) {
                            if (blocking_ok()) {
                                set_jcp_WEI_params(jcp, tile_block_ur,
                                        tile_block, nb_ic, nb_oc);
                                jcp.sched_policy = WSCHED_WEI_S_D_Giot_W;
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

status_t jit_avx512_core_conv_winograd_bwd_weights_kernel_f32::init_conf(
        jit_conv_winograd_conf_t &jcp, const convolution_desc_t &cd,
        const memory_desc_wrapper &src_d, const memory_desc_wrapper &diff_dst_d,
        const memory_desc_wrapper &diff_weights_d)
{
    if (!mayiuse(avx512_core))
        return status::unimplemented;

    const bool with_groups = diff_weights_d.ndims() == src_d.ndims() + 1;

    jcp.ngroups = with_groups ? diff_weights_d.dims()[0] : 1;
    jcp.mb = src_d.dims()[0];
    jcp.oc = diff_dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;
    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = diff_dst_d.dims()[2];
    jcp.ow = diff_dst_d.dims()[3];
    jcp.kh = diff_weights_d.dims()[with_groups + 2];
    jcp.kw = diff_weights_d.dims()[with_groups + 3];
    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];
    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];
    jcp.r_pad = nstl::max(
            0, (jcp.ow - 1) * jcp.stride_w + jcp.kw - jcp.iw - jcp.l_pad);
    jcp.b_pad = nstl::max(
            0, (jcp.oh - 1) * jcp.stride_h + jcp.kh - jcp.ih - jcp.t_pad);
    jcp.ihp = jcp.ih + jcp.t_pad + jcp.b_pad;
    jcp.iwp = jcp.iw + jcp.l_pad + jcp.r_pad;
    jcp.ohp = jcp.oh;
    jcp.owp = jcp.ow;
    jcp.with_bias = (cd.diff_bias_desc.format != memory_format::undef);
    jcp.dilate_h = cd.dilates[0];
    jcp.dilate_w = cd.dilates[1];

    if (!mayiuse(avx512_core)) return status::unimplemented;
    jcp.ver = ver_avx512_core;

    // Winograd specific initialization
    jcp.itiles = (jcp.ow + tile_size - 1) / tile_size;
    jcp.jtiles = (jcp.oh + tile_size - 1) / tile_size;
    jcp.ntiles = jcp.mb * jcp.itiles * jcp.jtiles;

    // Winograd kernel works only for 3x3 convolution with stride 1
    if (jcp.ngroups != 1)
        return status::unimplemented;
    if ((jcp.kh != 3) || (jcp.kw != 3))
        return status::unimplemented;
    if ((jcp.dilate_h != 0) || (jcp.dilate_w != 0))
        return status::unimplemented;
    if ((jcp.stride_h != 1) || (jcp.stride_w != 1))
        return status::unimplemented;
    if ((jcp.ic % simd_w) != 0 || (jcp.oc % simd_w) != 0)
        return status::unimplemented;
    if (src_d.format() != nChw16c)
        return status::unimplemented;
    if (diff_weights_d.format() != (with_groups ? gOIhw16i16o : OIhw16i16o))
        return status::unimplemented;
    if (diff_dst_d.format() != nChw16c)
        return status::unimplemented;

    /*************************** New Kernel Parameters
     * *****************************/
    jcp.ic_simd_block = simd_w;
    jcp.oc_simd_block = simd_w;
    jcp.dimK_4fma = 1;
    jcp.tile_4fma_padding = 0;

#define MAX_4FMA_UR 8
    if (jcp.ver == ver_4fma) {
        auto test_cond_4fma = [](jit_conv_winograd_conf_t &jcp, int dimK_4fma,
                                      int current_best) {
            return (dimK_4fma % 4 == 0) && (dimK_4fma <= MAX_4FMA_UR)
                    && (dimK_4fma > current_best);
        };
        jcp.dimK_4fma = get_divisor_satisfying_cond(
                jcp, jcp.itiles * jcp.jtiles, 4, test_cond_4fma);
        if (jcp.dimK_4fma == 1)
            jcp.dimK_4fma = 4;
        if ((jcp.itiles * jcp.jtiles) % jcp.dimK_4fma != 0)
            jcp.tile_4fma_padding = jcp.dimK_4fma
                    - ((jcp.itiles * jcp.jtiles) % jcp.dimK_4fma);
    }

    jcp.tile_4fma = jcp.dimK_4fma;
    /*NOTE: When (itiles * jtiles) % dimK_4fma != 0, transpose in diff_src
     * transform
     * will not work correctly, this is solved by applying padding.*/
    jcp.dimK = jcp.mb * (jcp.itiles * jcp.jtiles + jcp.tile_4fma_padding);
    jcp.dimN = jcp.ic;
    jcp.dimM = jcp.oc;

    jcp.double_buffering = true;
    if (jcp.double_buffering)
        jcp.zmm_start = jcp.ver == ver_4fma ? 8 : 2;
    else
        jcp.zmm_start = jcp.ver == ver_4fma ? 4 : 1;
    jcp.nb_reg = 32 - jcp.zmm_start;

    status_t res;
    jcp.sched_policy = WSCHED_INVALID;
    if ((jcp.ver == ver_avx512_core &&
            (set_wsched_WEI_SDGt_W_avx512_core(jcp)
            || set_wsched_WEI_SDGtWo_avx512_core(jcp)
            || set_wsched_WEI_S_D_Giot_W_avx512_core(jcp)))
        || set_wsched_WEI_S_D_G_W_avx512_core(jcp))
        res = status::success;
    else
        return status::unimplemented;

    jcp.tile_block_ur = jcp.dimK_reg_block;
    jcp.nb_tile_block_ur = jcp.dimK_block;
    jcp.tile_block = jcp.dimK_nb_block;

    jcp.ic_block = jcp.dimN_block;
    jcp.nb_ic = jcp.dimN_nb_block;

    jcp.oc_block = jcp.dimM_block;
    jcp.nb_oc = jcp.dimM_nb_block;

    return res;

}
}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s