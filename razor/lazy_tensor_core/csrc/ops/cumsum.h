/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Modifications Copyright (c) Facebook, Inc.
 */

#pragma once

#include <c10/core/ScalarType.h>
#include <c10/util/Optional.h>

#include "lazy_tensor_core/csrc/ir.h"

namespace torch_lazy_tensors {
namespace ir {
namespace ops {

class CumSum : public Node {
 public:
  CumSum(const Value& input, lazy_tensors::int64 dim, c10::optional<at::ScalarType> dtype);

  std::string ToString() const override;

  NodePtr Clone(OpList operands) const override;

  lazy_tensors::int64 dim() const {
    return dim_;
  }

  const c10::optional<at::ScalarType>& dtype() const {
    return dtype_;
  }

 private:
  lazy_tensors::int64 dim_;
  c10::optional<at::ScalarType> dtype_;
};

}  // namespace ops
}  // namespace ir
}  // namespace torch_lazy_tensors