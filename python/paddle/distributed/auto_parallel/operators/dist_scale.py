# Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from ..utils import compute_compatible_and_update_dim_mapping
from .common import (
    DistributedOperatorImpl,
    DistributedOperatorImplContainer,
    register_distributed_operator_impl,
    register_distributed_operator_impl_container,
)
from .dist_default import DistributedDefaultImpl0


class DistributedScale(DistributedOperatorImplContainer):
    def __init__(self, op_type):
        super().__init__(op_type)


register_distributed_operator_impl_container(DistributedScale("scale"))
register_distributed_operator_impl_container(DistributedScale("fill_any_like"))
register_distributed_operator_impl_container(DistributedScale("where"))
register_distributed_operator_impl_container(DistributedScale("tanh"))


class DistributedScaleImpl(DistributedOperatorImpl):
    def __init__(self, name):
        super().__init__(name)
        self._forward_implemented = True
        self._backward_implemented = True

    def is_input_compatible(self, dist_op):
        return True

    def is_output_compatible(self, dist_op):
        return True

    def is_auto_compatible(self, dist_op):
        if (not self.is_input_compatible(dist_op)) or (
            not self.is_output_compatible(dist_op)
        ):
            return False

        op_desc = dist_op.serial_op.desc
        op_dist_attr = dist_op.dist_attr
        out_name = op_desc.output('Out')[0]
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)

        in_dims_mappings = []
        for in_name in op_desc.input_arg_names():
            in_dims_mapping = op_dist_attr.get_input_dims_mapping(in_name)
            in_dims_mappings.append(in_dims_mapping)

        for x_dims_mapping in in_dims_mappings:
            if x_dims_mapping != out_dims_mapping:
                return False

        return True

    def update_dims_mapping(self, dist_op):
        changed = False
        op_desc = dist_op.serial_op.desc
        op_dist_attr = dist_op.dist_attr
        x_name = op_desc.input('X')[0]
        out_name = op_desc.output('Out')[0]
        x_dims_mapping = op_dist_attr.get_input_dims_mapping(x_name)
        out_dims_mapping = op_dist_attr.get_output_dims_mapping(out_name)

        for i in range(len(x_dims_mapping)):
            dim_changed = compute_compatible_and_update_dim_mapping(
                [x_dims_mapping, out_dims_mapping], [i, i]
            )
            if dim_changed:
                op_dist_attr.set_input_dims_mapping(x_name, x_dims_mapping)
                op_dist_attr.set_output_dims_mapping(out_name, out_dims_mapping)
                changed = True

        return changed

    @staticmethod
    def forward(ctx, *args, **kwargs):
        DistributedDefaultImpl0.forward(ctx, *args, **kwargs)

    @staticmethod
    def backward(ctx, *args, **kwargs):
        DistributedDefaultImpl0.backward(ctx, *args, **kwargs)


register_distributed_operator_impl("scale", DistributedScaleImpl("scale"))
register_distributed_operator_impl(
    "fill_any_like", DistributedScaleImpl("fill_any_like")
)
register_distributed_operator_impl("where", DistributedScaleImpl("where"))
register_distributed_operator_impl("tanh", DistributedScaleImpl("tanh"))
