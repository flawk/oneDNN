/*******************************************************************************
* Copyright 2018-2022 Intel Corporation
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

#include "opdesc.hpp"
#include "primitive_desc_iface.hpp"
#include <initializer_list>

#include "oneapi/dnnl/dnnl.h"

#include "c_types_map.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

namespace dnnl {
namespace impl {
namespace rnn {

int get_gates_count(dnnl_alg_kind_t cell_kind) {
    switch (cell_kind) {
        case dnnl::impl::alg_kind::vanilla_rnn: return 1;
        case dnnl::impl::alg_kind::vanilla_gru:
        case dnnl::impl::alg_kind::vanilla_augru: return 3;
        case dnnl::impl::alg_kind::lbr_gru:
        case dnnl::impl::alg_kind::lbr_augru: return 3;
        case dnnl::impl::alg_kind::vanilla_lstm: return 4;
        default: assert(!"unknown cell kind"); return 0;
    }
    return 0;
}

} // namespace rnn
} // namespace impl
} // namespace dnnl

namespace {
using namespace dnnl::impl;
using namespace dnnl::impl::status;
using namespace dnnl::impl::types;
using namespace dnnl::impl::utils;

void maybe_init_md(memory_desc_t &md, const memory_desc_t *with_md) {
    if (with_md) md = *with_md;
}

bool xnor_md(const memory_desc_t *a_md, const memory_desc_t *b_md) {
    return is_zero_md(a_md) == is_zero_md(b_md);
}

status_t check_runtime_dims_or_strides(
        std::initializer_list<const memory_desc_t *> l) {
    bool runtime_dims_or_strides = false;
    for (auto md : l)
        runtime_dims_or_strides = runtime_dims_or_strides
                || memory_desc_wrapper(md).has_runtime_dims_or_strides();
    return runtime_dims_or_strides ? unimplemented : success;
}

template <typename... DTs>
bool expect_dt(const memory_desc_t &md, DTs... dts) {
    return IMPLICATION(!is_zero_md(&md), utils::one_of(md.data_type, dts...));
}

status_t expect_dims(const memory_desc_t &md, std::initializer_list<dim_t> dims,
        bool allow_zero = true) {
    if (is_zero_md(&md))
        return (allow_zero || dims.size() == 0) ? success : invalid_arguments;

    if (md.ndims != (int)dims.size()) return invalid_arguments;

    int d_in_md = 0;
    for (auto d : dims)
        if (d != md.dims[d_in_md++]) return invalid_arguments;

    return success;
}

status_t check_data_type_consistency_fwd(const rnn_desc_t &r) {
    using namespace data_type;
    data_type_t src_layer_dt = r.src_layer_desc.data_type;
    data_type_t dst_layer_dt = r.dst_layer_desc.data_type;
    data_type_t weights_iter_dt = r.weights_iter_desc.data_type;
    data_type_t weights_layer_dt = r.weights_layer_desc.data_type;
    data_type_t weights_projection_dt = r.weights_projection_desc.data_type;

    const bool is_forward = !(r.prop_kind == prop_kind::backward);
    const bool is_inference = r.prop_kind == prop_kind::forward_inference;
    const bool is_int8_ok
            = one_of(r.cell_kind, dnnl_vanilla_lstm, dnnl_vanilla_gru);

    const bool cell_state_check = expect_dt(r.src_iter_c_desc, f32, bf16, f16)
            && expect_dt(r.dst_iter_c_desc, f32, bf16, f16);

    const bool is_f32 = everyone_is(f32, src_layer_dt, dst_layer_dt,
                                weights_iter_dt, weights_layer_dt)
            && expect_dt(r.src_iter_desc, f32)
            && expect_dt(r.weights_peephole_desc, f32)
            && expect_dt(r.weights_projection_desc, f32)
            && expect_dt(r.dst_iter_desc, f32) && expect_dt(r.bias_desc, f32);

    const bool is_bf16 = everyone_is(bf16, src_layer_dt, dst_layer_dt,
                                 weights_iter_dt, weights_layer_dt)
            && expect_dt(r.src_iter_desc, bf16)
            && IMPLICATION(r.cell_kind == dnnl_vanilla_lstm,
                    expect_dt(r.weights_peephole_desc, f32))
            /* weights_peephole_desc is reused as attention_desc */
            && IMPLICATION(
                    one_of(r.cell_kind, dnnl_vanilla_augru, dnnl_lbr_augru),
                    expect_dt(r.weights_peephole_desc, bf16))
            && one_of(weights_projection_dt, bf16, data_type::undef)
            && expect_dt(r.dst_iter_desc, bf16)
            && one_of(r.bias_desc.data_type, bf16, f32);

    const bool is_f16 = is_forward
            && everyone_is(f16, src_layer_dt, dst_layer_dt, weights_iter_dt,
                    weights_layer_dt)
            && expect_dt(r.src_iter_desc, f16)
            && expect_dt(r.weights_peephole_desc, f16)
            && r.weights_peephole_desc.data_type == data_type::undef
            && expect_dt(r.dst_iter_desc, f16) && expect_dt(r.bias_desc, f16);

    const bool is_u8u8u8 = is_inference && is_int8_ok && src_layer_dt == u8
            && one_of(dst_layer_dt, u8, f32)
            && everyone_is(s8, weights_iter_dt, weights_layer_dt)
            && expect_dt(r.src_iter_desc, u8)
            && expect_dt(r.src_iter_c_desc, f32)
            && r.weights_peephole_desc.data_type == data_type::undef
            && one_of(weights_projection_dt, s8, data_type::undef)
            && expect_dt(r.dst_iter_desc, u8)
            && expect_dt(r.dst_iter_c_desc, f32) && expect_dt(r.bias_desc, f32);

    const bool is_f32u8f32 = is_inference && is_int8_ok && src_layer_dt == u8
            && everyone_is(s8, weights_iter_dt, weights_layer_dt)
            && r.weights_peephole_desc.data_type == data_type::undef
            && one_of(weights_projection_dt, s8, data_type::undef)
            && one_of(dst_layer_dt, u8, f32) && expect_dt(r.src_iter_desc, f32)
            && expect_dt(r.dst_iter_desc, f32) && expect_dt(r.bias_desc, f32);

    const bool is_s8s8s8 = is_inference && is_int8_ok && src_layer_dt == s8
            && one_of(dst_layer_dt, s8, f32)
            && everyone_is(s8, weights_iter_dt, weights_layer_dt)
            && expect_dt(r.src_iter_desc, s8)
            && expect_dt(r.src_iter_c_desc, f32)
            && r.weights_peephole_desc.data_type == data_type::undef
            && one_of(weights_projection_dt, s8, data_type::undef)
            && expect_dt(r.dst_iter_desc, s8)
            && expect_dt(r.dst_iter_c_desc, f32) && expect_dt(r.bias_desc, f32);

    const bool is_f32s8f32 = is_inference && is_int8_ok && src_layer_dt == s8
            && everyone_is(s8, weights_iter_dt, weights_layer_dt)
            && r.weights_peephole_desc.data_type == data_type::undef
            && one_of(weights_projection_dt, s8, data_type::undef)
            && one_of(dst_layer_dt, s8, f32) && expect_dt(r.src_iter_desc, f32)
            && expect_dt(r.dst_iter_desc, f32) && expect_dt(r.bias_desc, f32);

    return cell_state_check
                    && (is_f32 || is_bf16 || is_f16 || is_u8u8u8 || is_f32u8f32
                            || is_s8s8s8 || is_f32s8f32)
            ? success
            : unimplemented;
}

status_t check_data_type_consistency_bwd(const rnn_desc_t &r) {
    using namespace data_type;

    /* We require diffs to be f32, even for bf16 */
    bool are_diff_f32 = everyone_is(f32, r.diff_src_layer_desc.data_type,
                                r.diff_dst_layer_desc.data_type,
                                r.diff_weights_iter_desc.data_type,
                                r.diff_weights_layer_desc.data_type)
            && expect_dt(r.diff_src_iter_desc, f32)
            && expect_dt(r.diff_dst_iter_desc, f32)
            && expect_dt(r.diff_weights_peephole_desc, f32)
            && expect_dt(r.diff_weights_projection_desc, f32)
            && expect_dt(r.diff_bias_desc, f32)
            && expect_dt(r.diff_src_iter_c_desc, f32)
            && expect_dt(r.diff_dst_iter_c_desc, f32);

    return are_diff_f32 ? success : unimplemented;
}

status_t check_dim_consistency(const rnn_desc_t &r) {
    const bool is_lstm_projection = r.cell_kind == dnnl_vanilla_lstm
            && !is_zero_md(&r.weights_projection_desc);

    const dim_t L = r.weights_layer_desc.dims[0];
    const dim_t T = r.src_layer_desc.dims[0];
    const dim_t N = r.src_layer_desc.dims[1];
    const dim_t D = one_of(r.direction, dnnl_unidirectional_left2right,
                            dnnl_unidirectional_right2left)
            ? 1
            : 2;
    const dim_t G = rnn::get_gates_count(r.cell_kind);
    const dim_t SLC = r.src_layer_desc.dims[2];
    const dim_t SIC = r.weights_iter_desc.dims[2];
    const dim_t DLC = r.dst_layer_desc.dims[2];
    const dim_t DHC = r.weights_layer_desc.dims[4];
    const dim_t DIC
            = is_lstm_projection ? r.weights_projection_desc.dims[3] : DHC;

    const bool extra_bias = utils::one_of(
            r.cell_kind, alg_kind::lbr_gru, alg_kind::lbr_augru);
    const dim_t dlc_multiplier
            = (r.direction == dnnl_bidirectional_concat) ? 2 : 1;

    bool args_ok
            = IMPLICATION(utils::one_of(r.cell_kind, alg_kind::vanilla_gru,
                                  alg_kind::lbr_gru, alg_kind::vanilla_augru,
                                  alg_kind::lbr_augru),
                      SIC == DHC)
            && dlc_multiplier * DIC == DLC
            && IMPLICATION(L > 1, dlc_multiplier * SLC == DLC)
            && IMPLICATION(T > 1, SIC == DIC);
    if (!args_ok) return invalid_arguments;

    const bool is_augru = utils::one_of(
            r.cell_kind, alg_kind::vanilla_augru, alg_kind::lbr_augru);
    CHECK(expect_dims(r.src_layer_desc, {T, N, SLC}, false));
    CHECK(expect_dims(r.src_iter_desc, {L, D, N, SIC}));
    CHECK(expect_dims(r.src_iter_c_desc, {L, D, N, DHC}));
    CHECK(expect_dims(r.weights_layer_desc, {L, D, SLC, G, DHC}, false));
    CHECK(expect_dims(r.weights_iter_desc, {L, D, SIC, G, DHC}, false));
    if (is_augru)
        CHECK(expect_dims(r.weights_peephole_desc, {T, N, 1}));
    else
        CHECK(expect_dims(r.weights_peephole_desc, {L, D, 3, DHC}));
    CHECK(expect_dims(r.weights_projection_desc, {L, D, DHC, DIC}));
    CHECK(expect_dims(r.bias_desc, {L, D, G + extra_bias, DHC}));
    CHECK(expect_dims(r.dst_layer_desc, {T, N, DLC}, false));
    CHECK(expect_dims(r.dst_iter_desc, {L, D, N, DIC}));
    CHECK(expect_dims(r.dst_iter_c_desc, {L, D, N, DHC}));

    if (r.prop_kind == prop_kind::backward) {
        CHECK(expect_dims(r.diff_src_layer_desc, {T, N, SLC}, false));
        CHECK(expect_dims(r.diff_src_iter_desc, {L, D, N, SIC}));
        CHECK(expect_dims(r.diff_src_iter_c_desc, {L, D, N, DHC}));
        CHECK(expect_dims(
                r.diff_weights_layer_desc, {L, D, SLC, G, DHC}, false));
        CHECK(expect_dims(
                r.diff_weights_iter_desc, {L, D, SIC, G, DHC}, false));
        if (is_augru)
            CHECK(expect_dims(r.diff_weights_peephole_desc, {T, N, 1}));
        else
            CHECK(expect_dims(r.diff_weights_peephole_desc, {L, D, 3, DHC}));
        CHECK(expect_dims(r.diff_weights_projection_desc, {L, D, DHC, DIC}));
        CHECK(expect_dims(r.diff_bias_desc, {L, D, G + extra_bias, DHC}));
        CHECK(expect_dims(r.diff_dst_layer_desc, {T, N, DLC}, false));
        CHECK(expect_dims(r.diff_dst_iter_desc, {L, D, N, DIC}));
        CHECK(expect_dims(r.diff_dst_iter_c_desc, {L, D, N, DHC}));
    }

    return success;
}

status_t rnn_common_fwd_desc_init(rnn_desc_t *rnn_desc, prop_kind_t prop_kind,
        alg_kind_t cell_kind, const rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *attention_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc,
        const memory_desc_t *weights_peephole_desc,
        const memory_desc_t *weights_projection_desc,
        const memory_desc_t *bias_desc, const memory_desc_t *dst_layer_desc,
        const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc, unsigned flags,
        alg_kind_t activation = alg_kind::undef, float alpha = 0.0f,
        float beta = 0.0f) {

    // check that a supported cell kind has been passed
    bool args_ok = one_of(cell_kind, dnnl_vanilla_rnn, dnnl_vanilla_lstm,
            dnnl_vanilla_gru, dnnl_lbr_gru, dnnl_vanilla_augru, dnnl_lbr_augru);
    if (!args_ok) return invalid_arguments;

    // check that all mandatory parameters are non-null
    args_ok = args_ok
            && !any_null(src_layer_desc, weights_layer_desc, weights_iter_desc,
                    dst_layer_desc);
    if (!args_ok) return invalid_arguments;

    if (cell_kind == dnnl_vanilla_rnn) {
        using namespace alg_kind;
        args_ok = args_ok
                && one_of(activation, eltwise_relu, eltwise_tanh,
                        eltwise_logistic);
        if (!args_ok) return invalid_arguments;
    }

    if (cell_kind == dnnl_vanilla_lstm) {
        // check if optional *_iter is provided then *_iter_c is provided too
        args_ok = args_ok && xnor_md(src_iter_desc, src_iter_c_desc)
                && xnor_md(dst_iter_desc, dst_iter_c_desc);
        if (!args_ok) return invalid_arguments;
    }

    // check augru-specific restrictions
    const bool is_augru = one_of(cell_kind, dnnl_vanilla_augru, dnnl_lbr_augru);
    if (is_augru) {
        const dim_t L = weights_layer_desc->dims[0];
        args_ok = args_ok && direction == dnnl_unidirectional_left2right
                && L == 1;
        if (!args_ok) return invalid_arguments;
    }

    CHECK(check_runtime_dims_or_strides({src_layer_desc, src_iter_desc,
            src_iter_c_desc, weights_layer_desc, weights_iter_desc,
            weights_peephole_desc, weights_projection_desc, bias_desc,
            dst_layer_desc, dst_iter_desc, dst_iter_c_desc}));

    // Create the descriptor
    auto rd = rnn_desc_t();

    rd.primitive_kind = primitive_kind::rnn;
    rd.prop_kind = prop_kind;
    rd.cell_kind = cell_kind;
    rd.direction = direction;
    maybe_init_md(rd.src_layer_desc, src_layer_desc);
    maybe_init_md(rd.src_iter_desc, src_iter_desc);
    maybe_init_md(rd.src_iter_c_desc, src_iter_c_desc);
    maybe_init_md(rd.weights_layer_desc, weights_layer_desc);
    maybe_init_md(rd.weights_iter_desc, weights_iter_desc);
    maybe_init_md(rd.weights_peephole_desc, weights_peephole_desc);
    if (is_augru) maybe_init_md(rd.weights_peephole_desc, attention_desc);
    maybe_init_md(rd.weights_projection_desc, weights_projection_desc);
    maybe_init_md(rd.bias_desc, bias_desc);
    maybe_init_md(rd.dst_layer_desc, dst_layer_desc);
    maybe_init_md(rd.dst_iter_desc, dst_iter_desc);
    maybe_init_md(rd.dst_iter_c_desc, dst_iter_c_desc);

    rd.flags = flags;
    rd.activation_kind = activation;
    rd.alpha = alpha;
    rd.beta = beta;

    CHECK(check_data_type_consistency_fwd(rd));
    CHECK(check_dim_consistency(rd));

    *rnn_desc = rd;

    return success;
}

status_t rnn_common_bwd_desc_init(rnn_desc_t *rnn_desc, prop_kind_t prop_kind,
        alg_kind_t cell_kind, const dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *attention_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc,
        const memory_desc_t *weights_peephole_desc,
        const memory_desc_t *weights_projection_desc,
        const memory_desc_t *bias_desc, const memory_desc_t *dst_layer_desc,
        const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_src_iter_c_desc,
        const memory_desc_t *diff_attention_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_weights_peephole_desc,
        const memory_desc_t *diff_weights_projection_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc,
        const memory_desc_t *diff_dst_iter_c_desc, unsigned flags,
        alg_kind_t activation = alg_kind::undef, float alpha = 0.0f,
        float beta = 0.0f) {

    // check that a supported cell kind has been passed
    bool args_ok = one_of(cell_kind, dnnl_vanilla_rnn, dnnl_vanilla_lstm,
            dnnl_vanilla_gru, dnnl_lbr_gru, dnnl_vanilla_augru, dnnl_lbr_augru);
    if (!args_ok) return invalid_arguments;

    // check that all mandatory parameters are non-null
    args_ok = args_ok
            && !any_null(src_layer_desc, weights_layer_desc, weights_iter_desc,
                    dst_layer_desc, diff_src_layer_desc,
                    diff_weights_layer_desc, diff_weights_iter_desc,
                    diff_dst_layer_desc);
    if (!args_ok) return invalid_arguments;

    if (cell_kind == dnnl_vanilla_rnn) {
        using namespace alg_kind;
        args_ok = args_ok
                && one_of(activation, eltwise_relu, eltwise_tanh,
                        eltwise_logistic);
        if (!args_ok) return invalid_arguments;
    }

    if (cell_kind == dnnl_vanilla_lstm) {
        // check if optional *_iter is provided then *_iter_c is provided too
        args_ok = args_ok && xnor_md(src_iter_desc, src_iter_c_desc)
                && xnor_md(dst_iter_desc, dst_iter_c_desc);
        if (!args_ok) return invalid_arguments;
    }

    const bool is_augru = one_of(cell_kind, dnnl_vanilla_augru, dnnl_lbr_augru);
    // check augru-specific restrictions
    if (is_augru) {
        const dim_t L = weights_layer_desc->dims[0];
        const bool dims_ok = args_ok
                && direction == dnnl_unidirectional_left2right && L == 1;
        const bool descs_ok = !any_null(attention_desc, diff_attention_desc);
        const bool args_ok = dims_ok && descs_ok;
        if (!args_ok) return invalid_arguments;
    }

    // check if optional md is provided then diff_md is provided too
    args_ok = args_ok && xnor_md(bias_desc, diff_bias_desc)
            && xnor_md(weights_peephole_desc, diff_weights_peephole_desc)
            && xnor_md(weights_projection_desc, diff_weights_projection_desc)
            && xnor_md(src_iter_desc, diff_src_iter_desc)
            && xnor_md(src_iter_c_desc, diff_src_iter_c_desc)
            && xnor_md(dst_iter_desc, diff_dst_iter_desc)
            && xnor_md(dst_iter_c_desc, diff_dst_iter_c_desc);
    if (!args_ok) return invalid_arguments;

    CHECK(check_runtime_dims_or_strides({src_layer_desc, src_iter_desc,
            src_iter_c_desc, attention_desc, weights_layer_desc,
            weights_iter_desc, weights_peephole_desc, weights_projection_desc,
            bias_desc, dst_layer_desc, dst_iter_desc, dst_iter_c_desc,
            diff_src_layer_desc, diff_src_iter_desc, diff_src_iter_c_desc,
            diff_attention_desc, diff_weights_layer_desc,
            diff_weights_iter_desc, diff_weights_peephole_desc,
            diff_weights_projection_desc, diff_bias_desc, diff_dst_layer_desc,
            diff_dst_iter_desc, diff_dst_iter_c_desc}));

    auto rd = rnn_desc_t();

    rd.primitive_kind = primitive_kind::rnn;
    rd.prop_kind = prop_kind;
    rd.cell_kind = cell_kind;
    rd.direction = direction;

    maybe_init_md(rd.src_layer_desc, src_layer_desc);
    maybe_init_md(rd.src_iter_desc, src_iter_desc);
    maybe_init_md(rd.src_iter_c_desc, src_iter_c_desc);
    maybe_init_md(rd.weights_layer_desc, weights_layer_desc);
    maybe_init_md(rd.weights_iter_desc, weights_iter_desc);
    maybe_init_md(rd.weights_peephole_desc, weights_peephole_desc);
    if (is_augru) maybe_init_md(rd.weights_peephole_desc, attention_desc);
    maybe_init_md(rd.weights_projection_desc, weights_projection_desc);
    maybe_init_md(rd.bias_desc, bias_desc);
    maybe_init_md(rd.dst_layer_desc, dst_layer_desc);
    maybe_init_md(rd.dst_iter_desc, dst_iter_desc);
    maybe_init_md(rd.dst_iter_c_desc, dst_iter_c_desc);
    maybe_init_md(rd.diff_src_layer_desc, diff_src_layer_desc);
    maybe_init_md(rd.diff_src_iter_desc, diff_src_iter_desc);
    maybe_init_md(rd.diff_src_iter_c_desc, diff_src_iter_c_desc);
    maybe_init_md(rd.diff_weights_layer_desc, diff_weights_layer_desc);
    maybe_init_md(rd.diff_weights_iter_desc, diff_weights_iter_desc);
    maybe_init_md(rd.diff_weights_peephole_desc, diff_weights_peephole_desc);
    if (is_augru)
        maybe_init_md(rd.diff_weights_peephole_desc, diff_attention_desc);
    maybe_init_md(
            rd.diff_weights_projection_desc, diff_weights_projection_desc);
    maybe_init_md(rd.diff_bias_desc, diff_bias_desc);
    maybe_init_md(rd.diff_dst_layer_desc, diff_dst_layer_desc);
    maybe_init_md(rd.diff_dst_iter_desc, diff_dst_iter_desc);
    maybe_init_md(rd.diff_dst_iter_c_desc, diff_dst_iter_c_desc);

    rd.flags = flags;
    rd.activation_kind = activation;
    rd.alpha = alpha;
    rd.beta = beta;

    CHECK(check_data_type_consistency_fwd(rd));
    CHECK(check_data_type_consistency_bwd(rd));

    CHECK(check_dim_consistency(rd));

    *rnn_desc = rd;

    return success;
}

} // namespace

/* Public C Api */

status_t dnnl_vanilla_rnn_forward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, const dnnl_alg_kind_t activation,
        const dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        unsigned flags, float alpha, float beta, const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_fwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_rnn,
            direction, src_layer_desc, src_iter_desc, nullptr, nullptr,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, flags, activation, alpha,
            beta));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, nullptr, attr);
}

status_t dnnl_lstm_forward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc, unsigned flags,
        const primitive_attr_t *attr) {

    return dnnl_lstm_forward_primitive_desc_create_v3(primitive_desc_iface,
            engine, prop_kind, direction, src_layer_desc, src_iter_desc,
            src_iter_c_desc, weights_layer_desc, weights_iter_desc, nullptr,
            nullptr, bias_desc, dst_layer_desc, dst_iter_desc, dst_iter_c_desc,
            flags, attr);
}

status_t dnnl_lstm_forward_primitive_desc_create_v2(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc,
        const memory_desc_t *weights_peephole_desc,
        const memory_desc_t *bias_desc, const memory_desc_t *dst_layer_desc,
        const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc, unsigned flags,
        const primitive_attr_t *attr) {

    return dnnl_lstm_forward_primitive_desc_create_v3(primitive_desc_iface,
            engine, prop_kind, direction, src_layer_desc, src_iter_desc,
            src_iter_c_desc, weights_layer_desc, weights_iter_desc,
            weights_peephole_desc, nullptr, bias_desc, dst_layer_desc,
            dst_iter_desc, dst_iter_c_desc, flags, attr);
}

status_t dnnl_lstm_forward_primitive_desc_create_v3(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc,
        const memory_desc_t *weights_peephole_desc,
        const memory_desc_t *weights_projection_desc,
        const memory_desc_t *bias_desc, const memory_desc_t *dst_layer_desc,
        const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc, unsigned flags,
        const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_fwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_lstm,
            direction, src_layer_desc, src_iter_desc, src_iter_c_desc, nullptr,
            weights_layer_desc, weights_iter_desc, weights_peephole_desc,
            weights_projection_desc, bias_desc, dst_layer_desc, dst_iter_desc,
            dst_iter_c_desc, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, nullptr, attr);
}

status_t dnnl_gru_forward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        unsigned flags, const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_fwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_gru,
            direction, src_layer_desc, src_iter_desc, nullptr, nullptr,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, nullptr, attr);
}

status_t dnnl_lbr_gru_forward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        unsigned flags, const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_fwd_desc_init(&rnn_desc, prop_kind, dnnl_lbr_gru,
            direction, src_layer_desc, src_iter_desc, nullptr, nullptr,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, nullptr, attr);
}

status_t dnnl_augru_forward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *attention_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        unsigned flags, const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_fwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_augru,
            direction, src_layer_desc, src_iter_desc, nullptr, attention_desc,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, nullptr, attr);
}

status_t dnnl_lbr_augru_forward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *attention_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        unsigned flags, const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_fwd_desc_init(&rnn_desc, prop_kind, dnnl_lbr_augru,
            direction, src_layer_desc, src_iter_desc, nullptr, attention_desc,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, nullptr, attr);
}

status_t dnnl_vanilla_rnn_backward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, const dnnl_alg_kind_t activation,
        const dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc, unsigned flags, float alpha,
        float beta, const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_bwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_rnn,
            direction, src_layer_desc, src_iter_desc, nullptr, nullptr,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, diff_src_layer_desc,
            diff_src_iter_desc, nullptr, nullptr, diff_weights_layer_desc,
            diff_weights_iter_desc, nullptr, nullptr, diff_bias_desc,
            diff_dst_layer_desc, diff_dst_iter_desc, nullptr, flags, activation,
            alpha, beta));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, hint_fwd_pd, attr);
}

status_t dnnl_lstm_backward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_src_iter_c_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc,
        const memory_desc_t *diff_dst_iter_c_desc, unsigned flags,
        const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    return dnnl_lstm_backward_primitive_desc_create_v3(primitive_desc_iface,
            engine, prop_kind, direction, src_layer_desc, src_iter_desc,
            src_iter_c_desc, weights_layer_desc, weights_iter_desc, nullptr,
            nullptr, bias_desc, dst_layer_desc, dst_iter_desc, dst_iter_c_desc,
            diff_src_layer_desc, diff_src_iter_desc, diff_src_iter_c_desc,
            diff_weights_layer_desc, diff_weights_iter_desc, nullptr, nullptr,
            diff_bias_desc, diff_dst_layer_desc, diff_dst_iter_desc,
            diff_dst_iter_c_desc, flags, hint_fwd_pd, attr);
}

status_t dnnl_lstm_backward_primitive_desc_create_v2(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc,
        const memory_desc_t *weights_peephole_desc,
        const memory_desc_t *bias_desc, const memory_desc_t *dst_layer_desc,
        const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_src_iter_c_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_weights_peephole_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc,
        const memory_desc_t *diff_dst_iter_c_desc, unsigned flags,
        const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    return dnnl_lstm_backward_primitive_desc_create_v3(primitive_desc_iface,
            engine, prop_kind, direction, src_layer_desc, src_iter_desc,
            src_iter_c_desc, weights_layer_desc, weights_iter_desc,
            weights_peephole_desc, nullptr, bias_desc, dst_layer_desc,
            dst_iter_desc, dst_iter_c_desc, diff_src_layer_desc,
            diff_src_iter_desc, diff_src_iter_c_desc, diff_weights_layer_desc,
            diff_weights_iter_desc, diff_weights_peephole_desc, nullptr,
            diff_bias_desc, diff_dst_layer_desc, diff_dst_iter_desc,
            diff_dst_iter_c_desc, flags, hint_fwd_pd, attr);
}

status_t dnnl_lstm_backward_primitive_desc_create_v3(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *src_iter_c_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc,
        const memory_desc_t *weights_peephole_desc,
        const memory_desc_t *weights_projection_desc,
        const memory_desc_t *bias_desc, const memory_desc_t *dst_layer_desc,
        const memory_desc_t *dst_iter_desc,
        const memory_desc_t *dst_iter_c_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_src_iter_c_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_weights_peephole_desc,
        const memory_desc_t *diff_weights_projection_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc,
        const memory_desc_t *diff_dst_iter_c_desc, unsigned flags,
        const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_bwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_lstm,
            direction, src_layer_desc, src_iter_desc, src_iter_c_desc, nullptr,
            weights_layer_desc, weights_iter_desc, weights_peephole_desc,
            weights_projection_desc, bias_desc, dst_layer_desc, dst_iter_desc,
            dst_iter_c_desc, diff_src_layer_desc, diff_src_iter_desc,
            diff_src_iter_c_desc, nullptr, diff_weights_layer_desc,
            diff_weights_iter_desc, diff_weights_peephole_desc,
            diff_weights_projection_desc, diff_bias_desc, diff_dst_layer_desc,
            diff_dst_iter_desc, diff_dst_iter_c_desc, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, hint_fwd_pd, attr);
}

status_t dnnl_gru_backward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc, unsigned flags,
        const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_bwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_gru,
            direction, src_layer_desc, src_iter_desc, nullptr, nullptr,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, diff_src_layer_desc,
            diff_src_iter_desc, nullptr, nullptr, diff_weights_layer_desc,
            diff_weights_iter_desc, nullptr, nullptr, diff_bias_desc,
            diff_dst_layer_desc, diff_dst_iter_desc, nullptr, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, hint_fwd_pd, attr);
}

status_t dnnl_lbr_gru_backward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc, unsigned flags,
        const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_bwd_desc_init(&rnn_desc, prop_kind, dnnl_lbr_gru,
            direction, src_layer_desc, src_iter_desc, nullptr, nullptr,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, diff_src_layer_desc,
            diff_src_iter_desc, nullptr, nullptr, diff_weights_layer_desc,
            diff_weights_iter_desc, nullptr, nullptr, diff_bias_desc,
            diff_dst_layer_desc, diff_dst_iter_desc, nullptr, flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, hint_fwd_pd, attr);
}

status_t dnnl_augru_backward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *attention_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_attention_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc, unsigned flags,
        const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_bwd_desc_init(&rnn_desc, prop_kind, dnnl_vanilla_augru,
            direction, src_layer_desc, src_iter_desc, nullptr, attention_desc,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, diff_src_layer_desc,
            diff_src_iter_desc, nullptr, diff_attention_desc,
            diff_weights_layer_desc, diff_weights_iter_desc, nullptr, nullptr,
            diff_bias_desc, diff_dst_layer_desc, diff_dst_iter_desc, nullptr,
            flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, hint_fwd_pd, attr);
}

status_t dnnl_lbr_augru_backward_primitive_desc_create(
        primitive_desc_iface_t **primitive_desc_iface, engine_t *engine,
        dnnl_prop_kind_t prop_kind, dnnl_rnn_direction_t direction,
        const memory_desc_t *src_layer_desc, const memory_desc_t *src_iter_desc,
        const memory_desc_t *attention_desc,
        const memory_desc_t *weights_layer_desc,
        const memory_desc_t *weights_iter_desc, const memory_desc_t *bias_desc,
        const memory_desc_t *dst_layer_desc, const memory_desc_t *dst_iter_desc,
        const memory_desc_t *diff_src_layer_desc,
        const memory_desc_t *diff_src_iter_desc,
        const memory_desc_t *diff_attention_desc,
        const memory_desc_t *diff_weights_layer_desc,
        const memory_desc_t *diff_weights_iter_desc,
        const memory_desc_t *diff_bias_desc,
        const memory_desc_t *diff_dst_layer_desc,
        const memory_desc_t *diff_dst_iter_desc, unsigned flags,
        const primitive_desc_iface_t *hint_fwd_pd,
        const primitive_attr_t *attr) {

    auto rnn_desc = rnn_desc_t();
    CHECK(rnn_common_bwd_desc_init(&rnn_desc, prop_kind, dnnl_lbr_augru,
            direction, src_layer_desc, src_iter_desc, nullptr, attention_desc,
            weights_layer_desc, weights_iter_desc, nullptr, nullptr, bias_desc,
            dst_layer_desc, dst_iter_desc, nullptr, diff_src_layer_desc,
            diff_src_iter_desc, nullptr, diff_attention_desc,
            diff_weights_layer_desc, diff_weights_iter_desc, nullptr, nullptr,
            diff_bias_desc, diff_dst_layer_desc, diff_dst_iter_desc, nullptr,
            flags));
    return primitive_desc_create(primitive_desc_iface, engine,
            (const op_desc_t *)&rnn_desc, hint_fwd_pd, attr);
}
