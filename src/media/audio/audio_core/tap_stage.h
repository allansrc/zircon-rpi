// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TAP_STAGE_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TAP_STAGE_H_

#include <memory>

#include "src/media/audio/audio_core/clock_reference.h"
#include "src/media/audio/audio_core/stream.h"

namespace media::audio {

// A |TapStage| reads stream buffers from an input |ReadableStream| and copies them to a secondary
// |WritableStream|.
class TapStage : public ReadableStream {
 public:
  // Creates a |TapStage| that returns buffers from |input| while copying their contents into |tap|.
  TapStage(std::shared_ptr<ReadableStream> input, std::shared_ptr<WritableStream> tap);

  // |media::audio::ReadableStream|
  std::optional<ReadableStream::Buffer> ReadLock(zx::time ref_time, int64_t frame,
                                                 uint32_t frame_count) override;
  void Trim(zx::time ref_time) override { source_->Trim(ref_time); }
  TimelineFunctionSnapshot ReferenceClockToFractionalFrames() const override {
    return source_->ReferenceClockToFractionalFrames();
  }
  ClockReference reference_clock() const override { return source_->reference_clock(); }

  void SetMinLeadTime(zx::duration min_lead_time) override;

 private:
  const TimelineFunction& SourceFracFrameToTapFrame();

  std::shared_ptr<ReadableStream> source_;
  std::shared_ptr<WritableStream> tap_;

  // Track the mapping of source frames to tap frames.
  TimelineFunction source_frac_frame_to_tap_frame_;
  uint32_t source_generation_ = kInvalidGenerationId;
  uint32_t tap_generation_ = kInvalidGenerationId;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TAP_STAGE_H_
