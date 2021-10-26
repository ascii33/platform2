/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common/stream_manipulator.h"

#include <utility>

#if USE_CAMERA_FEATURE_HDRNET
#include <base/files/file_util.h>

#include "common/still_capture_processor_impl.h"
#include "cros-camera/camera_mojo_channel_manager.h"
#include "cros-camera/constants.h"
#include "cros-camera/jpeg_compressor.h"
#include "features/gcam_ae/gcam_ae_stream_manipulator.h"
#include "features/hdrnet/hdrnet_stream_manipulator.h"
#endif

#if USE_CAMERA_FEATURE_FACE_DETECTION
#include "features/face_detection/face_detection_stream_manipulator.h"
#endif

#include "features/zsl/zsl_stream_manipulator.h"

namespace cros {

void MaybeEnableHdrNetStreamManipulator(
    const StreamManipulator::Options& options,
    std::vector<std::unique_ptr<StreamManipulator>>* out_stream_manipulators) {
#if USE_CAMERA_FEATURE_HDRNET
  if (base::PathExists(base::FilePath(constants::kForceDisableHdrNetPath))) {
    // HDRnet is forcibly disabled.
    return;
  }

  if (base::PathExists(base::FilePath(constants::kForceEnableHdrNetPath)) ||
      options.enable_hdrnet) {
    // HDRnet is enabled forcibly or by the device setting.

    // TODO(jcliang): Update the camera module name here when the names are
    // updated in the HAL (b/194471449).
    constexpr const char kIntelIpu6CameraModuleName[] =
        "Intel Camera3HAL Module";
    if (options.camera_module_name == kIntelIpu6CameraModuleName) {
      // The pipeline looks like:
      //        ____       ________       _________
      //   --> |    | --> |        | --> |         | -->
      //       | FD |     | HDRnet |     | Gcam AE |
      //   <== |____| <== |________| <== |_________| <==
      //
      //   --> capture request flow
      //   ==> capture result flow
      //
      // Why the pipeline is organized this way:
      // * FaceDetection (if present) is placed before HDRnet because we want to
      //   run face detection on result frames rendered by HDRnet so we can
      //   better detect the underexposed faces.
      // * Gcam AE is placed after HDRnet because it needs raw result frames as
      //   input to get accurate AE metering, and because Gcam AE produces the
      //   HDR ratio needed by HDRnet to render the output frame.
      std::unique_ptr<JpegCompressor> jpeg_compressor =
          JpegCompressor::GetInstance(CameraMojoChannelManager::GetInstance());
      out_stream_manipulators->emplace_back(
          std::make_unique<HdrNetStreamManipulator>(
              std::make_unique<StillCaptureProcessorImpl>(
                  std::move(jpeg_compressor))));
      out_stream_manipulators->emplace_back(
          std::make_unique<GcamAeStreamManipulator>());
    }
  }
#endif
}

// static
std::vector<std::unique_ptr<StreamManipulator>>
StreamManipulator::GetEnabledStreamManipulators(Options options) {
  std::vector<std::unique_ptr<StreamManipulator>> stream_manipulators;

#if USE_CAMERA_FEATURE_FACE_DETECTION
  stream_manipulators.emplace_back(
      std::make_unique<FaceDetectionStreamManipulator>());
#endif

  MaybeEnableHdrNetStreamManipulator(options, &stream_manipulators);

  if (options.enable_cros_zsl) {
    stream_manipulators.emplace_back(std::make_unique<ZslStreamManipulator>());
  }

  return stream_manipulators;
}

}  // namespace cros
