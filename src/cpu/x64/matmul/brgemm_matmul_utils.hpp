/*******************************************************************************
* Copyright 2021-2025 Intel Corporation
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

#ifndef CPU_X64_MATMUL_BRGEMM_MATMUL_UTILS_HPP
#define CPU_X64_MATMUL_BRGEMM_MATMUL_UTILS_HPP

#include "common/c_types_map.hpp"
#include "common/math_utils.hpp"
#include "common/memory_tracking.hpp"

#include "common/verbose.hpp"
#include "cpu/matmul/matmul_utils.hpp"
#include "cpu/x64/brgemm/brgemm.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {
namespace matmul {

constexpr int max_batch_ndims = DNNL_MAX_NDIMS - 2;

struct brgemm_matmul_bcast_desc_t {

    brgemm_matmul_bcast_desc_t()
        : bcast_mask(0)
        , first_bcast_dim(-1)
        , last_bcast_dim(-1)
        , bcast_across_all_batch_dims(false)
        , first_bcast_dim_to_last_batch_dim_prod(1)
        , bcast_dims_prod(1)
        , batch_dims {0}
        , gb_off {0} {}

    void set_params(const dims_t &inp_dims, const dims_t &dst_d_dims,
            int batch_ndims, dim_t batch) {
        const int ndims = batch_ndims;
        first_bcast_dim_to_last_batch_dim_prod = batch;
        for (int d = 0; d < ndims; ++d) {
            batch_dims[d] = dst_d_dims[d];
            gb_off[d] = (d == 0 ? batch : gb_off[d - 1]) / dst_d_dims[d];
            if (dst_d_dims[d] != 1 && inp_dims[d] == 1) { // broadcast
                const int mask = 1 << (ndims - 1);
                bcast_mask |= (mask >> d);
                if (first_bcast_dim == -1) {
                    first_bcast_dim = d;
                    if (d == 0) // broadcast_dim == B0
                        first_bcast_dim_to_last_batch_dim_prod = batch;
                }
                last_bcast_dim = d;
                bcast_dims_prod *= dst_d_dims[d];
            }
            if (first_bcast_dim == -1) // broadcast_dim > B0
                first_bcast_dim_to_last_batch_dim_prod /= dst_d_dims[d];
        }
        bcast_across_all_batch_dims = IMPLICATION(
                batch > 1, bcast_mask > 0 && bcast_dims_prod == batch);
    }

    int bcast_mask; // sets bcast_dim = 1, non_bcast_dim = 0

    int first_bcast_dim;
    int last_bcast_dim;
    bool bcast_across_all_batch_dims;

    dim_t first_bcast_dim_to_last_batch_dim_prod;
    dim_t bcast_dims_prod;

    dim_t batch_dims[max_batch_ndims];
    dim_t gb_off[max_batch_ndims]; // generalized batch offset
};

struct brgemm_matmul_conf_t {
    int ndims, batch_ndims;
    dim_t M, N, K, batch, batch_without_first_dim;
    dim_t M_blk, N_blk, K_blk, M_tail, N_tail, K_tail;
    int M_chunk_size, N_chunk_size, K_chunk_size;
    bool is_a_nt, is_b_nt, set_nt;
    dim_t LDA, LDB, LDC, LDD;
    dim_t LDB2;
    int brgemm_batch_size, brgemm_batch_tail_size;
    int wei_n_blk, wei_k_blk;
    brgemm_batch_kind_t brg_type;
    bool is_macro_heuristics;

    cpu_isa_t isa;

    matmul_reduce_kind_t reduce_kind;

    format_tag_t src_tag, wei_tag, dst_tag, bia_tag;
    bool with_reduce;
    bool with_bias;
    bool with_sum;
    bool with_eltwise;
    bool with_binary;
    bool with_scales;
    bool with_dst_scales;
    bool s8s8_compensation_required;
    bool packed_sparse_weights;
    bool req_transpose_scales;
    bool with_wei_decompression;
    int postops_inst_count;
    brgemm_broadcast_t src_zp_type;
    brgemm_broadcast_t wei_zp_type;
    brgemm_broadcast_t dst_zp_type;

    bool use_buffer_a;
    bool use_buffer_a_tail_only;
    bool use_buffer_b;
    bool use_buffer_c;
    bool use_buffer_reduce;

    brgemm_matmul_bcast_desc_t bcast_A_desc;
    brgemm_matmul_bcast_desc_t bcast_B_desc;

    data_type_t src_dt;
    data_type_t dst_dt;
    data_type_t wei_dt;
    data_type_t acc_dt;
    data_type_t bia_dt;
    data_type_t reduce_dt;
    data_type_t orig_src_dt;
    data_type_t orig_wei_dt;
    int nthr;
    int nthr_k = 1, nthr_m = 1, nthr_n = 1, nthr_b = 1;

    bool is_thread_chunks_exec_order_horizontal;
    brgemm_kernel_hint_mem_advice_t mem_advice;

    // Auxiliary values for init_config() and execute()
    dim_t a_dt_sz, b_dt_sz, c_dt_sz, acc_dt_sz, bias_dt_sz, reduce_dt_sz;

    // used for transposed buffer datatype when different from x_dt_sz
    // (e.g. used in BF32 implementations having to down-convert to BF16
    // from FP32 implementation)
    dim_t tr_a_dt_sz, tr_b_dt_sz;

    int M_chunks;
    int N_chunks;
    int K_chunks;
    int num_M_blocks;
    int num_N_blocks;
    int num_K_blocks;
    dim_t M_chunk_elems;
    dim_t N_chunk_elems;
    dim_t K_chunk_elems;

    // Pre-calculated memory strides for each tensor
    dim_t A_strides[3];
    dim_t B_strides[3];
    dim_t C_strides[3];
    dim_t buffer_c_chunk_sz;
    dim_t buffer_c_per_thread_sz;

    dim_t A_ptr_shift_b;
    dim_t B_ptr_shift_b;
    dim_t C_ptr_shift_b;
    dim_t copy_A_src_stride;
    dim_t copy_B_wei_stride;

    dim_t buffer_a_gb_stride;
    dim_t buffer_a_k_stride;
    dim_t buffer_a_m_stride;
    dim_t buffer_a_per_thread_sz;

    dim_t buffer_b_gb_stride;
    dim_t buffer_b_k_brg_stride;
    dim_t buffer_b_per_thread_sz;

    dim_t buffer_reduce_per_thread_sz;

    dim_t s8s8_comp_ithr_str;
    dim_t s8s8_comp_b_str;
    dim_t s8s8_comp_n_str;
    bool has_zero_point_a, has_zero_point_b, has_zero_point_c;
    bool post_ops_applicable;
    bool transposed_A;
    bool transposed_B;
    bool blocked_B;
    bool treat_A_as_plain;

    // A_strides could be changed during
    // Matmul conf initialization in case when batches merged into M.
    // This flag helps to properly initialize LDA when A_strides
    // were changed.
    bool adjust_a_strides = false;

    dim_t zp_a_comp_shift_n;
    dim_t zp_a_comp_elems_per_thr;

    dim_t zp_b_comp_result_shift_m;
    dim_t zp_b_comp_buffer_start;
    dim_t zp_b_comp_buffer_shift_m;
    dim_t zp_b_comp_elems_per_thr;

    int wsp_tile_per_thr_bytes;
    int brgemm_batch_element_per_thr_sz;
    bool is_amx;

    int required_k_granularity;
    bool is_bf32 = false;
    bool is_bf16_with_int_wei = false;
    bool is_f16_with_int_wei = false;
    bool is_f32_f16 = false;
    bool is_f32_bf16 = false;
    bool is_int4_weights = false;
    bool is_tf32 = false;
    bool req_wei_vnni_downconvert = false;
    bool is_runtime_M = false;
    bool is_runtime_N = false;
    bool is_runtime_K = false;
    bool is_src_batch_layout_trivial = false;
    bool is_wei_batch_layout_trivial = false;
    bool is_dst_batch_layout_trivial = false;
    bool is_oscale_per_n = false;
    bool is_oscale_per_k = false;
    bool apply_scales_in_buffer_b = false;
    bool extendable_k = false;

    inline bool lda_big_pow2() const {
        const dim_t big_stride_threshold_in_bytes = 8192;
        const dim_t big_K_threshold = big_stride_threshold_in_bytes / a_dt_sz;
        return !transposed_A && math::is_pow2(K) && K >= big_K_threshold;
    }
};

struct brgemm_matmul_conf_utils_t {

    brgemm_matmul_conf_utils_t(brgemm_matmul_conf_t &bgmmc, const cpu_isa_t isa,
            const primitive_attr_t &attr, bool A_any_layout, bool B_any_layout,
            bool C_any_layout, bool bias_any_layout);

    inline bool check_b_layout_blocked_by_n(format_tag_t matrix_b_tag) const {
        return blocked_B_layouts_allowed && !bgmmc.is_runtime_N
                && utils::one_of(matrix_b_tag, blocked_64n_B_layout_tag,
                        blocked_48n_B_layout_tag, blocked_32n_B_layout_tag,
                        blocked_24n_B_layout_tag, blocked_16n_B_layout_tag,
                        blocked_8n_B_layout_tag);
    }

    inline bool check_b_layout_blocked_32_by_n(
            format_tag_t matrix_b_tag) const {
        return blocked_B_layouts_allowed && !bgmmc.is_runtime_N
                && utils::one_of(matrix_b_tag, blocked_32n_B_layout_tag);
    }

    inline bool get_blocked_B() const {
        return blocked_B_layouts_allowed && !bgmmc.is_runtime_N
                && check_b_layout_blocked_by_n(bgmmc.wei_tag);
    }

    inline bool use_buffer_b(bool use_heuristic = true) const {
        if (bgmmc.is_runtime_N) return true;
        if (bgmmc.is_bf16_with_int_wei) return true;
        if (bgmmc.is_f16_with_int_wei) return true;
        if (bgmmc.apply_scales_in_buffer_b) return true;

        if (bgmmc.is_amx)
            // use b_buffer for AMX when:
            // - not bf32 && using non-blocked weights
            // - is bf32
            // - is tf32
            return IMPLICATION(!wei_down_convert_to_vnni(), !bgmmc.blocked_B)
                    || bgmmc.packed_sparse_weights;

        // Values based on measured performance difference
        // between plain and copy-to-blocked routine.
        const bool is_avx2_f32 = this->is_f32() && bgmmc.isa == avx2;
        size_t big_LDB = is_avx2_f32 ? bgmmc.N >= 128 : bgmmc.N > 256;
        bool is_pow2 = math::is_pow2(bgmmc.N);
        bool is_avx2_simd_tail = is_avx2_f32 && bgmmc.N > 64 && bgmmc.N % 8 != 0
                && !bgmmc.blocked_B;
        bool use_copy_buffer = IMPLICATION(
                this->is_f32(), use_heuristic && (big_LDB && is_pow2));
        return is_avx2_simd_tail
                || (this->is_f16() && bgmmc.isa == avx512_core_fp16)
                || (use_copy_buffer && this->check_is_plain(bgmmc.wei_tag))
                || this->check_is_transposed(bgmmc.wei_tag)
                || (bgmmc.wei_tag == format_tag::acbd)
                || (bgmmc.wei_tag == format_tag::adbc);
    }

    inline dim_t get_actual_LDB() const {
        const auto md_ldb = bgmmc.B_strides[1] / bgmmc.b_dt_sz;
        if (bgmmc.wei_tag == format_tag::acbd && !bgmmc.use_buffer_b) {
            assert(bgmmc.b_dt_sz == bgmmc.tr_b_dt_sz);
            return md_ldb;
        }
        bool use_blocked_LDB = bgmmc.is_amx || bgmmc.use_buffer_b
                || bgmmc.wei_tag != plain_tensor_layout_tag;
        if (use_blocked_LDB) return bgmmc.wei_n_blk;
        // When K == 1 we always pick "ab" format for B (see set_or_check_B_tag)
        // regardles of whether the actual tag was "ab" or  "ba".
        // Since the implementation assumes the "ab" format is used we cannot
        // use bgmmc.B_strides[1] directly as the strides could be specified for
        // "ba" therefore we need to use bgmmc.N instead.
        return bgmmc.K == 1 ? bgmmc.N : md_ldb;
    }

    inline bool maybe_low_brg_blocking() const {
        // Check if m_block is a prime number from 32 to 64
        const bool is_prime_num
                = utils::one_of(bgmmc.M_blk, 37, 41, 43, 47, 53, 59, 61);
        const bool maybe_ldb_tail = !bgmmc.is_runtime_N && bgmmc.N % 16;
        return is_prime_num && IMPLICATION(bgmmc.M_blk < 48, maybe_ldb_tail);
    }

    inline bool check_n_blk_fixed() const { return n_blk_fixed; }

    inline bool check_is_transposed(format_tag_t tag) const {
        return tag == transposed_tensor_layout_tag;
    }

    inline bool check_is_plain(format_tag_t tag) const {
        return tag == plain_tensor_layout_tag;
    }

    inline bool is_f32() const { return f32_dt; }

    inline bool is_bf16() const { return bf16_dt; }

    inline bool is_f16() const { return f16_dt; }

    inline bool is_f8() const { return f8_dt; }

    inline bool is_bf8() const { return bf8_dt; }

    inline bool is_int8() const { return int8_dt; }

    inline bool is_bf32() const { return bf32_dt; }

    inline bool is_tf32() const { return tf32_dt; }

    inline bool is_bf16_with_int_wei() const { return bf16_with_int_wei_dt; }

    inline bool is_f32_f16() const { return f32_f16_dt; }

    inline bool is_f32_bf16() const { return f32_bf16_dt; }

    inline bool is_f16_with_int_wei() const { return f16_with_int_wei_dt; }

    inline bool with_weights_decompression() const {
        return !utils::one_of(bgmmc.src_dt, data_type::s8, data_type::u8,
                       data_type::s4, data_type::u4)
                && weights_decompression_support;
    }

    inline bool is_int8_with_bf16_dst() const {
        return this->is_int8() && bgmmc.dst_dt == data_type::bf16;
    }

    inline bool wei_down_convert_to_vnni() const {
        return (bf32_dt || tf32_dt || f16_with_int_wei_dt
                       || bf16_with_int_wei_dt)
                && get_blocked_B();
    }

    inline bool is_any_B_layout() const { return B_any_layout; }

    inline cpu_isa_t get_isa() const { return isa_; }

    int get_default_n_block(format_tag_t matrix_b_tag) const;
    status_t set_or_check_B_tag(memory_desc_t &B_md,
            const dnnl::impl::cpu::matmul::matmul_helper_t &helper,
            bool init_n_tag = true) const;
    status_t update_and_check_B_tag(memory_desc_t &B_md, int n_blk_size,
            const dnnl::impl::cpu::matmul::matmul_helper_t &helper) const;
    status_t set_or_check_tags(memory_desc_t &A_md, memory_desc_t &C_md,
            memory_desc_t &bias_md,
            const dnnl::impl::cpu::matmul::matmul_helper_t &helper) const;
    status_t set_B_flags(memory_desc_t &B_md) const;
    format_tag_t pick_blocked_B_layout(int n_blk) const;

private:
    brgemm_matmul_conf_t &bgmmc;

    const bool f32_dt, bf16_dt, f16_dt, f8_dt, bf8_dt, int8_dt, bf32_dt,
            tf32_dt;
    const bool weights_decompression_support, bf16_with_int_wei_dt, f32_f16_dt,
            f32_bf16_dt, f16_with_int_wei_dt;
    const bool A_any_layout;
    const bool B_any_layout;
    const bool C_any_layout;
    const bool bias_any_layout;

    const format_tag_t plain_tensor_layout_tag;
    const format_tag_t transposed_tensor_layout_tag;
    const format_tag_t blocked_64n_B_layout_tag, blocked_48n_B_layout_tag,
            blocked_32n_B_layout_tag, blocked_24n_B_layout_tag,
            blocked_16n_B_layout_tag, blocked_8n_B_layout_tag;
    const bool blocked_B_layouts_allowed;
    const bool n_blk_fixed;
    const cpu_isa_t isa_;
};

// This function initializes all required fields in the conf object to generate
// copy_b kernel. Used in this impl and re-used in brgemm kernel API.
status_t init_conf(brgemm_matmul_conf_t &conf, dim_t batch, dim_t M, dim_t K,
        dim_t N, dim_t in_ld, dim_t n_blk, data_type_t in_type,
        data_type_t out_type, format_tag_t in_tag);

void init_aux_values(brgemm_matmul_conf_t &bgmmc,
        const memory_desc_wrapper &src_d, const memory_desc_wrapper &wei_d,
        const memory_desc_wrapper &dst_d);

status_t init_brgemm_matmul_conf(cpu_isa_t isa, brgemm_matmul_conf_t &bgmmc,
        const matmul_desc_t &mmd, memory_desc_t &src_md,
        memory_desc_t &weights_md, memory_desc_t &dst_md,
        memory_desc_t &bias_md, primitive_attr_t &attr);

void init_scratchpad(memory_tracking::registrar_t &scratchpad,
        const brgemm_matmul_conf_t &bgmmc);

int get_n_block_from_tag(format_tag_t matrix_b_tag);

void mem_advice_init(brgemm_matmul_conf_t &bgmmc);

bool is_batch_layout_trivial(const memory_desc_wrapper &mdw, const dim_t batch);

} // namespace matmul
} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif
