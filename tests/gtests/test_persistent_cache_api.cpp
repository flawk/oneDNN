/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
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

#include "dnnl_test_common.hpp"
#include "gtest/gtest.h"

#include "oneapi/dnnl/dnnl.hpp"

namespace dnnl {

class persistent_cache_api_test_t : public ::testing::Test {};

HANDLE_EXCEPTIONS_FOR_TEST(
        persistent_cache_api_test_t, TestPersistentCacheAPI) {
    engine e = get_test_engine();
    auto pd = convolution_forward::primitive_desc {e,
            prop_kind::forward_training, algorithm::convolution_direct,
            {{2, 16, 16, 16}, memory::data_type::f32, memory::format_tag::nchw},
            {{16, 16, 3, 3}, memory::data_type::f32, memory::format_tag::oihw},
            {{2, 16, 14, 14}, memory::data_type::f32, memory::format_tag::nchw},
            {1, 1}, {0, 0}, {0, 0}};
    auto p = convolution_forward(pd);

    std::vector<uint8_t> cache_blob_id;
    std::vector<uint8_t> cache_blob;

    ASSERT_NO_THROW(cache_blob_id = pd.get_cache_blob_id());
    ASSERT_EQ(cache_blob_id, pd.get_cache_blob_id());

    if (get_test_engine_kind() != engine::kind::gpu
            || (get_test_engine_kind() == engine::kind::gpu
                    && DNNL_GPU_RUNTIME != DNNL_RUNTIME_OCL)) {
        ASSERT_EQ(cache_blob_id.empty(), true);
        EXPECT_ANY_THROW(cache_blob = p.get_cache_blob());
        ASSERT_EQ(cache_blob.empty(), true);
        EXPECT_ANY_THROW(convolution_forward(pd, cache_blob));
    } else {
        ASSERT_EQ(cache_blob_id.empty(), false);
        ASSERT_NO_THROW(cache_blob = p.get_cache_blob());
        ASSERT_EQ(cache_blob.empty(), false);
        ASSERT_NO_THROW(p = convolution_forward(pd, cache_blob));
        ASSERT_EQ(cache_blob, p.get_cache_blob());
    }
}

} // namespace dnnl
