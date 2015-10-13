// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMECAST_PUBLIC_MEDIA_MEDIA_PIPELINE_DEVICE_PARAMS_H_
#define CHROMECAST_PUBLIC_MEDIA_MEDIA_PIPELINE_DEVICE_PARAMS_H_

#include "task_runner.h"

namespace chromecast {
namespace media {

// Supplies creation parameters to platform-specific pipeline backend.
struct MediaPipelineDeviceParams {
  enum MediaSyncType {
    // Default operation, synchronize playback using PTS with higher latency.
    kModeSyncPts = 0,
    // With this mode, synchronization is disabled and audio/video frames are
    // rendered "right away":
    // - for audio, frames are still rendered based on the sampling frequency
    // - for video, frames are rendered as soon as available at the output of
    //   the video decoder.
    //   The assumption is that no B frames are used when synchronization is
    //   disabled, otherwise B frames would always be skipped.
    kModeIgnorePts = 1,
    // In addition to the constraints above, also do not wait for vsync.
    kModeIgnorePtsAndVSync = 2,
  };

  MediaPipelineDeviceParams(TaskRunner* task_runner_in)
      : sync_type(kModeSyncPts), task_runner(task_runner_in) {}

  MediaPipelineDeviceParams(MediaSyncType sync_type_in,
                            TaskRunner* task_runner_in)
      : sync_type(sync_type_in), task_runner(task_runner_in) {}

  const MediaSyncType sync_type;

  // task_runner allows backend implementations to post tasks to the media
  // thread.  Since all calls from cast_shell into the backend are made on
  // the media thread, this may simplify thread management and safety for
  // some backends.
  TaskRunner* const task_runner;
};

}  // namespace media
}  // namespace chromecast

#endif  // CHROMECAST_MEDIA_CMA_BACKEND_MEDIA_PIPELINE_DEVICE_PARAMS_H_
