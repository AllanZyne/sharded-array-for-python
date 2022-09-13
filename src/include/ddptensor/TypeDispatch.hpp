// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "CppTypes.hpp"

template<typename DispatchFunc>
void dispatch(DTypeId dt, void * ptr, DispatchFunc func)
{
    switch(dt) {
    case FLOAT64:
        func(reinterpret_cast<double*>(ptr));
        break;
    case FLOAT32:
        func(reinterpret_cast<float*>(ptr));
        break;
    case INT64:
        func(reinterpret_cast<int64_t*>(ptr));
        break;
    case INT32:
        func(reinterpret_cast<int32_t*>(ptr));
        break;
    case INT16:
        func(reinterpret_cast<int16_t*>(ptr));
        break;
    case INT8:
        func(reinterpret_cast<int8_t*>(ptr));
        break;
    case UINT64:
        func(reinterpret_cast<uint64_t*>(ptr));
        break;
    case UINT32:
        func(reinterpret_cast<uint32_t*>(ptr));
        break;
    case UINT16:
        func(reinterpret_cast<uint16_t*>(ptr));
        break;
    case UINT8:
        func(reinterpret_cast<uint8_t*>(ptr));
        break;
    case BOOL:
        func(reinterpret_cast<bool*>(ptr));
        break;
    default:
        throw std::runtime_error("unknown dtype");
    }
}

#if 0
// Dependent on dt, dispatch arguments to a operation class.
// The operation must
//    * be a template class accepting the element type as argument
//    * implement one or more "op" methods matching the given arguments (args)
// All arguments other than dt are opaquely passed to the operation.
template<template<typename OD> class OpDispatch, typename... Ts>
auto TypeDispatch(DTypeId dt, Ts&&... args)
{
    switch(dt) {
    case FLOAT64:
        return OpDispatch<double>::op(std::forward<Ts>(args)...);
    case INT64:
        return OpDispatch<int64_t>::op(std::forward<Ts>(args)...);
#if ! defined(DDPT_2TYPES)
    case FLOAT32:
        return OpDispatch<float>::op(std::forward<Ts>(args)...);
    case INT32:
        return OpDispatch<int32_t>::op(std::forward<Ts>(args)...);
    case INT16:
        return OpDispatch<int16_t>::op(std::forward<Ts>(args)...);
    case INT8:
        return OpDispatch<int8_t>::op(std::forward<Ts>(args)...);
    case UINT64:
        return OpDispatch<uint64_t>::op(std::forward<Ts>(args)...);
    case UINT32:
        return OpDispatch<uint32_t>::op(std::forward<Ts>(args)...);
    case UINT16:
        return OpDispatch<uint16_t>::op(std::forward<Ts>(args)...);
    case UINT8:
        return OpDispatch<uint8_t>::op(std::forward<Ts>(args)...);
        /* FIXME
    case BOOL:
        return OpDispatch<bool>::op(std::forward<Ts>(args)...);
        */
#endif
    default:
        throw std::runtime_error("unknown dtype");
    }
}

template<typename A>
auto /*typename x::DPTensorX<A>::typed_ptr_type*/_downcast(const tensor_i::ptr_type & a_ptr)
{
    auto _a = std::dynamic_pointer_cast<x::DPTensorX<A>>(a_ptr);
    if(!_a )
        throw std::runtime_error("Invalid array object: dynamic cast failed");
    return _a;
}

// Dependent on dt, dispatch arguments to a operation class.
// The operation must implement one or more "op" methods matching the given arguments (args)
// Recursiovely called to downcast tensor_i::ptr_type to typed shared_pointers.
// Downcasted tensor arg is removed from front of arg list and appended to its end.
// All other arguments are opaquely passed to the operation.
template<typename OpDispatch, typename... Ts>
auto TypeDispatch(const tensor_i::ptr_type & a_ptr, Ts&&... args)
{
    using ptr_type = tensor_i::ptr_type;
    switch(a_ptr->dtype()) {
    case FLOAT64:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<double>(a_ptr));
    case INT64:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<int64_t>(a_ptr));
#if ! defined(DDPT_2TYPES)
    case FLOAT32:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<float>(a_ptr));
    case INT32:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<int32_t>(a_ptr));
    case INT16:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<int16_t>(a_ptr));
    case INT8:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<int8_t>(a_ptr));
    case UINT64:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<uint64_t>(a_ptr));
    case UINT32:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<uint32_t>(a_ptr));
    case UINT16:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<uint16_t>(a_ptr));
    case UINT8:
        return TypeDispatch<OpDispatch>(std::forward<Ts>(args)..., _downcast<uint8_t>(a_ptr));
        /* FIXME
    case BOOL:
        return TypeDispatch2<OpDispatch>(std::forward<Ts>(args)..., _downcast<bool>(a_ptr));
        */
#endif
    default:
        throw std::runtime_error("unknown dtype");
    }
}

// Root overload when no untyped tensors are in parameter pack.
template<typename OpDispatch, typename... Ts>
auto TypeDispatch(Ts&&... args)
{
    return OpDispatch::op(std::forward<Ts>(args)...);
}
#endif