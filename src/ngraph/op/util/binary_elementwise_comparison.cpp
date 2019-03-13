//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include "ngraph/op/util/binary_elementwise_comparison.hpp"

using namespace std;
using namespace ngraph;

op::util::BinaryElementwiseComparison::BinaryElementwiseComparison(const string& node_type,
                                                                   const NodeOutput& arg0,
                                                                   const NodeOutput& arg1)
    : Op(node_type, {arg0, arg1})
{
}

void op::util::BinaryElementwiseComparison::validate_and_infer_types()
{
    auto args_et_pshape = validate_and_infer_elementwise_args();
    PartialShape& args_pshape = std::get<1>(args_et_pshape);

    set_output_type(0, element::boolean, args_pshape);
}
