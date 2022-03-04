// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "UtilsAndTypes.hpp"
#include "tensor_i.hpp"
#include "p2c_ids.hpp"

struct IEWBinOp
{
    static tensor_i::future_type op(IEWBinOpId op, tensor_i::future_type & a, const py::object & b);
};
