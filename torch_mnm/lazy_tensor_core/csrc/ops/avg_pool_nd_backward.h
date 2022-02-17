#pragma once

#include "lazy_tensor_core/csrc/ir.h"

namespace torch_lazy_tensors {
namespace ir {
namespace ops {

class AvgPoolNdBackward : public Node {
 public:
  AvgPoolNdBackward(const Value& grad_output, const Value& input,
                    lazy_tensors::int64 spatial_dim_count,
                    std::vector<lazy_tensors::int64> kernel_size,
                    std::vector<lazy_tensors::int64> stride,
                    std::vector<lazy_tensors::int64> padding, bool ceil_mode,
                    bool count_include_pad);

  NodePtr Clone(OpList operands) const override;

  std::string ToString() const override;

  lazy_tensors::int64 spatial_dim_count() const {
    return spatial_dim_count_;
  }

  const std::vector<lazy_tensors::int64>& kernel_size() const {
    return kernel_size_;
  }

  const std::vector<lazy_tensors::int64>& stride() const {
    return stride_;
  }

  const std::vector<lazy_tensors::int64>& padding() const {
    return padding_;
  }

  bool ceil_mode() const {
    return ceil_mode_;
  }

  bool count_include_pad() const {
    return count_include_pad_;
  }

 private:
  lazy_tensors::int64 spatial_dim_count_;
  // The parameters of the pooling.
  std::vector<lazy_tensors::int64> kernel_size_;
  std::vector<lazy_tensors::int64> stride_;
  std::vector<lazy_tensors::int64> padding_;
  bool ceil_mode_;
  // Whether the counts used to compute the average should include the added
  // padding.
  bool count_include_pad_;
};

}  // namespace ops
}  // namespace ir
}  // namespace torch_lazy_tensors
