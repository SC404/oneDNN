/*******************************************************************************
* Copyright 2024-2025 Intel Corporation
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


#ifndef GEMMSTONE_GUARD_QUANTIZATION_HPP
#define GEMMSTONE_GUARD_QUANTIZATION_HPP

#include "gemmstone/type.hpp"
#include "register_layout.hpp"

GEMMSTONE_NAMESPACE_START

// Check if the optimized int4 dequantization sequence (dequantizeInt4) can be used.
bool canDequantizeInt4(const RegisterLayout &layoutSrc, const RegisterLayout &layoutDst,
                       const RegisterLayout &layoutOffset, const RegisterLayout &layoutScale);

GEMMSTONE_NAMESPACE_END

#endif /* header guard */
