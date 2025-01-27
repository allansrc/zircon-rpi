// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/mix_stage.h"

#include <gmock/gmock.h>

#include "src/media/audio/audio_core/clock_reference.h"
#include "src/media/audio/audio_core/mixer/gain.h"
#include "src/media/audio/audio_core/packet_queue.h"
#include "src/media/audio/audio_core/ring_buffer.h"
#include "src/media/audio/audio_core/testing/fake_stream.h"
#include "src/media/audio/audio_core/testing/packet_factory.h"
#include "src/media/audio/audio_core/testing/threading_model_fixture.h"
#include "src/media/audio/lib/clock/clone_mono.h"

using testing::Each;
using testing::FloatEq;

namespace media::audio {
namespace {

const Format kDefaultFormat =
    Format::Create(fuchsia::media::AudioStreamType{
                       .sample_format = fuchsia::media::AudioSampleFormat::FLOAT,
                       .channels = 2,
                       .frames_per_second = 48000,
                   })
        .take_value();

class MixStageTest : public testing::ThreadingModelFixture {
 protected:
  void SetUp() {
    ref_clock_ = ClockReference::MakeReadonly(clock_mono_);
    mix_stage_ = std::make_shared<MixStage>(kDefaultFormat, 128, timeline_function_, ref_clock_);
  }

  zx::time time_until(zx::duration delta) { return zx::time(delta.to_nsecs()); }

  fbl::RefPtr<VersionedTimelineFunction> timeline_function_ =
      fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
          TimelineRate(FractionalFrames<int64_t>(kDefaultFormat.frames_per_second()).raw_value(),
                       zx::sec(1).to_nsecs())));

  zx::clock clock_mono_ = clock::CloneOfMonotonic();
  ClockReference ref_clock_;

  std::shared_ptr<MixStage> mix_stage_;

  // Views the memory at |ptr| as a std::array of |N| elements of |T|. If |offset| is provided, it
  // is the number of |T| sized elements to skip at the beginning of |ptr|.
  //
  // It is entirely up to the caller to ensure that values of |T|, |N|, and |offset| are chosen to
  // not overflow |ptr|.
  template <typename T, size_t N>
  std::array<T, N>& as_array(void* ptr, size_t offset = 0) {
    return reinterpret_cast<std::array<T, N>&>(static_cast<T*>(ptr)[offset]);
  }
};

// TODO(50004): Add tests to verify we can read from other mix stages with unaligned frames.

TEST_F(MixStageTest, Trim) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));
  testing::PacketFactory packet_factory(dispatcher(), kDefaultFormat, PAGE_SIZE);
  auto packet_queue = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, ref_clock_);
  mix_stage_->AddInput(packet_queue);

  bool packet1_released = false;
  bool packet2_released = false;
  packet_queue->PushPacket(packet_factory.CreatePacket(
      1.0, zx::msec(5), [&packet1_released] { packet1_released = true; }));
  packet_queue->PushPacket(packet_factory.CreatePacket(
      0.5, zx::msec(5), [&packet2_released] { packet2_released = true; }));

  // After 4ms we should still be retaining packet1.
  mix_stage_->Trim(time_until(zx::msec(4)));
  RunLoopUntilIdle();
  ASSERT_FALSE(packet1_released);

  // 5ms; all the audio from packet1 is consumed and it should be released. We should still have
  // packet2, however.
  mix_stage_->Trim(time_until(zx::msec(5)));
  RunLoopUntilIdle();
  ASSERT_TRUE(packet1_released && !packet2_released);

  // After 9ms we should still be retaining packet2.
  mix_stage_->Trim(time_until(zx::msec(9)));
  RunLoopUntilIdle();
  ASSERT_FALSE(packet2_released);

  // Finally after 10ms we will have released packet2.
  mix_stage_->Trim(time_until(zx::msec(10)));
  RunLoopUntilIdle();
  ASSERT_TRUE(packet2_released);
}

TEST_F(MixStageTest, MixUniformFormats) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));

  // Create 2 packet queues that we will mix together.
  auto packet_queue1 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, ref_clock_);
  auto packet_queue2 = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, ref_clock_);
  mix_stage_->AddInput(packet_queue1);
  mix_stage_->AddInput(packet_queue2);

  // Mix 2 packet queues with the following samples and expected outputs. We'll feed this data
  // though the mix stage in 3 passes of 2ms windows:
  //
  //       -----------------------------------
  // q1   | 0.1 | 0.2 | 0.2 | 0.3 | 0.3 | 0.3 |
  //       -----------------------------------
  // q2   | 0.7 | 0.7 | 0.7 | 0.5 | 0.5 | 0.3 |
  //       -----------------------------------
  // mix  | 0.8 | 0.9 | 0.9 | 0.8 | 0.8 | 0.6 |
  //       -----------------------------------
  // pass |     1     |     2     |     3     |
  //       -----------------------------------
  testing::PacketFactory packet_factory1(dispatcher(), kDefaultFormat, PAGE_SIZE);
  {
    packet_queue1->PushPacket(packet_factory1.CreatePacket(0.1, zx::msec(1)));
    packet_queue1->PushPacket(packet_factory1.CreatePacket(0.2, zx::msec(2)));
    packet_queue1->PushPacket(packet_factory1.CreatePacket(0.3, zx::msec(3)));
  }
  testing::PacketFactory packet_factory2(dispatcher(), kDefaultFormat, PAGE_SIZE);
  {
    packet_queue2->PushPacket(packet_factory2.CreatePacket(0.7, zx::msec(3)));
    packet_queue2->PushPacket(packet_factory2.CreatePacket(0.5, zx::msec(2)));
    packet_queue2->PushPacket(packet_factory2.CreatePacket(0.3, zx::msec(1)));
  }

  int64_t output_frame_start = 0;
  uint32_t output_frame_count = 96;
  {  // Mix frames 0-2ms. Expect the first 1ms to be 0.8 samples and the second ms to be 0.9
     // samples.
    auto buf =
        mix_stage_->ReadLock(time_until(zx::msec(2)), output_frame_start, output_frame_count);
    // 1ms @ 48000hz == 48 frames. 2ms == 96 (frames).
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->length().Floor(), 96u);
    // Each frame is 2 channels, so 1ms will be 96 samples.
    auto& arr1 = as_array<float, 96>(buf->payload(), 0);
    EXPECT_THAT(arr1, Each(FloatEq(0.8f)));
    auto& arr2 = as_array<float, 96>(buf->payload(), 96);
    EXPECT_THAT(arr2, Each(FloatEq(0.9f)));
  }

  output_frame_start += output_frame_count;
  {  // Mix frames 2-4ms. Expect the first 1ms to be 0.9 samples and the second ms to be 0.8
     // samples.
    auto buf =
        mix_stage_->ReadLock(time_until(zx::msec(4)), output_frame_start, output_frame_count);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->length().Floor(), 96u);

    auto& arr1 = as_array<float, 96>(buf->payload(), 0);
    EXPECT_THAT(arr1, Each(FloatEq(0.9f)));
    auto& arr2 = as_array<float, 96>(buf->payload(), 96);
    EXPECT_THAT(arr2, Each(FloatEq(0.8f)));
  }

  output_frame_start += output_frame_count;
  {  // Mix frames 4-6ms. Expect the first 1ms to be 0.8 samples and the second ms to be 0.6
     // samples.
    auto buf =
        mix_stage_->ReadLock(time_until(zx::msec(6)), output_frame_start, output_frame_count);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->length().Floor(), 96u);

    auto& arr1 = as_array<float, 96>(buf->payload(), 0);
    EXPECT_THAT(arr1, Each(FloatEq(0.8f)));
    auto& arr2 = as_array<float, 96>(buf->payload(), 96);
    EXPECT_THAT(arr2, Each(FloatEq(0.6f)));
  }
}

TEST_F(MixStageTest, MixFromRingBuffersSinc) {
  // Create a new RingBuffer and add it to our mix stage.
  constexpr uint32_t kRingSizeFrames = 72;

  // We explictly request a SincSampler here to get a non-trivial filter width.
  auto ring_buffer_endpoints = BaseRingBuffer::AllocateSoftwareBuffer(
      kDefaultFormat, timeline_function_, ref_clock_, kRingSizeFrames);
  mix_stage_->AddInput(ring_buffer_endpoints.reader, Mixer::Resampler::WindowedSinc);

  // Fill up the ring buffer with some non-empty samples so that we can observe these values in
  // the mix output.
  constexpr float kRingBufferSampleValue1 = 0.5;
  constexpr float kRingBufferSampleValue2 = 0.7;
  float* ring_buffer_samples = reinterpret_cast<float*>(ring_buffer_endpoints.writer->virt());
  for (size_t sample = 0; sample < kRingSizeFrames; ++sample) {
    ring_buffer_samples[sample] = kRingBufferSampleValue1;
    ring_buffer_samples[kRingSizeFrames + sample] = kRingBufferSampleValue2;
  }

  // Read the ring in two halves, each has been assigned a different source value in the ring
  // above.
  constexpr uint32_t kRequestedFrames = kRingSizeFrames / 2;
  {
    auto buf = mix_stage_->ReadLock(time_until(zx::msec(1)), 0, kRequestedFrames);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->start().Floor(), 0u);
    ASSERT_EQ(buf->length().Floor(), kRequestedFrames);

    auto& arr = as_array<float, kRequestedFrames>(buf->payload(), 0);
    EXPECT_THAT(arr, Each(FloatEq(kRingBufferSampleValue1)));
  }

  {
    auto buf = mix_stage_->ReadLock(time_until(zx::msec(2)), kRequestedFrames, kRequestedFrames);
    ASSERT_TRUE(buf);
    ASSERT_EQ(buf->start().Floor(), kRequestedFrames);
    ASSERT_EQ(buf->length().Floor(), kRequestedFrames);

    auto& arr = as_array<float, 2 * kRequestedFrames>(buf->payload(), 0);
    EXPECT_THAT(arr, Each(FloatEq(kRingBufferSampleValue2)));
  }
}

TEST_F(MixStageTest, MixNoInputs) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));

  constexpr uint32_t kRequestedFrames = 48;
  auto buf = mix_stage_->ReadLock(zx::time(0), 0, kRequestedFrames);

  // With no inputs, we should have a muted buffer with no usages.
  ASSERT_TRUE(buf);
  EXPECT_TRUE(buf->usage_mask().is_empty());
  EXPECT_FLOAT_EQ(buf->gain_db(), fuchsia::media::audio::MUTED_GAIN_DB);
}

TEST_F(MixStageTest, MixSingleInput) {
  // Set timeline rate to match our format.
  auto timeline_function = fbl::MakeRefCounted<VersionedTimelineFunction>(TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs())));

  testing::PacketFactory packet_factory(dispatcher(), kDefaultFormat, PAGE_SIZE);

  static constexpr auto kInputStreamUsage = StreamUsage::WithRenderUsage(RenderUsage::INTERRUPTION);
  auto packet_queue = std::make_shared<PacketQueue>(kDefaultFormat, timeline_function, ref_clock_);
  packet_queue->set_usage(kInputStreamUsage);
  mix_stage_->AddInput(packet_queue);

  packet_queue->PushPacket(packet_factory.CreatePacket(1.0, zx::msec(5)));

  constexpr uint32_t kRequestedFrames = 48;
  auto buf = mix_stage_->ReadLock(zx::time(0), 0, kRequestedFrames);
  ASSERT_TRUE(buf);
  EXPECT_TRUE(buf->usage_mask().contains(kInputStreamUsage));
  EXPECT_FLOAT_EQ(buf->gain_db(), Gain::kUnityGainDb);

  mix_stage_->RemoveInput(*packet_queue);
}

TEST_F(MixStageTest, MixMultipleInputs) {
  // Set timeline rate to match our format.
  auto timeline_function = TimelineFunction(
      TimelineRate(FractionalFrames<uint32_t>(kDefaultFormat.frames_per_second()).raw_value(),
                   zx::sec(1).to_nsecs()));

  auto input1 = std::make_shared<testing::FakeStream>(kDefaultFormat, PAGE_SIZE);
  input1->timeline_function()->Update(timeline_function);
  auto input2 = std::make_shared<testing::FakeStream>(kDefaultFormat, PAGE_SIZE);
  input2->timeline_function()->Update(timeline_function);
  mix_stage_->AddInput(input1);
  mix_stage_->AddInput(input2);

  constexpr uint32_t kRequestedFrames = 48;

  // The buffer should return the union of the usage mask, and the largest of the input gains.
  input1->set_usage_mask(StreamUsageMask({StreamUsage::WithRenderUsage(RenderUsage::MEDIA)}));
  input1->set_gain_db(-20);
  input2->set_usage_mask(
      StreamUsageMask({StreamUsage::WithRenderUsage(RenderUsage::COMMUNICATION)}));
  input2->set_gain_db(-15);
  {
    auto buf = mix_stage_->ReadLock(zx::time(0), 0, kRequestedFrames);
    ASSERT_TRUE(buf);
    EXPECT_EQ(buf->usage_mask(), StreamUsageMask({
                                     StreamUsage::WithRenderUsage(RenderUsage::MEDIA),
                                     StreamUsage::WithRenderUsage(RenderUsage::COMMUNICATION),
                                 }));
    EXPECT_FLOAT_EQ(buf->gain_db(), -15);
  }
}

}  // namespace
}  // namespace media::audio
