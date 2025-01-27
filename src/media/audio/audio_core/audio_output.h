// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OUTPUT_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OUTPUT_H_

#include <lib/async/cpp/task.h>
#include <lib/async/cpp/time.h>
#include <lib/media/cpp/timeline_function.h>
#include <lib/zx/time.h>

#include "src/media/audio/audio_core/audio_device.h"
#include "src/media/audio/audio_core/output_pipeline.h"
#include "src/media/audio/audio_core/process_config.h"

namespace media::audio {

class Packet;

class AudioOutput : public AudioDevice {
 public:
  ~AudioOutput() override = default;

  // Minimum clock lead time for this output
  zx::duration min_lead_time() const override { return min_lead_time_; }

  fit::promise<void, fuchsia::media::audio::UpdateEffectError> UpdateEffect(
      const std::string& instance_name, const std::string& config) override;

  // Replace the existing PipelineConfig and VolumeCurve with new versions, for the sake of tuning.
  fit::promise<void, zx_status_t> UpdatePipelineConfig(const PipelineConfig& config,
                                                       const VolumeCurve& volume_curve) override;

  OutputPipeline* output_pipeline() const { return pipeline_.get(); }

 protected:
  friend class AudioOutputTest;

  AudioOutput(ThreadingModel* threading_model, DeviceRegistry* registry, LinkMatrix* link_matrix);
  AudioOutput(ThreadingModel* threading_model, DeviceRegistry* registry, LinkMatrix* link_matrix,
              std::unique_ptr<AudioDriver>);

  void Process() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token());

  // |media::audio::AudioObject|
  //
  // If we're initializing a source link, then we're connecting a renderer to this output. Else
  // if we're initializing a dest link, then we're being connected as a loopback so we should return
  // our loopback stream.
  fit::result<std::shared_ptr<Mixer>, zx_status_t> InitializeSourceLink(
      const AudioObject& source, std::shared_ptr<ReadableStream> stream) final;
  void CleanupSourceLink(const AudioObject& source, std::shared_ptr<ReadableStream> stream) final;
  fit::result<std::shared_ptr<ReadableStream>, zx_status_t> InitializeDestLink(
      const AudioObject& dest) override;

  // Mark this output as needing to be mixed at the specified future time.
  // async PostForTime requires a time in the CLOCK_MONOTONIC timebase, so we use that here.
  void SetNextSchedTime(zx::time next_sched_time_mono) {
    next_sched_time_mono_ = next_sched_time_mono;
    next_sched_time_known_ = true;
  }

  void SetupMixTask(const PipelineConfig& config, const VolumeCurve& volume_curve,
                    size_t max_block_size_frames,
                    TimelineFunction device_reference_clock_to_fractional_frame)
      FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token());
  virtual std::unique_ptr<OutputPipeline> CreateOutputPipeline(
      const PipelineConfig& config, const VolumeCurve& volume_curve, size_t max_block_size_frames,
      TimelineFunction device_reference_clock_to_fractional_frame, ClockReference ref_clock);

  void SetMinLeadTime(zx::duration min_lead_time) { min_lead_time_ = min_lead_time; }

  void Cleanup() override FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token());

  struct FrameSpan {
    int64_t start;
    uint32_t length;
    bool is_mute;
  };

  // Start mixing frames for a periodic mix job. This is called internally during the periodic mix
  // task for this output. Implementations can control mix behavior in the following ways:
  //
  // If |std::nullopt| is returned, then no frames will be mixed. Instead all inputs will be trimmed
  // such that any client audio packets that will have been fully consumed by |process_start| will
  // still be released. There will be no call to |FinishMixJob|.
  //
  // If the retuned optional contains a FrameSpan with |is_mute| set to true, then no frames will
  // be mixed. Instead all inputs will be trimmed such that any client audio packets that will have
  // been fully consumed by |process_start| will still be released. |FinishMixJob| will be called
  // with the returned FrameSpan and a null payload buffer. It is the responsibility of
  // |FinishMixJob| to produce the silence for the FrameSpan.
  //
  // If the retuned optional contains a FrameSpan with |is_mute| set to false, then the mix
  // pipeline will be advanced by the requested frame region. |FinishMixJob| will be called with a
  // FrameSpan that is 'at most' as long as the span in |StartMixJob|, but this length may be
  // reduced if the pipeline is unable to fill a single, contiguous buffer will all the frames
  // requested. If the entire region in StartMixJob is unable to be populated in a single pass, then
  // StartMixJob will be called again to process any remaining frames.
  virtual std::optional<AudioOutput::FrameSpan> StartMixJob(zx::time process_start)
      FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token()) = 0;

  // Finish a mix job by moving the frame range span |span| into the hardware ring buffer using
  // |buffer| as a source. |span.start| should be a value that was provided in |StartMixJob| and
  // |span.length| should be at most the value returned from |StartMixJob|, but may be adjusted
  // downwards if the full range cannot be produced.
  //
  // If |span.is_mute| is false, |buffer| must contain |span.length * channels| floating point
  // samples of audio data.
  //
  // If |span.is_mute| is true, then |buffer| is ignored and instead silence will be inserted
  // into the ring buffer for the frame range in |span|.
  virtual void FinishMixJob(const AudioOutput::FrameSpan& span, float* buffer)
      FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token()) = 0;

 private:
  // Timer used to schedule periodic mixing.
  void MixTimerThunk() {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
    Process();
  }
  async::TaskClosureMethod<AudioOutput, &AudioOutput::MixTimerThunk> mix_timer_
      FXL_GUARDED_BY(mix_domain().token()){this};

  zx::duration min_lead_time_;
  zx::time next_sched_time_mono_;
  bool next_sched_time_known_;
  size_t max_block_size_frames_;

  std::unique_ptr<OutputPipeline> pipeline_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OUTPUT_H_
