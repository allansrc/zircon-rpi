// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    bt_a2dp::{codec::MediaCodecConfig, inspect::DataStreamInspect, media_task::*},
    bt_a2dp_sink_metrics as metrics,
    bt_avdtp::{self as avdtp, MediaStream},
    fuchsia_async,
    fuchsia_bluetooth::types::PeerId,
    fuchsia_cobalt::CobaltSender,
    fuchsia_syslog::{self, fx_log_info, fx_vlog},
    fuchsia_trace as trace,
    futures::{
        future::{AbortHandle, Abortable, Aborted},
        lock::Mutex,
        select, FutureExt, StreamExt,
    },
    std::sync::Arc,
    thiserror::Error,
};

use crate::player;
use crate::DEFAULT_SESSION_ID;

#[derive(Clone)]
pub struct SinkTaskBuilder {
    cobalt_sender: CobaltSender,
}

impl SinkTaskBuilder {
    pub fn new(cobalt_sender: CobaltSender) -> Self {
        Self { cobalt_sender }
    }
}

impl MediaTaskBuilder for SinkTaskBuilder {
    fn configure(
        &self,
        _peer_id: &PeerId,
        codec_config: &MediaCodecConfig,
        data_stream_inspect: DataStreamInspect,
    ) -> Result<Box<dyn MediaTask>, Error> {
        Ok(Box::new(ConfiguredSinkTask::new(
            codec_config,
            self.cobalt_sender.clone(),
            data_stream_inspect,
        )))
    }
}

struct ConfiguredSinkTask {
    /// Configuration providing the format of encoded audio requested.
    codec_config: MediaCodecConfig,
    /// Used to send statistics about the length of playback to cobalt.
    cobalt_sender: CobaltSender,
    /// Handle used to stop the streaming task when stopped.
    stop_sender: Option<AbortHandle>,
    /// Data Stream inspect object for tracking total bytes / current transfer speed.
    stream_inspect: Arc<Mutex<DataStreamInspect>>,
}

impl ConfiguredSinkTask {
    fn new(
        codec_config: &MediaCodecConfig,
        cobalt_sender: CobaltSender,
        stream_inspect: DataStreamInspect,
    ) -> Self {
        Self {
            codec_config: codec_config.clone(),
            cobalt_sender,
            stream_inspect: Arc::new(Mutex::new(stream_inspect)),
            stop_sender: None,
        }
    }
}

impl MediaTask for ConfiguredSinkTask {
    fn start(&mut self, stream: MediaStream) -> Result<(), Error> {
        // TODO(42976) get real media session id
        let session_id = DEFAULT_SESSION_ID;
        let codec_config = self.codec_config.clone();
        let player_fut = media_stream_task(
            stream,
            Box::new(move || player::Player::new(session_id, codec_config.clone())),
            self.stream_inspect.clone(),
        );

        let _ = self.stream_inspect.try_lock().map(|mut l| l.start());
        let (stop_handle, stop_registration) = AbortHandle::new_pair();
        let player_fut = Abortable::new(player_fut, stop_registration);
        let cobalt_sender = self.cobalt_sender.clone();
        let codec_type = self.codec_config.codec_type().clone();
        fuchsia_async::spawn_local(async move {
            let start_time = fuchsia_async::Time::now();
            trace::instant!("bt-a2dp-sink", "Media:Start", trace::Scope::Thread);
            if let Err(Aborted) = player_fut.await {
                fx_log_info!("Player stopped via stop signal");
            }
            trace::instant!("bt-a2dp-sink", "Media:Stop", trace::Scope::Thread);
            let end_time = fuchsia_async::Time::now();

            report_stream_metrics(
                cobalt_sender,
                &codec_type,
                (end_time - start_time).into_seconds(),
            );
        });
        self.stop_sender = Some(stop_handle);
        Ok(())
    }

    fn stop(&mut self) -> Result<(), Error> {
        self.stop_sender.take().map(|x| x.abort());
        Ok(())
    }
}

impl Drop for ConfiguredSinkTask {
    fn drop(&mut self) {}
}

#[derive(Error, Debug)]
enum StreamingError {
    /// The media stream ended.
    #[error("Media stream ended")]
    MediaStreamEnd,
    /// The media stream returned an error. The error is provided.
    #[error("Media stream error: {:?}", _0)]
    MediaStreamError(avdtp::Error),
    /// The Media Player closed unexpectedly.
    #[error("Player closed unexpectedlky")]
    PlayerClosed,
}

/// Wrapper function for media streaming that handles creation of the Player and the media stream
/// metrics reporting
async fn media_stream_task(
    mut stream: (impl futures::Stream<Item = avdtp::Result<Vec<u8>>> + std::marker::Unpin),
    player_gen: Box<dyn Fn() -> Result<player::Player, Error>>,
    inspect: Arc<Mutex<DataStreamInspect>>,
) {
    loop {
        let mut player = match player_gen() {
            Ok(v) => v,
            Err(e) => {
                fx_log_info!("Can't setup player: {:?}", e);
                break;
            }
        };
        // Get the first status from the player to confirm it is setup.
        if let player::PlayerEvent::Closed = player.next_event().await {
            fx_log_info!("Player failed during startup");
            break;
        }

        match decode_media_stream(&mut stream, player, inspect.clone()).await {
            StreamingError::PlayerClosed => fx_log_info!("Player closed, rebuilding.."),
            e => {
                fx_log_info!("Unrecoverable streaming error: {:?}", e);
                break;
            }
        };
    }
}

/// Decodes a media stream by starting a Player and transferring media stream packets from AVDTP
/// to the player.  Restarts the player on player errors.
/// Ends when signaled from `end_signal`, or when the media transport stream is closed.
async fn decode_media_stream(
    stream: &mut (impl futures::Stream<Item = avdtp::Result<Vec<u8>>> + std::marker::Unpin),
    mut player: player::Player,
    inspect: Arc<Mutex<DataStreamInspect>>,
) -> StreamingError {
    let mut packet_count: u64 = 0;
    let _ = inspect.try_lock().map(|mut l| l.start());
    loop {
        select! {
            stream_packet = stream.next().fuse() => {
                let pkt = match stream_packet {
                    None => return StreamingError::MediaStreamEnd,
                    Some(Err(e)) => return StreamingError::MediaStreamError(e),
                    Some(Ok(packet)) => packet,
                };

                packet_count += 1;

                // link incoming and outgoing flows togther with shared duration event
                trace::duration!("bt-a2dp-sink", "ProfilePacket received");
                trace::flow_end!("bluetooth", "ProfilePacket", packet_count);

                if let Err(e) = player.push_payload(&pkt.as_slice()).await {
                    fx_log_info!("can't push packet: {:?}", e);
                }

                let _ = inspect.try_lock().map(|mut l| {
                    l.record_transferred(pkt.len(), fuchsia_async::Time::now());
                });
            },
            player_event = player.next_event().fuse() => {
                match player_event {
                    player::PlayerEvent::Closed => return StreamingError::PlayerClosed,
                    player::PlayerEvent::Status(s) => {
                        fx_vlog!(1, "PlayerEvent Status happened: {:?}", s);
                    },
                }
            },
        }
    }
}

fn report_stream_metrics(
    mut cobalt_sender: CobaltSender,
    codec_type: &avdtp::MediaCodecType,
    duration_seconds: i64,
) {
    let codec = match codec_type {
        &avdtp::MediaCodecType::AUDIO_SBC => {
            metrics::A2dpStreamDurationInSecondsMetricDimensionCodec::Sbc
        }
        &avdtp::MediaCodecType::AUDIO_AAC => {
            metrics::A2dpStreamDurationInSecondsMetricDimensionCodec::Aac
        }
        _ => metrics::A2dpStreamDurationInSecondsMetricDimensionCodec::Unknown,
    };

    cobalt_sender.log_elapsed_time(
        metrics::A2DP_STREAM_DURATION_IN_SECONDS_METRIC_ID,
        codec as u32,
        duration_seconds,
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_cobalt::{CobaltEvent, EventPayload};
    use fidl_fuchsia_media::{
        AudioConsumerRequest, AudioConsumerStatus, SessionAudioConsumerFactoryMarker,
        StreamSinkRequest,
    };
    use fuchsia_async as fasync;
    use fuchsia_inspect as inspect;
    use fuchsia_inspect_derive::WithInspect;
    use futures::{channel::mpsc, pin_mut, task::Poll, StreamExt};

    use crate::tests::fake_cobalt_sender;

    fn setup_media_stream_test(
    ) -> (fasync::Executor, MediaCodecConfig, Arc<Mutex<DataStreamInspect>>) {
        let exec = fasync::Executor::new().expect("executor should build");
        let sbc_config = MediaCodecConfig::min_sbc();
        let inspect = Arc::new(Mutex::new(DataStreamInspect::default()));
        (exec, sbc_config, inspect)
    }

    #[test]
    fn decode_media_stream_empty() {
        let (mut exec, sbc_config, inspect) = setup_media_stream_test();
        let (player, _sink_requests, _consumer_requests, _vmo) =
            player::tests::setup_player(&mut exec, sbc_config);

        let mut empty_stream = futures::stream::empty();

        let decode_fut = decode_media_stream(&mut empty_stream, player, inspect);
        pin_mut!(decode_fut);

        match exec.run_until_stalled(&mut decode_fut) {
            Poll::Ready(StreamingError::MediaStreamEnd) => {}
            x => panic!("Expected decoding to end when media stream ended, got {:?}", x),
        };
    }

    #[test]
    fn decode_media_stream_error() {
        let (mut exec, sbc_config, inspect) = setup_media_stream_test();
        let (player, _sink_requests, _consumer_requests, _vmo) =
            player::tests::setup_player(&mut exec, sbc_config);

        let mut error_stream =
            futures::stream::poll_fn(|_| -> Poll<Option<avdtp::Result<Vec<u8>>>> {
                Poll::Ready(Some(Err(avdtp::Error::PeerDisconnected)))
            });

        let decode_fut = decode_media_stream(&mut error_stream, player, inspect);
        pin_mut!(decode_fut);

        match exec.run_until_stalled(&mut decode_fut) {
            Poll::Ready(StreamingError::MediaStreamError(avdtp::Error::PeerDisconnected)) => {}
            x => panic!("Expected decoding to end with included error, got {:?}", x),
        };
    }

    #[test]
    fn decode_media_player_closed() {
        let (mut exec, sbc_config, inspect) = setup_media_stream_test();
        let (player, mut sink_requests, mut consumer_requests, _vmo) =
            player::tests::setup_player(&mut exec, sbc_config);

        let mut pending_stream = futures::stream::pending();

        let decode_fut = decode_media_stream(&mut pending_stream, player, inspect);
        pin_mut!(decode_fut);

        match exec.run_until_stalled(&mut decode_fut) {
            Poll::Pending => {}
            x => panic!("Expected pending immediately after with no input but got {:?}", x),
        };
        let responder = match exec.run_until_stalled(&mut consumer_requests.select_next_some()) {
            Poll::Ready(Ok(AudioConsumerRequest::WatchStatus { responder, .. })) => responder,
            x => panic!("Expected a watch status request from the player setup, but got {:?}", x),
        };
        drop(responder);
        drop(consumer_requests);
        loop {
            match exec.run_until_stalled(&mut sink_requests.select_next_some()) {
                Poll::Pending => {}
                x => fx_log_info!("Got sink request: {:?}", x),
            };
            match exec.run_until_stalled(&mut decode_fut) {
                Poll::Ready(StreamingError::PlayerClosed) => break,
                Poll::Pending => {}
                x => panic!("Expected decoding to end when player closed, got {:?}", x),
            };
        }
    }

    #[test]
    fn decode_media_stream_stats() {
        let mut exec = fasync::Executor::new_with_fake_time().expect("executor should build");
        let sbc_config = MediaCodecConfig::min_sbc();
        let (player, mut sink_requests, _consumer_requests, _vmo) =
            player::tests::setup_player(&mut exec, sbc_config);
        let inspector = inspect::component::inspector();
        let root = inspector.root();
        let d = DataStreamInspect::default().with_inspect(root, "stream").expect("attach to tree");
        let inspect = Arc::new(Mutex::new(d));

        exec.set_fake_time(fasync::Time::from_nanos(5_678900000));

        let (mut media_sender, mut media_receiver) = mpsc::channel(1);

        let decode_fut = decode_media_stream(&mut media_receiver, player, inspect);
        pin_mut!(decode_fut);

        assert!(exec.run_until_stalled(&mut decode_fut).is_pending());

        fuchsia_inspect::assert_inspect_tree!(inspector, root: {
        stream: {
            start_time: "5.678",
            total_bytes: 0 as u64,
            bytes_per_second_current: 0 as u64,
        }});

        // raw rtp header with sequence number of 1 followed by 1 sbc frame
        let raw = vec![
            128, 96, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x9c, 0xb1, 0x20, 0x3b, 0x80, 0x00, 0x00,
            0x11, 0x7f, 0xfa, 0xab, 0xef, 0x7f, 0xfa, 0xab, 0xef, 0x80, 0x4a, 0xab, 0xaf, 0x80,
            0xf2, 0xab, 0xcf, 0x83, 0x8a, 0xac, 0x32, 0x8a, 0x78, 0x8a, 0x53, 0x90, 0xdc, 0xad,
            0x49, 0x96, 0xba, 0xaa, 0xe6, 0x9c, 0xa2, 0xab, 0xac, 0xa2, 0x72, 0xa9, 0x2d, 0xa8,
            0x9a, 0xab, 0x75, 0xae, 0x82, 0xad, 0x49, 0xb4, 0x6a, 0xad, 0xb1, 0xba, 0x52, 0xa9,
            0xa8, 0xc0, 0x32, 0xad, 0x11, 0xc6, 0x5a, 0xab, 0x3a,
        ];

        media_sender.try_send(Ok(raw)).expect("should be able to send into stream");

        exec.set_fake_time(fasync::Time::from_nanos(6_678900000));
        exec.wake_expired_timers();

        loop {
            assert!(exec.run_until_stalled(&mut decode_fut).is_pending());
            // Should (eventually) get a packet send to the sink
            match exec.run_until_stalled(&mut sink_requests.select_next_some()) {
                Poll::Ready(Ok(StreamSinkRequest::SendPacketNoReply { .. })) => break,
                Poll::Pending => {}
                x => panic!("Expected to receive a packet from sending data.. got {:?}", x),
            };
        }

        assert!(exec.run_until_stalled(&mut decode_fut).is_pending());

        // After the update interval, we should have updated the rx stats.
        fuchsia_inspect::assert_inspect_tree!(inspector, root: {
        stream: {
            start_time: "5.678",
            total_bytes: 85 as u64,
            bytes_per_second_current: 85 as u64,
        }});
    }

    #[test]
    fn media_stream_task_reopens_player() {
        let mut exec = fasync::Executor::new_with_fake_time().expect("executor should build");

        let (audio_consumer_factory_proxy, mut audio_consumer_factory_request_stream) =
            create_proxy_and_stream::<SessionAudioConsumerFactoryMarker>()
                .expect("proxy pair creation");

        let sbc_config = MediaCodecConfig::min_sbc();

        let inspect = Arc::new(Mutex::new(DataStreamInspect::default()));

        let pending_stream = futures::stream::pending();
        let codec_type = sbc_config.codec_type().clone();

        let media_stream_fut = media_stream_task(
            pending_stream,
            Box::new(move || {
                player::Player::from_proxy(
                    DEFAULT_SESSION_ID,
                    sbc_config.clone(),
                    audio_consumer_factory_proxy.clone(),
                )
            }),
            inspect,
        );
        pin_mut!(media_stream_fut);

        assert!(exec.run_until_stalled(&mut media_stream_fut).is_pending());

        let (_sink_request_stream, mut audio_consumer_request_stream, _sink_vmo) =
            player::tests::expect_player_setup(
                &mut exec,
                &mut audio_consumer_factory_request_stream,
                codec_type.clone(),
                DEFAULT_SESSION_ID,
            );
        player::tests::respond_event_status(
            &mut exec,
            &mut audio_consumer_request_stream,
            AudioConsumerStatus {
                min_lead_time: Some(50),
                max_lead_time: Some(500),
                error: None,
                presentation_timeline: None,
            },
        );

        drop(audio_consumer_request_stream);

        assert!(exec.run_until_stalled(&mut media_stream_fut).is_pending());

        // Should set up the player again after it closes.
        let (_sink_request_stream, audio_consumer_request_stream, _sink_vmo) =
            player::tests::expect_player_setup(
                &mut exec,
                &mut audio_consumer_factory_request_stream,
                codec_type,
                DEFAULT_SESSION_ID,
            );

        // This time we don't respond to the event status, so the player failed immediately after
        // trying to be rebuilt and we end.
        drop(audio_consumer_request_stream);

        assert!(exec.run_until_stalled(&mut media_stream_fut).is_ready());
    }

    #[test]
    /// Test that cobalt metrics are sent after stream ends
    fn test_cobalt_metrics() {
        let (send, mut recv) = fake_cobalt_sender();
        const TEST_DURATION: i64 = 1;

        report_stream_metrics(send, &avdtp::MediaCodecType::AUDIO_AAC, TEST_DURATION);

        let event = recv.try_next().expect("no stream error").expect("event present");

        assert_eq!(
            event,
            CobaltEvent {
                metric_id: metrics::A2DP_STREAM_DURATION_IN_SECONDS_METRIC_ID,
                event_codes: vec![
                    metrics::A2dpStreamDurationInSecondsMetricDimensionCodec::Aac as u32
                ],
                component: None,
                payload: EventPayload::ElapsedMicros(TEST_DURATION),
            }
        );
    }
}
