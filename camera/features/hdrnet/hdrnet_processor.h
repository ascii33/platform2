/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_FEATURES_HDRNET_HDRNET_PROCESSOR_H_
#define CAMERA_FEATURES_HDRNET_HDRNET_PROCESSOR_H_

#include <memory>
#include <vector>

#include <base/callback.h>
#include <base/files/scoped_file.h>
#include <base/single_thread_task_runner.h>
#include <system/camera_metadata.h>

#include "cros-camera/common_types.h"
#include "features/hdrnet/hdrnet_config.h"
#include "features/hdrnet/hdrnet_device_processor.h"
#include "gpu/image_processor.h"
#include "gpu/shared_image.h"

namespace cros {

// An interface class to facilitate testing.  For the actual HdrNetProcessor
// implementation, see features/hdrnet/hdrnet_processor_impl.{h,cc}.
class HdrNetProcessor {
 public:
  using Factory = base::RepeatingCallback<std::unique_ptr<HdrNetProcessor>(
      const camera_metadata_t* static_info,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner)>;

  virtual ~HdrNetProcessor() = default;

  // Initializes the HDRnet pipeline. |input_size| is the size of the input
  // buffer (usually in NV12 or P010 format). |output_sizes| are the set of
  // possible output buffer sizes that the pipeline will need to render into.
  virtual bool Initialize(Size input_size,
                          const std::vector<Size>& output_sizes) = 0;

  virtual void TearDown() = 0;

  // Per-frame callback to pass the capture result metadata to HdrNetProcessor.
  // |frame_number| is the frame number as set in the camera3_capture_result
  // struct. |metadata| is the result_metadata carried in the capture result.
  virtual void ProcessResultMetadata(int frame_number,
                                     const camera_metadata_t* metadata) = 0;

  // Runs the HDRnet pipeline for frame |frame_number| with configuration
  // specified in |options|. |input_yuv| is the input YUV buffer
  // produced by the device camera stack and |input_release_fence| is the fence
  // FD for |input_yuv|. The implementation should wait on the fence
  // before acquiring the input buffer. The output buffer rendered by the HDRnet
  // pipeline will be scaled and filled the buffers in |output_nv12_buffers|.
  //
  // Returns a fence FD for the output buffers. The FD can be passed as the
  // release FD in the camera3_stream_buffer passed to the client.
  virtual base::ScopedFD Run(
      int frame_number,
      const HdrNetConfig::Options& options,
      const SharedImage& input_yuv,
      base::ScopedFD input_release_fence,
      const std::vector<buffer_handle_t>& output_nv12_buffers) = 0;
};

}  // namespace cros

#endif  // CAMERA_FEATURES_HDRNET_HDRNET_PROCESSOR_H_
