/*
 * Copyright (C) 2020 MediaTek Inc.
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

#ifndef CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_UTILS_STD_MOJO_H_
#define CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_UTILS_STD_MOJO_H_

#include <mtkcam/def/common.h>

namespace NSCam {
namespace Utils {

cros::CameraMojoChannelManagerToken* VISIBILITY_PUBLIC getMojoManagerToken();

void VISIBILITY_PUBLIC
setMojoManagerToken(cros::CameraMojoChannelManagerToken*);

}  // namespace Utils
}  // namespace NSCam

#endif  // CAMERA_HAL_MEDIATEK_MTKCAM_INCLUDE_MTKCAM_UTILS_STD_MOJO_H_
