// SPDX-License-Identifier: BSD-3-Clause

/*
  Core MLIR functionality.
  - Adding/creating input and output to functions
  - Handling MLIR compiler machinery

  To reduce compile/link time only MLIR dialects and passes get
  registered/linked which are actually used.

  A typical jit cycle is controlled by the worker process executing
  process_promises.
  - create MLIR module/function
  - adding deferred operations
  - adding appropriate casts and return statements
  - updating function signature to accept existing tensors and returning new and
  live ones

  Typically operations have input dependences, e.g. tensors produced by other
  operations. These can either come from outside the jit'ed function or be
  created within the function. Since we strictly add operations in serial order
  input dependences must lready exists. Deps are represented by guids and stored
  in the Registry.

  Internally ddpt's MLIR machinery keeps track of created and needed tensors.
  Those which were not created internally are added as input arguments to the
  jit-function. Those which are live (not destructed within the function) when
  the function is finalized are added as return values.

  MLIR/LLVM supports a single return value only. Following LLVM's policy we need
  to pack al return tensors into one large buffer/struct. Input tensors get
  represented as a series of arguments, as defined by MLIR/LLVM and IMEX's dist
  dialect.
*/

#include "ddptensor/jit/mlir.hpp"
#include "ddptensor/DDPTensorImpl.hpp"
#include "ddptensor/Registry.hpp"

// #include "llvm/Support/InitLLVM.h"

#include "mlir/IR/MLIRContext.h"

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include <mlir/Dialect/Linalg/IR/Linalg.h>

#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
// #include "mlir/Transforms/Passes.h"
#include "llvm/ADT/Twine.h"

#include "mlir/Conversion/Passes.h"
#include "mlir/Dialect/Affine/Passes.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/Pass/PassRegistry.h"
// #include "mlir/Dialect/Async/Passes.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/Transforms/Passes.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Dialect/LLVMIR/Transforms/Passes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
// #include "mlir/Dialect/NVGPU/Passes.h"
#include "mlir/Dialect/SCF/Transforms/Passes.h"
#include "mlir/Dialect/SPIRV/Transforms/Passes.h"
#include "mlir/Dialect/Shape/Transforms/Passes.h"
// #include "mlir/Dialect/SparseTensor/Pipelines/Passes.h"
// #include "mlir/Dialect/SparseTensor/Transforms/Passes.h"
#include "mlir/Dialect/Tensor/Transforms/Passes.h"
#include "mlir/Dialect/Tosa/Transforms/Passes.h"
// #include "mlir/Dialect/Transform/Transforms/Passes.h"
// #include "mlir/Dialect/Vector/Transforms/Passes.h"
#include "mlir/Transforms/Passes.h"
#include <mlir/InitAllDialects.h>
#include <mlir/InitAllExtensions.h>
#include <mlir/InitAllPasses.h>

#include "mlir/Parser/Parser.h"

#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/Target/LLVMIR/Dialect/All.h"
// #include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
// #include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
// #include "mlir/Target/LLVMIR/Dialect/OpenMP/OpenMPToLLVMIRTranslation.h"

#include <llvm/Support/raw_sha1_ostream.h>

#include <imex/Dialect/Dist/Utils/Utils.h>
#include <imex/Dialect/PTensor/IR/PTensorOps.h>
#include <imex/InitIMEXDialects.h>
#include <imex/InitIMEXPasses.h>

#include <cstdlib>
#include <iostream>
#include <string>

// #include "llvm/ADT/StringRef.h"
// #include "llvm/IR/Module.h"
// #include "llvm/Support/CommandLine.h"
// #include "llvm/Support/ErrorOr.h"
// #include "llvm/Support/MemoryBuffer.h"
// #include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
// #include "llvm/Support/raw_ostream.h"

#include "ddptensor/itac.hpp"

namespace DDPT {
namespace jit {

static ::mlir::Type makeSignlessType(::mlir::Type type) {
  if (auto shaped = type.dyn_cast<::mlir::ShapedType>()) {
    auto origElemType = shaped.getElementType();
    auto signlessElemType = makeSignlessType(origElemType);
    return shaped.clone(signlessElemType);
  } else if (auto intType = type.dyn_cast<::mlir::IntegerType>()) {
    if (!intType.isSignless())
      return ::mlir::IntegerType::get(intType.getContext(), intType.getWidth());
  }
  return type;
}

::mlir::SmallVector<::mlir::Attribute> mkEnvs(::mlir::Builder &builder,
                                              int64_t rank,
                                              const std::string &device,
                                              uint64_t team) {
  ::mlir::SmallVector<::mlir::Attribute> envs;
  if (team) {
    envs.emplace_back(
        ::imex::dist::DistEnvAttr::get(builder.getI64IntegerAttr(team), rank));
  }
  if (!device.empty()) {
    envs.emplace_back(
        ::imex::region::GPUEnvAttr::get(builder.getStringAttr(device)));
  }
  return envs;
}

// convert ddpt's DTYpeId into MLIR type
static ::mlir::Type getTType(::mlir::OpBuilder &builder, DTypeId dtype,
                             const ::mlir::SmallVector<int64_t> &gShape,
                             const ::mlir::SmallVector<int64_t> &lhShape,
                             const ::mlir::SmallVector<int64_t> &ownShape,
                             const ::mlir::SmallVector<int64_t> &rhShape,
                             const std::string &device, uint64_t team,
                             const uint64_t *lOffs) {
  ::mlir::Type etyp;

  switch (dtype) {
  case FLOAT64:
    etyp = builder.getF64Type();
    break;
  case FLOAT32:
    etyp = builder.getF32Type();
    break;
  case INT64:
  case UINT64:
    etyp = builder.getI64Type();
    break;
  case INT32:
  case UINT32:
    etyp = builder.getI32Type();
    break;
  case INT16:
  case UINT16:
    etyp = builder.getIntegerType(16);
    break;
  case INT8:
  case UINT8:
    etyp = builder.getI8Type();
    break;
  case BOOL:
    etyp = builder.getI1Type();
    break;
  default:
    throw std::runtime_error("unknown dtype");
  };

  auto rank = gShape.size();
  auto envs = mkEnvs(builder, rank, device, 0);
  if (team) {
    if (rank) {
      envs.emplace_back(::imex::dist::DistEnvAttr::get(
          builder.getI64IntegerAttr(team),
          ::llvm::ArrayRef<int64_t>(reinterpret_cast<const int64_t *>(lOffs),
                                    rank),
          {lhShape, ownShape, rhShape}));
      return ::imex::ptensor::PTensorType::get(gShape, etyp, envs);
    } else {
      envs.emplace_back(
          ::imex::dist::DistEnvAttr::get(builder.getI64IntegerAttr(team), 0));
      return ::imex::ptensor::PTensorType::get({}, etyp, envs);
    }
  } else {
    return ::imex::ptensor::PTensorType::get(ownShape, etyp, envs);
  }
}

::mlir::Value DepManager::getDependent(::mlir::OpBuilder &builder,
                                       id_type guid) {
  auto loc = builder.getUnknownLoc();
  if (auto d = _ivm.find(guid); d == _ivm.end()) {
    // Not found -> this must be an input argument to the jit function
    auto idx = _args.size();
    auto fut = Registry::get(guid);
    auto impl = std::dynamic_pointer_cast<DDPTensorImpl>(fut.get());
    auto rank = impl->ndims();
    ::mlir::SmallVector<int64_t> lhShape(rank), ownShape(rank), rhShape(rank);
    for (size_t i = 0; i < rank; i++) {
      lhShape[i] = impl->lh_shape() ? impl->lh_shape()[i] : 0;
      ownShape[i] = impl->local_shape()[i];
      rhShape[i] = impl->rh_shape() ? impl->rh_shape()[i] : 0;
    }
    auto typ = getTType(
        builder, impl->dtype(),
        ::mlir::SmallVector<int64_t>(impl->shape(), impl->shape() + rank),
        lhShape, ownShape, rhShape, fut.device(), fut.team(),
        impl->local_offsets());
    _func.insertArgument(idx, typ, {}, loc);
    auto val = _func.getArgument(idx);
    _args.push_back({guid, std::move(fut)});
    _ivm[guid] = val;
    return val;
  } else {
    return d->second;
  }
}

std::vector<void *> DepManager::store_inputs() {
  std::vector<void *> res;
  for (auto a : _args) {
    a.second.get().get()->add_to_args(res);
    _ivm.erase(a.first); // inputs need no delivery
    _icm.erase(a.first);
  }
  return res;
}

void DepManager::addVal(id_type guid, ::mlir::Value val, SetResFunc cb) {
  assert(_ivm.find(guid) == _ivm.end());
  _ivm[guid] = val;
  _icm[guid] = cb;
}

void DepManager::addReady(id_type guid, ReadyFunc cb) {
  _icr[guid].emplace_back(cb);
}

void DepManager::drop(id_type guid) {
  _ivm.erase(guid);
  _icm.erase(guid);
  _icr.erase(guid);
  Registry::del(guid);
  // FIXME create delete op
}

// Now we have to define the return type as a ValueRange of all arrays which we
// have created (runnables have put them into DepManager when generating mlir)
// We also compute the total size of the struct llvm created for this return
// type llvm will basically return a struct with all the arrays as members, each
// of type JIT::MemRefDescriptor
uint64_t DepManager::handleResult(::mlir::OpBuilder &builder) {
  // Need a container to put all return values, will be used to construct
  // TypeRange
  std::vector<::mlir::Value> ret_values(_ivm.size());

  // remove default result
  //_func.eraseResult(0);

  // here we store the total size of the llvm struct
  auto loc = builder.getUnknownLoc();
  uint64_t sz = 0;
  unsigned idx = 0;
  for (auto &v : _ivm) {
    ::mlir::Value value = v.second;
    bool isDist = ::imex::dist::isDist(value.getType());
    ret_values[idx] = value;
    _func.insertResult(idx, value.getType(), {});
    auto rank = value.getType().cast<::imex::ptensor::PTensorType>().getRank();
    _irm[v.first] = std::make_pair(rank, isDist);
    // add sizes of tensor
    sz += ptensor_sz(rank, isDist);
    // clear reference to MLIR value
    v.second = nullptr;
    ++idx;
  }

  if (HAS_ITAC()) {
    int vtExeSym, vtDDPTClass;
    VT(VT_classdef, "ddpt", &vtDDPTClass);
    VT(VT_funcdef, "execute", vtDDPTClass, &vtExeSym);
    auto s = builder.create<::mlir::arith::ConstantOp>(
        loc, builder.getI32IntegerAttr(vtExeSym));
    auto end = builder.create<::mlir::func::CallOp>(
        builder.getUnknownLoc(), "VT_end",
        ::mlir::TypeRange(builder.getIntegerType(32)), ::mlir::ValueRange(s));
    mlir::OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToStart(end->getBlock());
    (void)builder.create<::mlir::func::CallOp>(
        builder.getUnknownLoc(), "VT_begin",
        ::mlir::TypeRange(builder.getIntegerType(32)), ::mlir::ValueRange(s));
  }

  // add return statement
  auto ret_value = builder.create<::mlir::func::ReturnOp>(
      builder.getUnknownLoc(), ret_values);

  // _ivm defines the order of return values -> do not clear

  return 2 * sz;
}

void DepManager::deliver(std::vector<intptr_t> &outputV, uint64_t sz) {
  auto output = outputV.data();
  size_t pos = 0;

  auto getMR = [](int rank, auto buff, void *&allocated, void *&aligned,
                  intptr_t &offset, intptr_t *&sizes, intptr_t *&strides) {
    allocated = reinterpret_cast<void *>(buff[0]);
    aligned = reinterpret_cast<void *>(buff[1]);
    offset = buff[2];
    sizes = &buff[3];
    strides = &buff[3 + rank];
    return memref_sz(rank);
  };

  // _ivm defines the order of return values
  for (auto &r : _ivm) {
    auto guid = r.first;
    if (auto v = _icm.find(guid); v != _icm.end()) {
      assert(v->first == guid);
      auto tmp = _irm[guid];
      auto rank = tmp.first;
      auto isDist = tmp.second;
      void *t_allocated[3];
      void *t_aligned[3];
      intptr_t t_offset[3];
      intptr_t *t_sizes[3];
      intptr_t *t_strides[3];

      if (rank > 0 && isDist) {
        for (auto t = 0; t < 3; ++t) {
          pos += getMR(rank, &output[pos], t_allocated[t], t_aligned[t],
                       t_offset[t], t_sizes[t], t_strides[t]);
        }
        // lastly extract local offsets
        uint64_t *lo_allocated = reinterpret_cast<uint64_t *>(output[pos]);
        uint64_t *lo_aligned = reinterpret_cast<uint64_t *>(output[pos + 1]);
        intptr_t lo_offset = output[pos + 2];
        // no sizes/stride needed, just skip
        pos += memref_sz(1);
        // call finalization callback
        v->second(
            rank, t_allocated[0], t_aligned[0], t_offset[0], t_sizes[0],
            t_strides[0], // lhsHalo
            t_allocated[1], t_aligned[1], t_offset[1], t_sizes[1],
            t_strides[1], // lData
            t_allocated[2], t_aligned[2], t_offset[2], t_sizes[2],
            t_strides[2], // rhsHalo
            lo_allocated,
            lo_aligned + lo_offset // local offset is 1d tensor of uint64_t
        );
      } else { // 0d tensor or non-dist
        pos += getMR(rank, &output[pos], t_allocated[1], t_aligned[1],
                     t_offset[1], t_sizes[1], t_strides[1]);
        v->second(rank, nullptr, nullptr, 0, nullptr, nullptr, // lhsHalo
                  t_allocated[1], t_aligned[1], t_offset[1], t_sizes[1],
                  t_strides[1],                          // lData
                  nullptr, nullptr, 0, nullptr, nullptr, // lhsHalo
                  nullptr, nullptr);
      }
    } else {
      assert(false);
    }
  }

  // ready signals will always be sent, at this point they are not linked to a
  // return value
  for (auto &readyV : _icr) {
    for (auto cb : readyV.second) {
      cb(readyV.first);
    }
  }
}

std::vector<intptr_t> JIT::run(::mlir::ModuleOp &module,
                               const std::string &fname,
                               std::vector<void *> &inp, size_t osz) {

  int vtDDPTClass, vtHashSym, vtEEngineSym, vtRunSym, vtHashGenSym;
  if (HAS_ITAC()) {
    VT(VT_classdef, "ddpt", &vtDDPTClass);
    VT(VT_funcdef, "lookup_cache", vtDDPTClass, &vtHashSym);
    VT(VT_funcdef, "gen_sha", vtDDPTClass, &vtHashGenSym);
    VT(VT_funcdef, "eengine", vtDDPTClass, &vtEEngineSym);
    VT(VT_funcdef, "run", vtDDPTClass, &vtRunSym);
    VT(VT_begin, vtEEngineSym);

    ::mlir::OpBuilder builder(module->getContext());
    ::mlir::OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPoint(module.getBody(),
                              std::prev(module.getBody()->end()));
    auto intTyp = builder.getIntegerType(32);
    auto funcType = builder.getFunctionType({intTyp}, {intTyp});
    builder.create<::mlir::func::FuncOp>(module.getLoc(), "VT_begin", funcType)
        .setPrivate();
    builder.create<::mlir::func::FuncOp>(module.getLoc(), "VT_end", funcType)
        .setPrivate();
  }

  ::mlir::ExecutionEngine *enginePtr;
  std::unique_ptr<::mlir::ExecutionEngine> tmpEngine;

  if (_useCache) {
    VT(VT_begin, vtHashGenSym);
    static std::map<std::array<unsigned char, 20>,
                    std::unique_ptr<::mlir::ExecutionEngine>>
        engineCache;

    llvm::raw_sha1_ostream shaOS;
    module->print(shaOS);
    auto cksm = shaOS.sha1();
    VT(VT_end, vtHashGenSym);

    VT(VT_begin, vtHashSym);
    if (auto search = engineCache.find(cksm); search == engineCache.end()) {
      engineCache[cksm] = createExecutionEngine(module);
    } else {
      if (_verbose)
        std::cerr << "cached..." << std::endl;
    }
    enginePtr = engineCache[cksm].get();
    VT(VT_end, vtHashSym);
  } else {
    VT(VT_begin, vtHashSym);
    tmpEngine = createExecutionEngine(module);
    enginePtr = tmpEngine.get();
    VT(VT_end, vtHashSym);
  }

  auto expectedFPtr =
      enginePtr->lookupPacked(std::string("_mlir_ciface_") + fname);
  if (auto err = expectedFPtr.takeError()) {
    ::llvm::errs() << "JIT invocation failed: " << toString(std::move(err))
                   << "\n";
    throw std::runtime_error("JIT invocation failed");
  }
  auto jittedFuncPtr = *expectedFPtr;

  // pack function arguments
  llvm::SmallVector<void *> args;
  std::vector<intptr_t> out(osz);
  auto tmp = out.data();
  // first arg must be the result ptr
  if (osz) {
    args.push_back(&tmp);
  }
  // we need a void*& for every input tensor
  // we refer directly to the storage in inp
  for (auto &arg : inp) {
    args.push_back(&arg);
  }

  // call function
  (*jittedFuncPtr)(args.data());

  VT(VT_end, vtEEngineSym);
  return out;
}

std::unique_ptr<::mlir::ExecutionEngine>
JIT::createExecutionEngine(::mlir::ModuleOp &module) {
  if (_verbose)
    std::cerr << "compiling..." << std::endl;
  if (_verbose > 1)
    module.dump();

  // Create an ::mlir execution engine. The execution engine eagerly
  // JIT-compiles the module.
  ::mlir::ExecutionEngineOptions opts;
  opts.transformer = _optPipeline;
  opts.jitCodeGenOptLevel = llvm::CodeGenOpt::getLevel(_jit_opt_level);
  opts.sharedLibPaths = _sharedLibPaths;
  opts.enableObjectDump = true;

  // lower to LLVM
  if (::mlir::failed(_pm.run(module)))
    throw std::runtime_error("failed to run pass manager");

  if (_verbose > 2)
    module.dump();

  auto maybeEngine = ::mlir::ExecutionEngine::create(module, opts);
  assert(maybeEngine && "failed to construct an execution engine");
  return std::move(maybeEngine.get());
}

static const char *cpu_pipeline = "ptensor-dist,"
                                  "func.func(dist-coalesce),"
                                  "func.func(dist-infer-elementwise-cores),"
                                  "convert-dist-to-standard,"
                                  "canonicalize,"
                                  "overlap-comm-and-compute,"
                                  "add-comm-cache-keys,"
                                  "lower-distruntime-to-idtr,"
                                  "convert-ptensor-to-linalg,"
                                  "canonicalize,"
                                  "func.func(tosa-to-linalg),"
                                  "func.func(tosa-to-tensor),"
                                  "canonicalize,"
                                  "linalg-fuse-elementwise-ops,"
                                  // "convert-shape-to-std,"
                                  "arith-expand,"
                                  "memref-expand,"
                                  "arith-bufferize,"
                                  // "func-bufferize,"
                                  "func.func(empty-tensor-to-alloc-tensor),"
                                  "func.func(scf-bufferize),"
                                  "func.func(tensor-bufferize),"
                                  "func.func(bufferization-bufferize),"
                                  "func.func(linalg-bufferize),"
                                  "func.func(linalg-detensorize),"
                                  "func.func(tensor-bufferize),"
                                  "func.func(finalizing-bufferize),"
                                  "func.func(buffer-deallocation),"
                                  "imex-remove-temporaries,"
                                  "func.func(convert-linalg-to-parallel-loops),"
                                  "func.func(scf-parallel-loop-fusion),"
                                  "canonicalize,"
                                  "fold-memref-alias-ops,"
                                  "expand-strided-metadata,"
                                  "convert-math-to-funcs,"
                                  "lower-affine,"
                                  "convert-scf-to-cf,"
                                  "finalize-memref-to-llvm,"
                                  "convert-math-to-llvm,"
                                  "convert-math-to-libm,"
                                  "convert-func-to-llvm,"
                                  "reconcile-unrealized-casts";

static const char *gpu_pipeline =
    "ptensor-dist,"
    "func.func(dist-coalesce),"
    "func.func(dist-infer-elementwise-cores),"
    "convert-dist-to-standard,"
    "canonicalize,"
    "overlap-comm-and-compute,"
    "add-comm-cache-keys,"
    "lower-distruntime-to-idtr,"
    "convert-ptensor-to-linalg,"
    "canonicalize,"
    "func.func(tosa-make-broadcastable),"
    "func.func(tosa-to-linalg),"
    "func.func(tosa-to-tensor),"
    "canonicalize,"
    "linalg-fuse-elementwise-ops,"
    "arith-expand,"
    "memref-expand,"
    "arith-bufferize,"
    "func-bufferize,"
    "func.func(empty-tensor-to-alloc-tensor),"
    "func.func(scf-bufferize),"
    "func.func(tensor-bufferize),"
    "func.func(bufferization-bufferize),"
    "func.func(linalg-bufferize),"
    "func.func(linalg-detensorize),"
    "func.func(tensor-bufferize),"
    "func.func(finalizing-bufferize),"
    "imex-remove-temporaries,"
    "func.func(convert-linalg-to-parallel-loops),"
    "func.func(scf-parallel-loop-fusion),"
    // GPU
    "func.func(imex-add-outer-parallel-loop),"
    "func.func(gpu-map-parallel-loops),"
    "func.func(convert-parallel-loops-to-gpu),"
    // insert-gpu-allocs pass can have client-api = opencl or vulkan args
    "func.func(insert-gpu-allocs{client-api=opencl}),"
    "canonicalize,"
    "normalize-memrefs,"
    // Unstride memrefs does not seem to be needed.
    //  "func.func(unstride-memrefs),"
    "func.func(lower-affine),"
    "gpu-kernel-outlining,"
    "canonicalize,"
    "cse,"
    // The following set-spirv-* passes can have client-api = opencl or vulkan
    // args
    "set-spirv-capabilities{client-api=opencl},"
    "gpu.module(set-spirv-abi-attrs{client-api=opencl}),"
    "canonicalize,"
    "fold-memref-alias-ops,"
    "imex-convert-gpu-to-spirv,"
    "spirv.module(spirv-lower-abi-attrs),"
    "spirv.module(spirv-update-vce),"
    // "func.func(llvm-request-c-wrappers),"
    "serialize-spirv,"
    "expand-strided-metadata,"
    "lower-affine,"
    "convert-gpu-to-gpux,"
    "convert-func-to-llvm,"
    "convert-math-to-llvm,"
    "convert-gpux-to-llvm,"
    "finalize-memref-to-llvm,"
    "reconcile-unrealized-casts";

static const char *pass_pipeline =
    getenv("DDPT_PASSES")
        ? getenv("DDPT_PASSES")
        : (getenv("DDPT_USE_GPU") ? gpu_pipeline : cpu_pipeline);

JIT::JIT()
    : _context(::mlir::MLIRContext::Threading::DISABLED), _pm(&_context),
      _verbose(0), _jit_opt_level(3) {
  // Register the translation from ::mlir to LLVM IR, which must happen before
  // we can JIT-compile.
  ::mlir::DialectRegistry registry;
  ::mlir::registerAllDialects(registry);
  ::mlir::registerAllExtensions(registry);
  ::imex::registerAllDialects(registry);
  ::mlir::registerAllToLLVMIRTranslations(registry);
  _context.appendDialectRegistry(registry);

  // load the dialects we use
  _context.getOrLoadDialect<::imex::ptensor::PTensorDialect>();
  _context.getOrLoadDialect<::imex::dist::DistDialect>();
  _context.getOrLoadDialect<::imex::distruntime::DistRuntimeDialect>();
  _context.getOrLoadDialect<::mlir::arith::ArithDialect>();
  _context.getOrLoadDialect<::mlir::func::FuncDialect>();
  _context.getOrLoadDialect<::mlir::linalg::LinalgDialect>();
  // create the pass pipeline from string
  if (::mlir::failed(::mlir::parsePassPipeline(pass_pipeline, _pm)))
    throw std::runtime_error("failed to parse pass pipeline");

  const char *v_ = getenv("DDPT_VERBOSE");
  if (v_) {
    _verbose = std::stoi(v_);
  }
  // some verbosity
  if (_verbose) {
    std::cerr << "DDPT_PASSES=\"" << pass_pipeline << "\"" << std::endl;
    // _pm.enableStatistics();
    if (_verbose > 2)
      _pm.enableTiming();
    // if(_verbose > 1)
    //   _pm.dump();
    if (_verbose > 3)
      _pm.enableIRPrinting();
  }

  const char *envptr = getenv("DDPT_USE_CACHE");
  envptr = envptr ? envptr : "1";
  {
    auto c = std::string(envptr);
    _useCache = c == "1" || c == "y" || c == "Y" || c == "on" || c == "ON";
    std::cerr << "enableObjectDump=" << _useCache << std::endl;
  }
  const char *ol_ = getenv("DDPT_OPT_LEVEL");
  if (ol_) {
    _jit_opt_level = std::stoi(ol_);
    if (_jit_opt_level < 0 || _jit_opt_level > 3) {
      throw std::runtime_error(std::string("Bad optimization level: ") + ol_);
    }
  }

  const char *mlirRoot = getenv("MLIRROOT");
  mlirRoot = mlirRoot ? mlirRoot : CMAKE_MLIR_ROOT;
  _crunnerlib = std::string(mlirRoot) + "/lib/libmlir_c_runner_utils.so";
  _runnerlib = std::string(mlirRoot) + "/lib/libmlir_runner_utils.so";

  const char *idtrlib = getenv("DDPT_IDTR_SO");
  idtrlib = idtrlib ? idtrlib : "libidtr.so";

  auto useGPU = getenv("DDPT_USE_GPU");
  if (useGPU) {
    const char *gpuxlibstr = getenv("DDPT_GPUX_SO");
    if (gpuxlibstr) {
      _gpulib = std::string(gpuxlibstr);
    } else {
      const char *imexRoot = getenv("IMEXROOT");
      imexRoot = imexRoot ? imexRoot : CMAKE_IMEX_ROOT;
      _gpulib = std::string(imexRoot) + "/lib/liblevel-zero-runtime.so";
    }
    _sharedLibPaths = {_crunnerlib.c_str(), _runnerlib.c_str(), idtrlib,
                       _gpulib.c_str()};
  } else {
    _sharedLibPaths = {_crunnerlib.c_str(), _runnerlib.c_str(), idtrlib};
  }

  // detect target architecture
  auto tmBuilderOrError = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!tmBuilderOrError) {
    throw std::runtime_error(
        "Failed to create a JITTargetMachineBuilder for the host\n");
  }

  // build TargetMachine
  auto tmOrError = tmBuilderOrError->createTargetMachine();
  if (!tmOrError) {
    throw std::runtime_error("Failed to create a TargetMachine for the host\n");
  }
  _tm = std::move(tmOrError.get());

  // build optimizing pipeline
  _optPipeline = ::mlir::makeOptimizingTransformer(
      /*optLevel=*/_jit_opt_level,
      /*sizeLevel=*/0,
      /*targetMachine=*/_tm.get());
}

// register dialects and passes
void init() {
  assert(sizeof(intptr_t) == sizeof(void *));
  assert(sizeof(intptr_t) == sizeof(uint64_t));

  ::mlir::registerAllPasses();
  ::imex::registerAllPasses();

  // Initialize LLVM targets.
  // llvm::InitLLVM y(0, nullptr);
  ::llvm::InitializeNativeTarget();
  ::llvm::InitializeNativeTargetAsmPrinter();
  ::llvm::InitializeNativeTargetAsmParser();
}
} // namespace jit
} // namespace DDPT
