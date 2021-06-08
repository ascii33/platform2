/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_COMMON_STREAM_MANIPULATOR_H_
#define CAMERA_COMMON_STREAM_MANIPULATOR_H_

#include <hardware/camera3.h>

#include <vector>

namespace cros {

// Interface class that can be used by feature implementations to add hooks into
// the standard camera HAL3 capture pipeline.
class StreamManipulator {
 public:
  virtual ~StreamManipulator() = default;

  // The followings are hooks to the camera3_device_ops APIs and will be called
  // by CameraDeviceAdapter on the CameraDeviceOpsThread.

  // A hook to the camera3_device_ops::initialize(). Will be called by
  // CameraDeviceAdapter with the camera device static metadata |static_info|.
  virtual bool Initialize(const camera_metadata_t* static_info) = 0;

  // A hook to the upper part of camera3_device_ops::configure_streams().
  // Will be called by CameraDeviceAdapter with the stream configuration
  // |stream_list| requested by the camera client. |streams| carries the set of
  // output streams in |stream_list| and can be used to modify the set of output
  // streams in |stream_list|.
  virtual bool ConfigureStreams(camera3_stream_configuration_t* stream_list,
                                std::vector<camera3_stream_t*>* streams) = 0;

  // A hook to the lower part of camera3_device_ops::configure_streams().
  // Will be called by CameraDeviceAdapter with the updated stream configuration
  // |stream_list| returned by the camera HAL implementation.
  virtual bool OnConfiguredStreams(
      camera3_stream_configuration_t* stream_list) = 0;

  // A hook to the camera3_device_ops::process_capture_request(). Will be called
  // by CameraDeviceAdapter for each incoming capture request |request|.
  virtual bool ProcessCaptureRequest(camera3_capture_request_t* request) = 0;

  // A hook to the camera3_device_ops::flush(). Will be called by
  // CameraDeviceAdapter when the camera client requests a flush.
  virtual bool Flush() = 0;

  // The followings are hooks to the camera3_callback_ops APIs and will be
  // called by CameraDeviceAdapter on the CameraCallbackOpsThread.

  // A hook to the camera3_callback_ops::process_capture_result(). Will be
  // called by CameraDeviceAdapter for each capture result |result| produced by
  // the camera HAL implementation.
  virtual bool ProcessCaptureResult(camera3_capture_result_t* result) = 0;

  // A hook to the camera3_callback_ops::notify(). Will be called by
  // CameraDeviceAdapter for each notify message |msg| produced by the camera
  // HAL implemnetation.
  virtual bool Notify(camera3_notify_msg_t* msg) = 0;
};

}  // namespace cros

#endif  // CAMERA_COMMON_STREAM_MANIPULATOR_H_