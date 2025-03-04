#   Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

import unittest
import numpy as np
from paddle.fluid.tests.unittests.op_test import OpTest, convert_float_to_uint16
import paddle
import paddle.fluid.core as core
import paddle.nn.functional as F

np.random.seed(10)
paddle.enable_static()


def ref_log_softmax(x):
    shiftx = x - np.max(x)
    out = shiftx - np.log(np.exp(shiftx).sum())
    return out


def ref_log_softmax_grad(x, axis):
    if axis < 0:
        axis += len(x.shape)
    out = np.apply_along_axis(ref_log_softmax, axis, x)
    axis_dim = x.shape[axis]
    dout = np.full_like(x, fill_value=1.0 / x.size)
    dx = dout - np.exp(out) * dout.copy().sum(axis=axis, keepdims=True).repeat(
        axis_dim, axis=axis
    )
    return dx


class TestLogSoftmaxOp(OpTest):
    def setUp(self):
        self.op_type = 'log_softmax'
        self.set_mlu()
        self.python_api = F.log_softmax
        self.dtype = 'float32'
        self.shape = [2, 3, 4, 5]
        self.axis = -1
        self.set_attrs()

        x = np.random.uniform(0.1, 1.0, self.shape).astype(self.dtype)
        out = np.apply_along_axis(ref_log_softmax, self.axis, x)
        self.x_grad = ref_log_softmax_grad(x, self.axis)

        self.inputs = {'X': x}
        self.outputs = {'Out': out}
        self.attrs = {'axis': self.axis}

    def set_attrs(self):
        pass

    def set_mlu(self):
        self.__class__.use_mlu = True
        self.place = paddle.device.MLUPlace(0)

    def test_check_output(self):
        self.check_output_with_place(self.place)

    def test_check_grad(self):
        self.check_grad_with_place(
            self.place, ['X'], ['Out'], user_defined_grads=[self.x_grad]
        )


class TestLogSoftmaxShape(TestLogSoftmaxOp):
    def set_attrs(self):
        self.shape = [12, 10]


class TestLogSoftmaxAxis(TestLogSoftmaxOp):
    def set_attrs(self):
        self.axis = 1


class TestLogSoftmaxOpFp16(OpTest):
    def setUp(self):
        self.op_type = 'log_softmax'
        self.set_mlu()
        self.python_api = F.log_softmax
        self.dtype = 'float16'
        self.shape = [2, 3, 4, 5]
        self.axis = -1
        self.set_attrs()

        x = np.random.uniform(0.1, 1.0, self.shape).astype(self.dtype)
        out = np.apply_along_axis(ref_log_softmax, self.axis, x)
        self.x_grad = ref_log_softmax_grad(x, self.axis)

        self.inputs = {'X': x}
        self.outputs = {'Out': out}
        self.attrs = {'axis': self.axis}

    def set_attrs(self):
        pass

    def set_mlu(self):
        self.__class__.use_mlu = True
        self.place = paddle.device.MLUPlace(0)

    def test_check_output(self):
        self.check_output_with_place(self.place, atol=1e-2)

    def test_check_grad(self):
        self.check_grad_with_place(
            self.place,
            ['X'],
            ['Out'],
            user_defined_grads=[self.x_grad],
            max_relative_error=0.015,
        )


class TestNNLogSoftmaxAPI(unittest.TestCase):
    def setUp(self):
        self.set_mlu()
        self.x_shape = [2, 3, 4, 5]
        self.x = np.random.uniform(-1.0, 1.0, self.x_shape).astype(np.float32)

    def set_mlu(self):
        self.__class__.use_mlu = True
        self.place = paddle.device.MLUPlace(0)

    def check_api(self, axis=-1):
        ref_out = np.apply_along_axis(ref_log_softmax, axis, self.x)

        logsoftmax = paddle.nn.LogSoftmax(axis)
        paddle.enable_static()
        # test static api
        with paddle.static.program_guard(paddle.static.Program()):
            x = paddle.static.data(name='x', shape=self.x_shape)
            y = logsoftmax(x)
            exe = paddle.static.Executor(self.place)
            out = exe.run(feed={'x': self.x}, fetch_list=[y])
        np.testing.assert_allclose(out[0], ref_out, rtol=1e-6)

        # test dygrapg api
        paddle.disable_static()
        x = paddle.to_tensor(self.x)
        y = logsoftmax(x)
        np.testing.assert_allclose(y.numpy(), ref_out, rtol=1e-6)
        paddle.enable_static()

    def test_check_api(self):
        for axis in [-1, 1]:
            self.check_api(axis)


class TestNNFunctionalLogSoftmaxAPI(unittest.TestCase):
    def setUp(self):
        self.set_mlu()
        self.x_shape = [2, 3, 4, 5]
        self.x = np.random.uniform(-1, 1, self.x_shape).astype(np.float32)

    def set_mlu(self):
        self.__class__.use_mlu = True
        self.place = paddle.device.MLUPlace(0)

    def check_api(self, axis=-1, dtype=None):
        x = self.x.copy()
        if dtype is not None:
            x = x.astype(dtype)
        ref_out = np.apply_along_axis(ref_log_softmax, axis, x)
        with paddle.static.program_guard(paddle.static.Program()):
            x = paddle.static.data(name='x', shape=self.x_shape)
            y = F.log_softmax(x, axis, dtype)
            exe = paddle.static.Executor(self.place)
            out = exe.run(feed={'x': self.x}, fetch_list=[y])
        np.testing.assert_allclose(out[0], ref_out, rtol=1e-6)

        paddle.disable_static()
        x = paddle.to_tensor(self.x)
        y = F.log_softmax(x, axis, dtype)
        np.testing.assert_allclose(y.numpy(), ref_out, rtol=1e-6)
        paddle.enable_static()

    def test_check_api(self):
        for axis in [-1, 1]:
            self.check_api(axis)
        self.check_api(-1, 'float32')

    def test_errors(self):
        paddle.enable_static()
        with paddle.static.program_guard(paddle.static.Program()):
            x = paddle.static.data(name='X1', shape=[100], dtype='int32')
            self.assertRaises(TypeError, F.log_softmax, x)

            x = paddle.static.data(name='X2', shape=[100], dtype='float32')
            self.assertRaises(TypeError, F.log_softmax, x, dtype='int32')
        paddle.disable_static()


if __name__ == "__main__":
    unittest.main()
