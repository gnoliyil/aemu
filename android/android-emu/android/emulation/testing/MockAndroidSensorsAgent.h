// Copyright 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/emulation/control/sensors_agent.h"

#include <gmock/gmock.h>

namespace android {

#define SENSORS_MOCK(MOCK_MACRO, name) \
    MOCK_MACRO(                        \
            name,                      \
            std::remove_pointer<decltype(QAndroidSensorsAgent::name)>::type)

class MockAndroidSensorsAgent {
public:
    static MockAndroidSensorsAgent* mock;

    static const QAndroidSensorsAgent* getAgent();

    // int setPhysicalParameterTarget(int parameterId, const float* val,
    //                                const size_t count,
    //                                int interpolationMethod)
    SENSORS_MOCK(MOCK_METHOD4, setPhysicalParameterTarget);

    // int getPhysicalParameter(int parameterId, float* out, const size_t count,
    //                          int parameterValueType)
    SENSORS_MOCK(MOCK_METHOD4, getPhysicalParameter);

    // int setCoarseOrientation(int orientation)
    SENSORS_MOCK(MOCK_METHOD1, setCoarseOrientation);

    // int setSensorOverride(int sensorId, const float* val, const size_t count)
    SENSORS_MOCK(MOCK_METHOD3, setSensorOverride);

    // int getSensor(int sensorId, float* out, const size_t count)
    SENSORS_MOCK(MOCK_METHOD3, getSensor);

    // int getSensorSize(int sensorId, size_t* size)
    SENSORS_MOCK(MOCK_METHOD2, getSensorSize);

    // int getDelayMs()
    SENSORS_MOCK(MOCK_METHOD0, getDelayMs);

    // int setPhysicalStateAgent(const struct QAndroidPhysicalStateAgent* agent)
    SENSORS_MOCK(MOCK_METHOD1, setPhysicalStateAgent);

    // void advanceTime()
    SENSORS_MOCK(MOCK_METHOD0, advanceTime);
};

}  // namespace android
