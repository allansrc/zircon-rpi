// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/driver_output.h"

#include <lib/async/cpp/time.h>
#include <lib/fit/defer.h>
#include <lib/trace/event.h>
#include <lib/zx/clock.h>

#include <algorithm>
#include <iomanip>

#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/reporter.h"

constexpr bool VERBOSE_TIMING_DEBUG = false;

namespace media::audio {

static constexpr fuchsia::media::AudioSampleFormat kDefaultAudioFmt =
    fuchsia::media::AudioSampleFormat::SIGNED_24_IN_32;
static constexpr zx::duration kDefaultMaxRetentionNsec = zx::msec(60);
static constexpr zx::duration kDefaultRetentionGapNsec = zx::msec(10);
static constexpr zx::duration kUnderflowCooldown = zx::msec(1000);

static std::atomic<zx_txid_t> TXID_GEN(1);
static thread_local zx_txid_t TXID = TXID_GEN.fetch_add(1);

// Consts used if kEnableFinalMixWavWriter is set:
//
// This atomic is only used when the final-mix wave-writer is enabled --
// specifically to generate unique ids for each final-mix WAV file.
std::atomic<uint32_t> DriverOutput::final_mix_instance_num_(0u);
// WAV file location: FilePathName+final_mix_instance_num_+FileExtension
constexpr const char* kDefaultWavFilePathName = "/tmp/final_mix_";
constexpr const char* kWavFileExtension = ".wav";

DriverOutput::DriverOutput(ThreadingModel* threading_model, DeviceRegistry* registry,
                           zx::channel initial_stream_channel, LinkMatrix* link_matrix,
                           VolumeCurve volume_curve)
    : AudioOutput(threading_model, registry, link_matrix, std::make_unique<AudioDriverV1>(this)),
      initial_stream_channel_(std::move(initial_stream_channel)),
      volume_curve_(std::move(volume_curve)) {}

DriverOutput::DriverOutput(ThreadingModel* threading_model, DeviceRegistry* registry,
                           fidl::InterfaceHandle<fuchsia::hardware::audio::StreamConfig> channel,
                           LinkMatrix* link_matrix, VolumeCurve volume_curve)
    : AudioOutput(threading_model, registry, link_matrix, std::make_unique<AudioDriverV2>(this)),
      initial_stream_channel_(channel.TakeChannel()),
      volume_curve_(std::move(volume_curve)) {}

DriverOutput::~DriverOutput() { wav_writer_.Close(); }

zx_status_t DriverOutput::Init() {
  TRACE_DURATION("audio", "DriverOutput::Init");
  FX_DCHECK(state_ == State::Uninitialized);

  zx_status_t res = AudioOutput::Init();
  if (res != ZX_OK) {
    return res;
  }

  res = driver()->Init(std::move(initial_stream_channel_));
  if (res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Failed to initialize driver object";
    return res;
  }

  state_ = State::FormatsUnknown;
  return res;
}

void DriverOutput::OnWakeup() {
  TRACE_DURATION("audio", "DriverOutput::OnWakeup");
  // If we are not in the FormatsUnknown state, then we have already started the
  // state machine.  There is (currently) nothing else to do here.
  FX_DCHECK(state_ != State::Uninitialized);
  if (state_ != State::FormatsUnknown) {
    return;
  }

  // Kick off the process of driver configuration by requesting the basic driver
  // info, which will include the modes which the driver supports.
  driver()->GetDriverInfo();
  state_ = State::FetchingFormats;
}

std::optional<AudioOutput::FrameSpan> DriverOutput::StartMixJob(zx::time ref_time) {
  TRACE_DURATION("audio", "DriverOutput::StartMixJob");
  if (state_ != State::Started) {
    FX_LOGS(ERROR) << "Bad state during StartMixJob " << static_cast<uint32_t>(state_);
    state_ = State::Shutdown;
    ShutdownSelf();
    return std::nullopt;
  }

  // TODO(mpuryear): Depending on policy, use send appropriate commands to the
  // driver to control gain as well.  Some policy settings which might be useful
  // include...
  //
  // ++ Never use HW gain, even if it supports it.
  // ++ Always use HW gain when present, regardless of its limitations.
  // ++ Use HW gain when present, but only if it reaches a minimum bar of
  //    functionality.
  // ++ Implement a hybrid of HW/SW gain.  IOW - Get as close as possible to our
  //    target using HW, and then get the rest of the way there using SW
  //    scaling.  This approach may end up being unreasonably tricky as we may
  //    not be able to synchronize the HW and SW changes in gain well enough to
  //    avoid strange situations where the jumps in one direction (because of
  //    the SW component), and then in the other (as the HW gain command takes
  //    effect).
  //
  bool output_muted = true;
  const auto& settings = device_settings();
  if (settings != nullptr) {
    auto [flags, cur_gain_state] = settings->SnapshotGainState();
    output_muted = cur_gain_state.muted;
  }

  FX_DCHECK(driver_writable_ring_buffer() != nullptr);
  const auto& ref_clock_to_safe_wr_frame = driver_safe_read_or_write_ref_clock_to_frames();
  const auto& output_frames_per_reference_tick = ref_clock_to_safe_wr_frame.rate();
  const auto& rb = *driver_writable_ring_buffer();
  uint32_t fifo_frames = driver()->fifo_depth_frames();

  // output_frames_consumed is the number of frames that the audio output
  // device's DMA *may* have read so far.  output_frames_transmitted is the
  // slightly-smaller number of frames that have *must* have been transmitted
  // over the interconnect so far.  Note, this is not technically the number of
  // frames which have made sound so far.  Once a frame has left the
  // interconnect, it still has the device's external_delay before it will
  // finally hit the speaker.
  int64_t output_frames_consumed = ref_clock_to_safe_wr_frame.Apply(ref_time.get());
  int64_t output_frames_transmitted = output_frames_consumed - fifo_frames;

  if (output_frames_consumed >= frames_sent_) {
    if (!underflow_start_time_.get()) {
      // If this was the first time we missed our limit, log a message, mark the start time of the
      // underflow event, and fill our entire ring buffer with silence.
      int64_t output_underflow_frames = output_frames_consumed - frames_sent_;
      int64_t low_water_frames_underflow = output_underflow_frames + low_water_frames_;

      zx::duration output_underflow_duration =
          zx::nsec(output_frames_per_reference_tick.Inverse().Scale(output_underflow_frames));
      FX_CHECK(output_underflow_duration.get() >= 0);

      zx::duration output_variance_from_expected_wakeup =
          zx::nsec(output_frames_per_reference_tick.Inverse().Scale(low_water_frames_underflow));

      FX_LOGS(ERROR) << "OUTPUT UNDERFLOW: Missed mix target by (worst-case, expected) = ("
                     << std::setprecision(4)
                     << static_cast<double>(output_underflow_duration.to_nsecs()) / ZX_MSEC(1)
                     << ", " << output_variance_from_expected_wakeup.to_msecs()
                     << ") ms. Cooling down for " << kUnderflowCooldown.to_msecs()
                     << " milliseconds.";

      // Use our Reporter to log this to Cobalt and Inspect, if enabled.
      REPORT(OutputUnderflow(*this, output_underflow_duration, ref_time));

      underflow_start_time_ = ref_time;
      output_producer_->FillWithSilence(rb.virt(), rb.frames());
      zx_cache_flush(rb.virt(), rb.size(), ZX_CACHE_FLUSH_DATA);

      wav_writer_.Close();
    }

    // Regardless of whether this was the first or a subsequent underflow,
    // update the cooldown deadline (the time at which we will start producing
    // frames again, provided we don't underflow again)
    underflow_cooldown_deadline_ = zx::deadline_after(kUnderflowCooldown);
  }

  // We want to fill up to be HighWaterNsec ahead of the current safe write
  // pointer position.  Add HighWaterNsec to our concept of "now" and run it
  // through our transformation to figure out what frame number this.
  int64_t fill_target = ref_clock_to_safe_wr_frame.Apply((ref_time + kDefaultHighWaterNsec).get());

  // Are we in the middle of an underflow cooldown? If so, check whether we have recovered yet.
  if (underflow_start_time_.get()) {
    if (ref_time < underflow_cooldown_deadline_) {
      // Looks like we have not recovered yet.  Pretend to have produced the
      // frames we were going to produce and schedule the next wakeup time.
      frames_sent_ = fill_target;
      ScheduleNextLowWaterWakeup();
      return std::nullopt;
    } else {
      // Looks like we recovered.  Log and go back to mixing.
      FX_LOGS(WARNING) << "OUTPUT UNDERFLOW: Recovered after "
                       << (ref_time - underflow_start_time_).to_msecs() << " ms.";
      underflow_start_time_ = zx::time(0);
      underflow_cooldown_deadline_ = zx::time(0);
    }
  }

  // Compute the number of frames which are currently "in flight".  We define
  // this as the number of frames that we have rendererd into the ring buffer
  // but which have may have not been transmitted over the output's interconnect
  // yet.  The distance between frames_sent_ and output_frames_transmitted
  // should give us this number.
  int64_t frames_in_flight = frames_sent_ - output_frames_transmitted;
  FX_DCHECK((frames_in_flight >= 0) && (frames_in_flight <= rb.frames()));
  FX_DCHECK(frames_sent_ <= fill_target);
  int64_t desired_frames = fill_target - frames_sent_;

  // If we woke up too early to have any work to do, just get out now.
  if (desired_frames == 0) {
    return std::nullopt;
  }

  uint32_t rb_space = rb.frames() - static_cast<uint32_t>(frames_in_flight);
  if (desired_frames > rb.frames()) {
    FX_LOGS(ERROR) << "OUTPUT UNDERFLOW: want to produce " << desired_frames
                   << " but the ring buffer is only " << rb.frames() << " frames long.";
    return std::nullopt;
  }

  uint32_t frames_to_mix = static_cast<uint32_t>(std::min<int64_t>(rb_space, desired_frames));

  return AudioOutput::FrameSpan{
      .start = frames_sent_,
      .length = frames_to_mix,
      .is_mute = output_muted,
  };
}

void DriverOutput::WriteToRing(
    const AudioOutput::FrameSpan& span,
    fit::function<void(uint64_t offset, uint32_t length, void* dest_buf)> writer) {
  TRACE_DURATION("audio", "DriverOutput::FinishMixJob");
  const auto& rb = driver_writable_ring_buffer();
  FX_DCHECK(rb != nullptr);

  size_t frames_left = span.length;
  size_t offset = 0;
  while (frames_left > 0) {
    uint32_t wr_ptr = (span.start + offset) % rb->frames();
    uint32_t contig_space = rb->frames() - wr_ptr;
    uint32_t to_send = frames_left;
    if (to_send > contig_space) {
      to_send = contig_space;
    }
    void* dest_buf = rb->virt() + (rb->format().bytes_per_frame() * wr_ptr);

    writer(offset, to_send, dest_buf);

    frames_left -= to_send;
    offset += to_send;
  }
  frames_sent_ += offset;
}

void DriverOutput::FinishMixJob(const AudioOutput::FrameSpan& span, float* buffer) {
  TRACE_DURATION("audio", "DriverOutput::FinishMixJob");
  if (span.is_mute) {
    FillRingSpanWithSilence(span);
  } else {
    FX_DCHECK(buffer != nullptr);
    WriteToRing(span, [this, buffer](uint64_t offset, uint32_t frames, void* dest_buf) {
      auto job_buf_offset = offset * output_producer_->channels();
      output_producer_->ProduceOutput(buffer + job_buf_offset, dest_buf, frames);

      size_t dest_buf_len = frames * output_producer_->bytes_per_frame();
      wav_writer_.Write(dest_buf, dest_buf_len);
      wav_writer_.UpdateHeader();
      zx_cache_flush(dest_buf, dest_buf_len, ZX_CACHE_FLUSH_DATA);
    });
  }

  if (VERBOSE_TIMING_DEBUG) {
    auto now = async::Now(mix_domain().dispatcher());
    const auto& ref_clock_to_safe_wr_frame = driver_safe_read_or_write_ref_clock_to_frames();
    int64_t output_frames_consumed = ref_clock_to_safe_wr_frame.Apply(now.get());
    int64_t playback_lead_end = frames_sent_ - output_frames_consumed;
    int64_t playback_lead_start = playback_lead_end - span.length;

    FX_LOGS(INFO) << "PLead [" << std::setw(4) << playback_lead_start << ", " << std::setw(4)
                  << playback_lead_end << "]";
  }
  ScheduleNextLowWaterWakeup();
}

void DriverOutput::FillRingSpanWithSilence(const AudioOutput::FrameSpan& span) {
  WriteToRing(span, [this](auto offset, auto frames, auto dest_buf) {
    output_producer_->FillWithSilence(dest_buf, frames);
  });
}

void DriverOutput::ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info,
                                   fuchsia::media::AudioGainValidFlags set_flags) {
  TRACE_DURATION("audio", "DriverOutput::ApplyGainLimits");
  // See the comment at the start of StartMixJob.  The actual limits we set here
  // are going to eventually depend on what our HW gain control capabilities
  // are, and how we choose to apply them (based on policy)
  FX_DCHECK(in_out_info != nullptr);

  // We do not currently allow more than unity gain for audio outputs.
  if (in_out_info->gain_db > 0.0) {
    in_out_info->gain_db = 0;
  }

  // Audio outputs should never support AGC
  in_out_info->flags &= ~(fuchsia::media::AudioGainInfoFlags::AGC_ENABLED);
}

void DriverOutput::ScheduleNextLowWaterWakeup() {
  TRACE_DURATION("audio", "DriverOutput::ScheduleNextLowWaterWakeup");

  // After filling up, we are "high water frames" ahead of the safe write
  // pointer. Compute when this will have been reduced to low_water_frames_.
  // This is when we want to wake up and repeat the mixing cycle.
  //
  // frames_sent_ is the total number of frames we have ever synthesized since
  // starting.  Subtracting low_water_frames_ from this will give us the
  // absolute frame number at which we are only low_water_frames_ ahead of the
  // safe write pointer.  Running this backwards through the safe write
  // pointer's reference clock <-> frame number function will tell us when it
  // will be time to wake up.
  int64_t low_water_frame_number = frames_sent_ - low_water_frames_;
  int64_t low_water_time =
      driver_safe_read_or_write_ref_clock_to_frames().ApplyInverse(low_water_frame_number);
  SetNextSchedTime(zx::time(low_water_time));
}

void DriverOutput::OnDriverInfoFetched() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverInfoFetched");
  auto cleanup = fit::defer([this]() FXL_NO_THREAD_SAFETY_ANALYSIS {
    state_ = State::Shutdown;
    ShutdownSelf();
  });

  if (state_ != State::FetchingFormats) {
    FX_LOGS(ERROR) << "Unexpected GetFormatsComplete while in state "
                   << static_cast<uint32_t>(state_);
    return;
  }

  zx_status_t res;

  auto output_device_profile = ProcessConfig::instance().device_config().output_device_profile(
      driver()->persistent_unique_id());
  float driver_gain_db = output_device_profile.driver_gain_db();
  AudioDeviceSettings::GainState gain_state = {.gain_db = driver_gain_db, .muted = false};
  driver()->SetGain(gain_state, AUDIO_SGF_GAIN_VALID | AUDIO_SGF_MUTE_VALID);

  pipeline_config_ = {output_device_profile.pipeline_config()};

  uint32_t pref_fps = pipeline_config_->frames_per_second();
  uint32_t pref_chan = pipeline_config_->channels();
  fuchsia::media::AudioSampleFormat pref_fmt = kDefaultAudioFmt;
  zx::duration min_rb_duration =
      kDefaultHighWaterNsec + kDefaultMaxRetentionNsec + kDefaultRetentionGapNsec;

  res = driver()->SelectBestFormat(&pref_fps, &pref_chan, &pref_fmt);

  if (res != ZX_OK) {
    FX_LOGS(ERROR) << "Output: cannot match a driver format to this request: " << pref_fps
                   << " Hz, " << pref_chan << "-channel, sample format 0x" << std::hex
                   << static_cast<uint32_t>(pref_fmt);
    return;
  }

  // TODO(mpuryear): Save to the hub the configured format for this output.
  auto format_result = Format::Create(fuchsia::media::AudioStreamType{
      .sample_format = pref_fmt,
      .channels = pref_chan,
      .frames_per_second = pref_fps,
  });
  if (format_result.is_error()) {
    FX_LOGS(ERROR) << "Driver format is invalid";
    return;
  }
  auto& format = format_result.value();

  // Update our pipeline to produce audio in the compatible format.
  if (pipeline_config_->frames_per_second() != pref_fps) {
    FX_LOGS(WARNING) << "Hardware does not support the requested rate of "
                     << pipeline_config_->root().output_rate << " fps; hardware will run at "
                     << pref_fps << " fps";
    pipeline_config_->mutable_root().output_rate = pref_fps;
  }
  if (pipeline_config_->channels() != pref_chan) {
    FX_LOGS(WARNING) << "Hardware does not support the requested channelization of "
                     << pipeline_config_->channels() << " channels; hardware will run at "
                     << pref_chan << " channels";
    pipeline_config_->mutable_root().output_channels = pref_chan;
    // Some effects may perform rechannelization. If the hardware does not support the
    // channelization with rechannelization effects we clear all effects on the final stage. This
    // is a compromise in being robust and gracefully handling misconfiguration.
    for (const auto& effect : pipeline_config_->root().effects) {
      if (effect.output_channels && effect.output_channels != pref_chan) {
        FX_LOGS(ERROR) << "Removing effects on the root stage due to unsupported channelization";
        pipeline_config_->mutable_root().effects.clear();
        break;
      }
    }
  }
  FX_DCHECK(pipeline_config_->frames_per_second() == pref_fps);
  FX_DCHECK(pipeline_config_->channels() == pref_chan);

  // Select our output producer
  output_producer_ = OutputProducer::Select(format.stream_type());
  if (!output_producer_) {
    FX_LOGS(ERROR) << "Output: OutputProducer cannot support this request: " << pref_fps << " Hz, "
                   << pref_chan << "-channel, sample format 0x" << std::hex
                   << static_cast<uint32_t>(pref_fmt);
    return;
  }

  // Start the process of configuring our driver
  res = driver()->Configure(format, min_rb_duration);
  if (res != ZX_OK) {
    FX_LOGS(ERROR) << "Output: failed to configure driver for: " << pref_fps << " Hz, " << pref_chan
                   << "-channel, sample format 0x" << std::hex << static_cast<uint32_t>(pref_fmt)
                   << " (res " << std::dec << res << ")";
    return;
  }

  if constexpr (kEnableFinalMixWavWriter) {
    std::string file_name_ = kDefaultWavFilePathName;
    uint32_t instance_count = final_mix_instance_num_.fetch_add(1);
    file_name_ += (std::to_string(instance_count) + kWavFileExtension);
    wav_writer_.Initialize(file_name_.c_str(), pref_fmt, pref_chan, pref_fps,
                           format.bytes_per_frame() * 8 / pref_chan);
  }

  // Success; now wait until configuration completes.
  state_ = State::Configuring;
  cleanup.cancel();
}

void DriverOutput::OnDriverConfigComplete() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverConfigComplete");
  auto cleanup = fit::defer([this]() FXL_NO_THREAD_SAFETY_ANALYSIS {
    state_ = State::Shutdown;
    ShutdownSelf();
  });

  if (state_ != State::Configuring) {
    FX_LOGS(ERROR) << "Unexpected ConfigComplete while in state " << static_cast<uint32_t>(state_);
    return;
  }

  // Driver is configured, we have all the needed info to compute minimum lead time for this output.
  SetMinLeadTime(driver()->external_delay() + driver()->fifo_depth_duration() +
                 kDefaultHighWaterNsec);

  // Fill our brand new ring buffer with silence
  FX_CHECK(driver_writable_ring_buffer() != nullptr);
  const auto& rb = *driver_writable_ring_buffer();
  FX_DCHECK(output_producer_ != nullptr);
  FX_DCHECK(rb.virt() != nullptr);
  output_producer_->FillWithSilence(rb.virt(), rb.frames());

  // Start the ring buffer running
  //
  // TODO(13292) : Don't actually start things up here. We should start only when we have clients
  // with work to do, and we should stop when we have no work to do.
  zx_status_t res = driver()->Start();
  if (res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Failed to start ring buffer";
    return;
  }

  // Start monitoring plug state.
  res = driver()->SetPlugDetectEnabled(true);
  if (res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Failed to enable plug detection";
    return;
  }

  // Success
  state_ = State::Starting;
  cleanup.cancel();
}

void DriverOutput::OnDriverStartComplete() {
  TRACE_DURATION("audio", "DriverOutput::OnDriverStartComplete");
  if (state_ != State::Starting) {
    FX_LOGS(ERROR) << "Unexpected StartComplete while in state " << static_cast<uint32_t>(state_);
    return;
  }

  // Set up the mix task in the AudioOutput.
  //
  // TODO(39886): The intermediate buffer probably does not need to be as large as the entire ring
  // buffer.  Consider limiting this to be something only slightly larger than a nominal mix job.
  auto format = driver()->GetFormat();
  FX_CHECK(format);
  FX_DCHECK(pipeline_config_);
  SetupMixTask(*pipeline_config_, volume_curve_, driver_writable_ring_buffer()->frames(),
               driver_ptscts_ref_clock_to_fractional_frames());

  // Tell AudioDeviceManager we are ready to be an active audio device.
  ActivateSelf();

  // Compute low_water_frames_.  low_water_frames_ is minimum the number of
  // frames ahead of the safe write position we ever want to be.  When we hit
  // the point where we are only this number of frames ahead of the safe write
  // position, we need to wake up and fill up to our high water mark.
  const TimelineRate& rate = driver_safe_read_or_write_ref_clock_to_frames().rate();
  low_water_frames_ = rate.Scale(kDefaultLowWaterNsec.get());

  // We started with a buffer full of silence.  Set up our bookkeeping so we
  // consider ourselves to have generated and sent up to our low-water mark's
  // worth of silence already, then start to generate real frames.  This value
  // should be the sum of the fifo frames and the low water frames.
  int64_t fd_frames = driver()->fifo_depth_frames();
  frames_sent_ = fd_frames + low_water_frames_;

  if (VERBOSE_TIMING_DEBUG) {
    FX_LOGS(INFO) << "Audio output: FIFO depth (" << fd_frames << " frames " << std::fixed
                  << std::setprecision(3) << rate.Inverse().Scale(fd_frames) / 1000000.0
                  << " mSec) Low Water (" << frames_sent_ << " frames " << std::fixed
                  << std::setprecision(3) << rate.Inverse().Scale(frames_sent_) / 1000000.0
                  << " mSec)";
  }

  state_ = State::Started;
  Process();
}

}  // namespace media::audio
