/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <migraphx/common.hpp>
#include <migraphx/module.hpp>
#include <migraphx/make_op.hpp>
#include <migraphx/algorithm.hpp>
#include <migraphx/stringutils.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/ranges.hpp>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {
std::vector<std::size_t> compute_broadcasted_lens(std::vector<std::size_t> s0,
                                                  std::vector<std::size_t> s1)
{
    if(s0 == s1)
        return s0;
    if(s0.size() > s1.size())
        s0.swap(s1);
    std::vector<std::size_t> out_lens(s1);
    auto offset = s1.size() - s0.size();
    std::transform(
        s0.begin(), s0.end(), s1.begin() + offset, out_lens.begin() + offset, [&](auto a, auto b) {
            if(a != b and a != 1 and b != 1)
            {
                MIGRAPHX_THROW("COMPUTE_BROADCASTLEN: shape {" + migraphx::to_string_range(s0) +
                               "} and {" + migraphx::to_string_range(s1) + "} mismatch!");
            }
            return std::max(a, b);
        });
    return out_lens;
}

std::vector<shape::dynamic_dimension> compute_broadcasted_dyn_dims(shape s0, shape s1)
{
    // change both shapes to dynamic_dimension representation
    s0 = s0.to_dynamic();
    s1 = s1.to_dynamic();
    if(s0.ndim() > s1.ndim())
    {
        std::swap(s0, s1);
    }
    auto offset = s1.ndim() - s0.ndim();
    std::vector<shape::dynamic_dimension> out_dims(s1.dyn_dims());
    std::transform(s0.dyn_dims().cbegin(),
                   s0.dyn_dims().cend(),
                   s1.dyn_dims().cbegin() + offset,
                   out_dims.begin() + offset,
                   [&](auto a, auto b) {
                       if(a == b or b == 1)
                       {
                           return a;
                       }
                       else if(a == 1)
                       {
                           return b;
                       }
                       else
                       {
                           MIGRAPHX_THROW("COMPUTE_BROADCASTED_DYN_DIMS: dynamic shapes {" +
                                          migraphx::to_string_range(s0.dyn_dims()) + "} and {" +
                                          migraphx::to_string_range(s1.dyn_dims()) + "} mismatch!");
                       }
                   });
    return out_dims;
}

std::vector<shape::dynamic_dimension> compute_common_dyn_dims(const std::vector<shape>& shapes)
{
    auto ret_shape = shapes.at(0);
    std::for_each(shapes.cbegin() + 1, shapes.cend(), [&](auto s) {
        ret_shape = shape{ret_shape.type(), compute_broadcasted_dyn_dims(ret_shape, s)};
    });
    return ret_shape.dyn_dims();
}

std::vector<std::size_t> compute_common_lens(const std::vector<shape>& shapes)
{
    assert(not shapes.empty());
    assert(
        std::none_of(shapes.cbegin(), shapes.cend(), [](auto shape) { return shape.dynamic(); }));
    return transform_accumulate(shapes.begin() + 1,
                                shapes.end(),
                                shapes.front().lens(),
                                &compute_broadcasted_lens,
                                [](auto s) { return s.lens(); });
}

shape::type_t compute_common_type(shape::type_t t1, shape::type_t t2)
{
    if(t1 == t2)
        return t1;
    shape::type_t result;
    shape::visit(t1, [&](auto x) {
        shape::visit(t2, [&](auto y) {
            // Workaround broken warning on gcc 5
            (void)x;
            (void)y;
            using type = std::common_type_t<decltype(x()), decltype(y())>;
            result     = shape::get_type<type>{};
        });
    });
    return result;
}

shape::type_t compute_common_types(const std::vector<shape>& shapes)
{
    assert(not shapes.empty());
    return transform_accumulate(
        shapes.begin() + 1, shapes.end(), shapes.front().type(), &compute_common_type, [&](auto s) {
            return s.type();
        });
}

shape common_shape(const std::vector<shape>& shapes)
{
    if(shapes.empty())
        return {};
    return {compute_common_types(shapes), compute_common_lens(shapes)};
}

std::vector<instruction_ref>
insert_common_args(module& m, instruction_ref ins, std::vector<instruction_ref> inputs)
{
    if(std::any_of(
           inputs.cbegin(), inputs.cend(), [](auto input) { return input->get_shape().dynamic(); }))
    {
        auto input_shapes = to_shapes(inputs);
        auto c_type       = compute_common_types(input_shapes);
        auto c_dyn_dims   = compute_common_dyn_dims(input_shapes);

        // following should work for a static or dynamic shape
        if(inputs[0]->get_shape().dyn_dims() != c_dyn_dims)
        {
            inputs[0] = m.insert_instruction(
                ins, make_op("multibroadcast", {{"out_dyn_dims", to_value(c_dyn_dims)}}), inputs);
        }
        std::transform(inputs.begin() + 1, inputs.end(), inputs.begin() + 1, [&](auto input) {
            // uses previous input to avoid recalculating the common shape from the
            // full set of input shapes at runtime
            if(input->get_shape().dyn_dims() != c_dyn_dims)
            {
                return m.insert_instruction(
                    ins,
                    make_op("multibroadcast", {{"out_dyn_dims", to_value(c_dyn_dims)}}),
                    input,
                    inputs[0]);
            }
            return input;
        });
        std::transform(inputs.begin(), inputs.end(), inputs.begin(), [&](auto input) {
            if(input->get_shape().type() != c_type)
            {
                input =
                    m.insert_instruction(ins, make_op("convert", {{"target_type", c_type}}), input);
            }
            return input;
        });
    }
    else
    {
        auto common = common_shape(to_shapes(inputs));
        std::transform(inputs.begin(), inputs.end(), inputs.begin(), [&](auto input) {
            if(input->get_shape().lens() != common.lens())
            {
                input = m.insert_instruction(
                    ins, make_op("multibroadcast", {{"out_lens", common.lens()}}), input);
            }
            if(input->get_shape().type() != common.type())
            {
                input = m.insert_instruction(
                    ins, make_op("convert", {{"target_type", common.type()}}), input);
            }
            return input;
        });
    }
    return inputs;
}

std::vector<instruction_ref> add_common_args(module& m, std::vector<instruction_ref> inputs)
{
    return insert_common_args(m, m.end(), std::move(inputs));
}

instruction_ref insert_common_op(module& m,
                                 instruction_ref ins,
                                 const operation& op,
                                 std::vector<instruction_ref> inputs)
{
    return m.insert_instruction(ins, op, insert_common_args(m, ins, std::move(inputs)));
}

instruction_ref add_common_op(module& m, const operation& op, std::vector<instruction_ref> inputs)
{
    return insert_common_op(m, m.end(), op, std::move(inputs));
}

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
