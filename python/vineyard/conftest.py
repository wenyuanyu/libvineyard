#! /usr/bin/env python
# -*- coding: utf-8 -*-
#
# Copyright 2020 Alibaba Group Holding Limited.
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
#

import pytest

import vineyard


def pytest_addoption(parser):
    parser.addoption(
        "--vineyard-ipc-socket",
        action="store",
        default='/var/run/vineyard.sock',
        help='Location of vineyard ipc socket',
    )


@pytest.fixture(scope='session')
def vineyard_ipc_socket(request):
    return request.config.option.vineyard_ipc_socket


@pytest.fixture(scope='session')
def vineyard_client(request):
    ipc_socket = request.config.option.vineyard_ipc_socket
    return vineyard.connect(ipc_socket)


pytest_plugins = ["benchmark"]