// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "UtilsAndTypes.hpp"
#include "tensor_i.hpp"
#include "x.hpp"
#include "p2c_ids.hpp"

struct Creator
{
    static tensor_i::ptr_type create_from_shape(CreatorId op, shape_type && shape, DType dtype=DT_FLOAT64);
    static tensor_i::ptr_type full(shape_type && shape, py::object && val, DType dtype=DT_FLOAT64);
};

struct IEWBinOp
{
    static void op(IEWBinOpId op, x::DPTensorBaseX::ptr_type a, x::DPTensorBaseX::ptr_type b);
};

struct EWBinOp
{
    static tensor_i::ptr_type op(EWBinOpId op, x::DPTensorBaseX::ptr_type a, x::DPTensorBaseX::ptr_type b);
};

struct EWUnyOp
{
    static tensor_i::ptr_type op(EWUnyOpId op, x::DPTensorBaseX::ptr_type a);
};

struct ReduceOp
{
    static tensor_i::ptr_type op(ReduceOpId op, x::DPTensorBaseX::ptr_type a, const dim_vec_type & dim);
};

struct GetItem
{
    static tensor_i::ptr_type __getitem__(x::DPTensorBaseX::ptr_type a, const std::vector<py::slice> & v);
    static py::object get_slice(x::DPTensorBaseX::ptr_type a, const std::vector<py::slice> & v);
};

struct SetItem
{
    static void __setitem__(x::DPTensorBaseX::ptr_type a, const std::vector<py::slice> & v, x::DPTensorBaseX::ptr_type b);
};


// Dependent on dt, dispatch arguments to a operation class.
// The operation must
//    * be a template class accepting the element type as argument
//    * implement one or more "op" methods matching the given arguments (args)
// All arguments other than dt are opaquely passed to the operation.
template<template<typename OD> class OpDispatch, typename... Ts>
auto TypeDispatch(DType dt, Ts&&... args)
{
    switch(dt) {
    case DT_FLOAT64:
        return OpDispatch<double>::op(std::forward<Ts>(args)...);
    case DT_INT64:
        return OpDispatch<int64_t>::op(std::forward<Ts>(args)...);
    case DT_FLOAT32:
        return OpDispatch<float>::op(std::forward<Ts>(args)...);
    case DT_INT32:
        return OpDispatch<int32_t>::op(std::forward<Ts>(args)...);
    case DT_INT16:
        return OpDispatch<int16_t>::op(std::forward<Ts>(args)...);
    case DT_UINT64:
        return OpDispatch<uint64_t>::op(std::forward<Ts>(args)...);
    case DT_UINT32:
        return OpDispatch<uint32_t>::op(std::forward<Ts>(args)...);
    case DT_UINT16:
        return OpDispatch<uint16_t>::op(std::forward<Ts>(args)...);
        /* FIXME
    case DT_BOOL:
        return OpDispatch<bool>::op(std::forward<Ts>(args)...);
        */
    default:
        throw std::runtime_error("unknown dtype");
    }
}
