/*******************************************************************************
* Copyright 2022 Intel Corporation
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

#ifndef GPU_JIT_UTILS_TRACE_HPP
#define GPU_JIT_UTILS_TRACE_HPP

#include "gpu/jit/ir/ir.hpp"
#include "gpu/jit/utils/utils.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

// Trace for debugging purposes.
#ifdef GEN_CONV_PROFILE
ir_utils::debug_profiler_t &get_trace_profiler();
inline void trace_start() {
    get_trace_profiler().start();
}
inline void trace_reset() {
    get_trace_profiler().reset();
}
inline void trace_stamp(const char *pass_name) {
    get_trace_profiler().stamp(pass_name);
}
inline void trace_stop(const char *pass_name) {
    get_trace_profiler().stop(pass_name);
}
inline void trace_perf() {
    ir_perf() << get_trace_profiler() << std::endl;
}
#else
inline void trace_start() {};
inline void trace_reset() {};
inline void trace_stamp(const char *) {};
inline void trace_stop(const char *) {};
inline void trace_perf() {};
#endif

#if defined(GEN_CONV_PROFILE) || defined(GEN_CONV_DEBUG)
void trace_pass(
        const char *pass_name, const stmt_t &stmt, ir_context_t &ir_ctx);
#else
inline void trace_pass(
        const char *pass_name, const stmt_t &stmt, ir_context_t &ir_ctx) {};
#endif

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
