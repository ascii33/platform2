/*
 * Copyright (C) 2017 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "IPCCommon.h"
#include <iostream>
#include <string>
#include <unordered_map>

const char* Intel3AIpcCmdToString(IPC_CMD cmd) {
    static const char* defStr = "unknown";
    static std::unordered_map<int, std::string> gIpcCmdMapping = {
        {IPC_3A_AIC_INIT, "IPC_3A_AIC_INIT"},
        {IPC_3A_AIC_RUN, "IPC_3A_AIC_RUN"},
        {IPC_3A_AIC_RESET, "IPC_3A_AIC_RESET"},
        {IPC_3A_AIC_GETAICVERSION, "IPC_3A_AIC_GETAICVERSION"},
        {IPC_3A_AIC_GETAICCONFIG, "IPC_3A_AIC_GETAICCONFIG"},
        {IPC_3A_AIQ_INIT, "IPC_3A_AIQ_INIT"},
        {IPC_3A_AIQ_DEINIT, "IPC_3A_AIQ_DEINIT"},
        {IPC_3A_AIQ_AE_RUN, "IPC_3A_AIQ_AE_RUN"},
        {IPC_3A_AIQ_AF_RUN, "IPC_3A_AIQ_AF_RUN"},
        {IPC_3A_AIQ_AWB_RUN, "IPC_3A_AIQ_AWB_RUN"},
        {IPC_3A_AIQ_GBCE_RUN, "IPC_3A_AIQ_GBCE_RUN"},
        {IPC_3A_AIQ_PA_RUN, "IPC_3A_AIQ_PA_RUN"},
        {IPC_3A_AIQ_SA_RUN, "IPC_3A_AIQ_SA_RUN"},
        {IPC_3A_AIQ_GET_AIQ_DATA, "IPC_3A_AIQ_GET_AIQ_DATA"},
        {IPC_3A_AIQ_STATISTICS_SET, "IPC_3A_AIQ_STATISTICS_SET"},
        {IPC_3A_AIQ_GET_VERSION, "IPC_3A_AIQ_GET_VERSION"},
        {IPC_3A_CMC_INIT, "IPC_3A_CMC_INIT"},
        {IPC_3A_CMC_DEINIT, "IPC_3A_CMC_DEINIT"},
        {IPC_3A_EXC_ANALOG_GAIN_TO_SENSOR, "IPC_3A_EXC_ANALOG_GAIN_TO_SENSOR"},
        {IPC_3A_EXC_SENSOR_TO_ANALOG_GAIN, "IPC_3A_EXC_SENSOR_TO_ANALOG_GAIN"},
        {IPC_3A_MKN_INIT, "IPC_3A_MKN_INIT"},
        {IPC_3A_MKN_UNINIT, "IPC_3A_MKN_UNINIT"},
        {IPC_3A_MKN_PREPARE, "IPC_3A_MKN_PREPARE"},
        {IPC_3A_MKN_ENABLE, "IPC_3A_MKN_ENABLE"},
        {IPC_3A_COORDINATE_COVERT, "IPC_3A_COORDINATE_COVERT"},
        {IPC_3A_COORDINATE_FACES, "IPC_3A_COORDINATE_FACES"}};

    auto item = gIpcCmdMapping.find(cmd);
    if (item != gIpcCmdMapping.end()) {
        return item->second.c_str();
    }

    return defStr;
}
