#include "client/mnm_computation_client.h"

#include <fstream>
#include <iostream>

#include "torch_mnm/csrc/compiler/utils.h"
#include "torch_mnm/csrc/compiler/mnm_lowering_context.h"
#include "torch_mnm/csrc/value_ext/value.h"
#include "torch_mnm/csrc/pass_ext/pass.h"
#include "env_vars.h"

#include "lazy_tensors/computation_client/nnc_computation_client.h"
#include "lazy_tensor_core/csrc/device.h"

#include "tvm/node/serialization.h"
#include "mnm/device.h"
#include "mnm/pass_manager.h"
#include "mnm/serialization.h"
#include "mnm/vm/vm.h"
#include "mnm/vm/value.h"
#include "meta/src/common/shape_utils.h"
#include "meta/src/impl/vm/compiler.h"
#include "meta/src/op/ty/utils.h"

namespace torch_mnm {

using namespace torch_lazy_tensors::compiler;
using namespace torch_lazy_tensors::compiler::mnm_backend;
using namespace mnm::value;

void MNMComputationClient::MNMData::Assign(const Data& data) {
  const MNMData& mnm_data = dynamic_cast<const MNMData&>(data);
  if (&mnm_data != this) {
    handle = mnm_data.handle;
  }
}

MNMComputationClient::MNMComputationClient(Options options) : BaseComputationClient(options) {
}

void PopulateLocalDevices(torch_mnm::MNMComputationClient::Options* options) {
  auto dev_kind = sys_util::GetEnvString(torch_mnm::env::kEnvDefaultDevice, "CPU");
  int dev_id = 0;  // TODO: Determine the device ID using local rank.
  bool ignore = true;

  // Iterate candidate devices in the preferred order, and include all devices the
  // lower or equal ordinal of the user specified default device.
  for (auto kind : {"GPU", "CPU"}) {
    std::string ltc_device = dev_kind + ":" + std::to_string(dev_id);
    if (kind == dev_kind) {
      options->default_device = ltc_device;
      ignore = false;
    }
    if (!ignore) {
      options->devices.insert(ltc_device);
      options->global_device_map[ltc_device] = torch_mnm::ToMNMDevice(ltc_device).c_str();
    }
  }
}

std::unique_ptr<ComputationClient> MNMComputationClient::Create() {
  Options options;
  PopulateLocalDevices(&options);
  return std::make_unique<MNMComputationClient>(options);
}

template <typename NativeT>
void PopulateRn(lazy_tensors::Literal& literal, lazy_tensors::Span<const NativeT> values) {
  LTC_CHECK(literal.shape().IsArray());
  LTC_CHECK_EQ(ShapeUtil::ElementsIn(literal.shape()), values.size());
  LTC_CHECK_EQ(literal.shape().element_type(), primitive_util::NativeToPrimitiveType<NativeT>());
  auto data_span = literal.data<NativeT>();
  std::copy(values.begin(), values.end(), data_span.begin());
}

void PopulateRn(lazy_tensors::Literal& literal, void* buf) {
  using namespace lazy_tensors;
  switch (literal.shape().element_type()) {
    case PrimitiveType::S8:
      return PopulateRn(
          literal, Span<const int8>(reinterpret_cast<const int8*>(buf), literal.value().numel()));
    case PrimitiveType::S32:
      return PopulateRn(
          literal, Span<const int32>(reinterpret_cast<const int32*>(buf), literal.value().numel()));
    case PrimitiveType::S64:
      return PopulateRn(
          literal, Span<const int64>(reinterpret_cast<const int64*>(buf), literal.value().numel()));
    case PrimitiveType::PRED:
      return PopulateRn(
          literal, Span<const bool>(reinterpret_cast<const bool*>(buf), literal.value().numel()));
    case PrimitiveType::U8:
      return PopulateRn(literal,
                        Span<const uint8>(reinterpret_cast<const lazy_tensors::uint8*>(buf),
                                          literal.value().numel()));
    case PrimitiveType::U32:
      return PopulateRn(literal,
                        Span<const uint32>(reinterpret_cast<const lazy_tensors::uint32*>(buf),
                                           literal.value().numel()));
    case PrimitiveType::U64:
      return PopulateRn(literal,
                        Span<const uint64>(reinterpret_cast<const lazy_tensors::uint64*>(buf),
                                           literal.value().numel()));
    case PrimitiveType::F16:
      return PopulateRn(literal, Span<const half>(reinterpret_cast<const lazy_tensors::half*>(buf),
                                                  literal.value().numel()));
    case PrimitiveType::F32:
      return PopulateRn(
          literal, Span<const float>(reinterpret_cast<const float*>(buf), literal.value().numel()));
    case PrimitiveType::F64:
      return PopulateRn(literal, Span<const double>(reinterpret_cast<const double*>(buf),
                                                    literal.value().numel()));
    default:
      LTC_LOG(FATAL) << "NotImplementedError: " << literal.shape().element_type();
  }
}

ComputationClient::DataPtr MNMComputationClient::CreateDataPlaceholder(std::string device,
                                                                       Shape shape) {
  return std::make_shared<MNMData>(std::move(device), shape);
}

std::vector<ComputationClient::DataPtr> MNMComputationClient::TransferToServerInternal(
    lazy_tensors::Span<const TensorSource> tensors) {
  std::vector<mnm::value::TensorValue> tvs(tensors.size());
  std::vector<ComputationClient::DataPtr> result;
  for (const auto& ts : tensors) {
    mnm::DType dtype;
    std::vector<int64_t> shape;
    mnm::Device dev_cpu(mnm::DevType::kCPU(), 0);
    mnm::Device dev = ToMNMDevice(ts.device);
    std::tie(shape, dtype) = ToMNMShape(ts.shape);
    TensorValue tv_shape = mnm::value::TensorValue::Assemble(dev_cpu, dtype, shape);
    int64_t nbytes = mnm::common::shape_utils::BytesCompactTensor(*(tv_shape.operator DLTensor*()));
    auto buffer_cpu = mnm::memory_pool::Memory::Alloc(dev_cpu, nbytes);
    auto tv_cpu = TensorValue::Assemble(dev_cpu, dtype, shape, {}, buffer_cpu->data, buffer_cpu);
    ts.populate_fn(ts, buffer_cpu->data, nbytes);
    auto tv = TensorValue::make(
        mnm::tensor::Tensor(tv_cpu->tensor.CopyTo(dev)));  // memory of tv is allocated by tvm
    result.push_back(
        std::make_shared<MNMComputationClient::MNMData>(ts.device, Shape(ts.shape), tv));
  }
  return result;
}

std::vector<ComputationClient::DataPtr> MNMComputationClient::TransferToServer(
    lazy_tensors::Span<const TensorSource> tensors) {
  // TODO(@hzfan): parallel transfer
  return TransferToServerInternal(tensors);
}

std::vector<Literal> MNMComputationClient::TransferFromServer(
    lazy_tensors::Span<const DataPtr> handles) {
  std::vector<Literal> results;
  for (const auto& handle : handles) {
    auto* ptr = static_cast<MNMData*>(handle.get());
    DLTensor* val = ptr->handle;
    auto shape = std::vector<int64_t>(val->shape, val->shape + val->ndim);
    Literal res(ToLTCShape(shape, val->dtype));

    // Transfer to CPU if it is on the other device.
    if (val->device.device_type != DevType::kCPU()) {
      mnm::Device dev_cpu(mnm::DevType::kCPU(), 0);
      TensorValue tv_shape = TensorValue::Assemble(dev_cpu, val->dtype, shape);
      int64_t nbytes =
          mnm::common::shape_utils::BytesCompactTensor(*(tv_shape.operator DLTensor*()));
      auto buffer_cpu = memory_pool::Memory::Alloc(dev_cpu, nbytes);
      auto tv_cpu =
          TensorValue::Assemble(dev_cpu, val->dtype, shape, {}, buffer_cpu->data, buffer_cpu);
      tv_cpu->tensor.CopyFrom(val);
      PopulateRn(res, (tv_cpu.operator DLTensor*())->data);
    } else {
      PopulateRn(res, val->data);
    }
    results.push_back(res);
  }
  return results;
}

bool IsIdentityFunction(Function func) {
  if (func->params.size() != 1U) return false;
  if (func->body != func->params[0]) return false;
  return true;
}

std::vector<ComputationClient::ComputationPtr> MNMComputationClient::Compile(
    std::vector<ComputationClient::CompileInstance> instances) {
  std::vector<ComputationPtr> results;
  for (const auto& ins : instances) {
    mnm::executor::vm::VMCompiler compiler;
    auto* computation = static_cast<GenericComputationMNM*>(ins.computation.get());
    Function func = Downcast<Function>(computation->computation());
    IRModule ir_module = IRModule::FromExpr(computation->computation());

    auto device = GetDefaultDevice();
    auto mnm_device = ToMNMDevice(device);

    mnm::pass::MNMSequential seq({
        mnm::pass::InferType(),
        mnm::pass::AssignDevice(mnm_device.device_type().c_str()),
        mnm::pass::InferType(),
        mnm::pass::LambdaLift(),
        mnm::pass::InferType(),
        mnm::pass::InlineClosure(),
        mnm::pass::InferType(),
        mnm::pass::DeadCodeElimination(),
        mnm::pass::InferType(),
        mnm::pass::EliminateClosure(),
        mnm::pass::InferType(),
        mnm::pass::InlineLet(),
        mnm::pass::InferType(),
        mnm::pass::DeadCodeElimination(),
        mnm::pass::InferType(),
        mnm::pass::CanonicalizeOps(),
        mnm::pass::InferType(),
    });
    // std::stringstream ss;
    // ss << "Compile: " << std::endl;
    // ss << ::mnm::ir::AsText(ir_module) << std::endl;
    // ss << "Alias: " << std::endl;
    // for (const auto& kv : computation->alias()) {
    //   ss << "(" << kv.first << ", " << kv.second << "), ";
    // }
    // std::cout << std::endl;
    tvm::runtime::Module exe;
    if (!IsIdentityFunction(func)) {
      mnm::executor::vm::DeviceMap device_map{
          {Integer((int)(mnm_device.device_type())), mnm_device}};
      ir_module = seq(ir_module);
      ir_module = IRModule::FromExpr(ir_module->Lookup("main"));
      ir_module = mnm::pass::InferType()(ir_module);
      compiler.Lower(ir_module, device_map);
      exe = compiler.GetFunction("get_executable", nullptr)();
    }
    results.emplace_back(std::make_shared<MNMComputation>(
        ins.computation, ConsumeValue(ins.computation->GetProgramShape()), ins.devices, exe));
    lifted_computation_[results.back().get()] = ir_module;
  }
  return results;
}

std::vector<ComputationClient::DataPtr> MNMComputationClient::ExecuteComputation(
    const Computation& computation, lazy_tensors::Span<const DataPtr> arguments,
    const std::string& device, const ExecuteComputationOptions& options) {
  std::function<std::vector<ComputationClient::DataPtr>(Value)> explode_tuple =
      [&](Value val) -> std::vector<ComputationClient::DataPtr> {
    if (const auto* tup = val.as<TupleValueObj>()) {
      std::vector<ComputationClient::DataPtr> ret;
      for (const auto& field : tup->fields) {
        std::vector<ComputationClient::DataPtr> tup_ret = explode_tuple(field);
        LTC_CHECK_EQ(tup_ret.size(), 1U);
        ret.push_back(tup_ret[0]);
      }
      return ret;
    } else if (const auto* tensor = val.as<TensorValueObj>()) {
      Shape shape = ToLTCShape(mnm::op::GetType(val));
      return {std::make_shared<MNMData>(device, shape, val)};
    } else if (const auto* closure_val_ext = val.as<ClosureValueExtObj>()) {
      auto func = Downcast<Function>(closure_val_ext->mod->Lookup(closure_val_ext->gvar));
      Shape shape = ToLTCShape(func->checked_type());
      return {std::make_shared<MNMData>(device, shape, val)};
    } else if (val.as<mnm::executor::vm::VMClosureValueObj>()) {
      // the return type of VMClosureValue cannot be inferred from its value solely
      return {std::make_shared<MNMData>(device, Shape(), val)};
    }
    LTC_LOG(FATAL) << "NotImplementedError: " << val->GetTypeKey();
  };
  std::function<Value(Value)> normalize_value = [&](Value val) -> Value {
    if (const auto* vm_closure_val = val.as<mnm::executor::vm::VMClosureValueObj>()) {
      IRModule mod = lifted_computation_.at(&computation);
      const auto& mnm_computation = static_cast<const MNMComputation&>(computation);
      std::string gvar_name;
      bool found = false;
      const auto* executable = mnm_computation.executable.as<mnm::executor::vm::Executable>();
      LTC_CHECK(executable);
      for (const auto& kv : executable->global_map) {
        if (kv.second == vm_closure_val->func_index) {
          gvar_name = kv.first;
          found = true;
          break;
        }
      }
      CHECK(found);
      GlobalVar gvar = mod->GetGlobalVar(gvar_name);
      ir::Map<ir::Var, Value> env;
      auto func = Downcast<Function>(mod->Lookup(gvar_name));
      size_t num_free_vars = func->params.size();
      CHECK_EQ(func->params.size(), vm_closure_val->free_vars.size());
      for (size_t i = 0; i < num_free_vars; ++i) {
        env.Set(func->params[i], vm_closure_val->free_vars[i]);
      }
      return ClosureValueExt::make(env, mod, gvar);
    }
    return val;
  };
  static auto vm_constructor = registry::GetPackedFunc("mnm.vm.VirtualMachine");
  const auto& mnm_computation = static_cast<const MNMComputation&>(computation);
  bool is_identity_function = !mnm_computation.executable.defined();
  std::vector<Value> values;
  Value ret;
  for (const auto& argument : arguments) {
    values.push_back(static_cast<MNMData*>(argument.get())->handle);
  }
  if (!is_identity_function) {
    // TODO(@hzfan): cache the VM
    tvm::runtime::Module vm_module = mnm_computation.executable.defined()
                                         ? vm_constructor(mnm_computation.executable, false)
                                         : tvm::runtime::Module();
    auto* vm = dynamic_cast<mnm::executor::vm::VirtualMachine*>(vm_module.operator->());
    vm_module->GetFunction("set_devices")(ToMNMDevice(GetDefaultDevice()));
    mnm::executor::vm::VMContext vm_ctx = vm->PrepareVMContext("main", values);
    // TODO(@hzfan): sync the execution
    ret = vm->Run(vm_ctx);
  } else {
    LTC_CHECK_EQ(values.size(), 1U);
    ret = values[0];
  }
  ret = normalize_value(ret);
  return explode_tuple(ret);
}

lazy_tensors::ComputationClient* MNMGet() {
  using namespace lazy_tensors;
  static auto mnm_computation_client = MNMComputationClient::Create();
  return mnm_computation_client.get();
}

lazy_tensors::ComputationClient* MNMGetIfInitialized() {
  using namespace lazy_tensors;
  return MNMGet();
}

}  // namespace torch_mnm