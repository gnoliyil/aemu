#  Copyright (C) 2021 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# -*- coding: utf-8 -*-
import time

from google.protobuf import empty_pb2
from aemu.discovery.emulator_discovery import get_default_emulator
from aemu.proto.emulator_controller_pb2 import DisplayMode
_EMPTY_ = empty_pb2.Empty()

# Create a client
stub = get_default_emulator().get_emulator_controller()
stub.setDisplayMode(DisplayMode(value = 0))
time.sleep(3)
print(stub.getDisplayMode(_EMPTY_))
stub.setDisplayMode(DisplayMode(value = 1))
time.sleep(3)
print(stub.getDisplayMode(_EMPTY_))
stub.setDisplayMode(DisplayMode(value = 2))
time.sleep(3)
print(stub.getDisplayMode(_EMPTY_))
stub.setDisplayMode(DisplayMode(value = 3))
time.sleep(3)
print(stub.getDisplayMode(_EMPTY_))
