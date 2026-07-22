// FFmpeg-based RTMP streaming:
// BGRA -> YUV420P (swscale) -> H.264 (libx264 / NVENC) -> FLV container -> librtmp / avformat RTMP.

#include "RTMPStreamer.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformProcess.h"
#include "Engine/Engine.h"
#include "AudioDevice.h"
#include "Sound/SoundSubmix.h"

// FFmpeg C headers
THIRD_PARTY_INCLUDES_START
#pragma warning(push)
#pragma warning(disable: 4510 4512 4610)
extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/channel_layout.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}
#pragma warning(pop)
THIRD_PARTY_INCLUDES_END

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void FRTMPStreamer::LogFFmpegError(int ErrCode, const TCHAR* Context) {
    char Buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ErrCode, Buf, sizeof(Buf));
    UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] %s: %s"), Context, ANSI_TO_TCHAR(Buf));
}

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------
FRTMPStreamer::FRTMPStreamer() {
    FrameAvailableEvent = FPlatformProcess::GetSynchEventFromPool(false /*manual reset*/);
    AudioDataEvent = FPlatformProcess::GetSynchEventFromPool(false /*manual reset*/);
}

FRTMPStreamer::~FRTMPStreamer() {
    Stop();
    if (FrameAvailableEvent) {
        FPlatformProcess::ReturnSynchEventToPool(FrameAvailableEvent);
        FrameAvailableEvent = nullptr;
    }
    if (AudioDataEvent) {
        FPlatformProcess::ReturnSynchEventToPool(AudioDataEvent);
        AudioDataEvent = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool FRTMPStreamer::Start(const FString& InRTMPUrl, int32 Width, int32 Height, int32 FPS, int32 BitRateKbps,
    ERTMPAudioMode InAudioMode, int32 AudioSampleRate, int32 AudioBitRateKbps, UWorld* InWorld) {
    if (bRunning) {
        UE_LOG(LogTemp, Warning, TEXT("[RTMPStreamer] Already running. Call Stop() first."));
        return false;
    }

    StreamWidth = Width;
    StreamHeight = Height;
    StreamFPS = FPS;
    AudioMode = InAudioMode;
    bStopRequested = false;
    StreamStartTime = FPlatformTime::Seconds();
    LastVideoPts = -1;
    CaptureWorld = InWorld;

    if (!OpenEncoder(Width, Height, FPS, BitRateKbps)) {
        Cleanup();
        return false;
    }

    if (AudioMode != ERTMPAudioMode::None) {
        if (!OpenAudioEncoder(AudioSampleRate, AudioBitRateKbps)) {
            Cleanup();
            return false;
        }
    }

    if (!OpenOutput(InRTMPUrl)) {
        Cleanup();
        return false;
    }

    if (AudioMode != ERTMPAudioMode::None) {
        RegisterAudioCapture();

        AudioRunnable = MakeUnique<FAudioRunnable>(this);
        AudioWorkerThread = FRunnableThread::Create(
            AudioRunnable.Get(), TEXT("RTMPStreamerAudioWorker"), 0, TPri_Normal);
    }

    bRunning = true;
    VideoRunnable = MakeUnique<FVideoRunnable>(this);
    VideoWorkerThread = FRunnableThread::Create(
        VideoRunnable.Get(), TEXT("RTMPStreamerVideoWorker"), 0, TPri_Normal);

    UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Stream started: %s  (%dx%d @ %dfps, %dkbps, audio=%d)"),
        *InRTMPUrl, Width, Height, FPS, BitRateKbps, (int32)AudioMode);
    return true;
}

void FRTMPStreamer::SubmitFrame(const TArray<uint8>& BGRAData, int32 Width, int32 Height) {
    if (!bRunning) {
        return;
    }

    FQueuedFrame Frame;
    Frame.Data = BGRAData;
    Frame.Width = Width;
    Frame.Height = Height;
    Frame.CaptureTimeSeconds = FPlatformTime::Seconds();

    {
        FScopeLock Lock(&QueueMutex);
        // If the queue is full, drop the oldest frame so the new one fits.
        if (FrameQueue.Count() >= (uint32)(QueueCapacity - 1)) {
            FQueuedFrame Discarded;
            FrameQueue.Dequeue(Discarded);
            UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Dropped a video frame (frame queue full)."));
        }
        FrameQueue.Enqueue(MoveTemp(Frame));
    }

    FrameAvailableEvent->Trigger();
}

void FRTMPStreamer::Stop() {
    if (!bRunning || bStopRequested) {
        return;
    }

    bStopRequested = true;

    // Stop new audio buffers from arriving before we drain what's left. Do this from the
    // game thread (where Stop() is expected to be called) rather than a worker thread.
    UnregisterAudioCapture();

    if (FrameAvailableEvent) {
        FrameAvailableEvent->Trigger(); // wake the video worker
    }
    if (AudioDataEvent) {
        AudioDataEvent->Trigger(); // wake the audio worker
    }

    // The video worker thread's Run() waits for the audio worker to finish flushing
    // before it writes the trailer, so joining it here is sufficient to guarantee
    // ordered shutdown.
    if (VideoWorkerThread) {
        VideoWorkerThread->WaitForCompletion();
        delete VideoWorkerThread;
        VideoWorkerThread = nullptr;
    }
    VideoRunnable.Reset();

    if (AudioWorkerThread) {
        delete AudioWorkerThread;
        AudioWorkerThread = nullptr;
    }
    AudioRunnable.Reset();

    bRunning = false;
    UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Stream stopped."));
}

// ---------------------------------------------------------------------------
// Video worker thread
// ---------------------------------------------------------------------------
uint32 FRTMPStreamer::RunVideoThread() {
    auto DrainTimestampedVideoQueue = [this]()
        {
            while (true) {
                FQueuedFrame Frame;
                bool bGotFrame = false;
                {
                    FScopeLock Lock(&QueueMutex);
                    bGotFrame = FrameQueue.Dequeue(Frame);
                }
                if (!bGotFrame) {
                    break;
                }
                ProcessVideoSamples(Frame);
            }
        };

    while (!bStopRequested) {
        // Wait for a frame (with a generous timeout so we can check bStopRequested)
        FrameAvailableEvent->Wait(200 /*ms*/);
        DrainTimestampedVideoQueue();

    }

    // Audio is processed entirely on its own worker thread so a slow audio path can
    // never starve video encoding. Wait for it to finish flushing its encoder and
    // writing its remaining packets before we drain video and write the trailer.
    if (AudioWorkerThread) {
        AudioWorkerThread->WaitForCompletion();
    }

    // Drain the remaining queued frames
    DrainTimestampedVideoQueue();

    DrainEncoder();
    Cleanup();
    return 0;
}

// ---------------------------------------------------------------------------
// Audio worker thread
// ---------------------------------------------------------------------------
uint32 FRTMPStreamer::RunAudioThread() {
    auto DrainTimestampedAudioQueue = [this]()
        {
            while (true) {
                FQueuedAudioChunk Chunk;
                bool bGotChunk = false;
                {
                    FScopeLock Lock(&AudioQueueMutex);
                    bGotChunk = AudioQueue.Dequeue(Chunk);
                }
                if (!bGotChunk) {
                    break;
                }

                ProcessAudioSamples(Chunk);
            }
        };

    while (!bStopRequested) {
        AudioDataEvent->Wait(50 /*ms*/);
        DrainTimestampedAudioQueue();
    }

    // Final drain of whatever arrived right before shutdown, then flush the
    // encoder's internal buffering and write the remaining packets.
    DrainTimestampedAudioQueue();
    FlushAudioEncoder();
    return 0;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
bool FRTMPStreamer::OpenEncoder(int32 Width, int32 Height, int32 FPS, int32 BitRateKbps) {
    // ----- Codec -----
    const AVCodec* Codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!Codec) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] H.264 encoder not found in FFmpeg build."));
        return false;
    }

    CodecCtx = avcodec_alloc_context3(Codec);
    if (!CodecCtx) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] Failed to allocate codec context."));
        return false;
    }

    CodecCtx->codec_id = AV_CODEC_ID_H264;
    CodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    CodecCtx->width = Width;
    CodecCtx->height = Height;
    CodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    CodecCtx->bit_rate = (int64_t)BitRateKbps * 1000;
    CodecCtx->rc_max_rate = CodecCtx->bit_rate;
    CodecCtx->rc_buffer_size = CodecCtx->bit_rate;
    CodecCtx->time_base = { 1, FPS };
    CodecCtx->framerate = { FPS, 1 };
    CodecCtx->gop_size = FPS;          // one keyframe per second
    CodecCtx->max_b_frames = 0;          // no B-frames for low latency RTMP

    // The FLV/RTMP muxer requires global headers (SPS/PPS go into extradata instead
    // of being repeated in-band). Without this flag some players/decoders will
    // eventually choke on the stream once the AAC audio track is added (its
    // AudioSpecificConfig has the same requirement — see OpenAudioEncoder).
    CodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Use the "zerolatency" tune for live streaming
    av_opt_set(CodecCtx->priv_data, "preset", "veryfast", 0);
    av_opt_set(CodecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(CodecCtx->priv_data, "profile", "high", 0);

    int Ret = avcodec_open2(CodecCtx, Codec, nullptr);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("avcodec_open2"));
        return false;
    }

    // ----- YUV frame -----
    YUVFrame = av_frame_alloc();
    if (!YUVFrame) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] Failed to allocate YUV frame."));
        return false;
    }
    YUVFrame->format = AV_PIX_FMT_YUV420P;
    YUVFrame->width = Width;
    YUVFrame->height = Height;
    Ret = av_frame_get_buffer(YUVFrame, 0);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("av_frame_get_buffer"));
        return false;
    }

    // ----- Packet -----
    Packet = av_packet_alloc();
    if (!Packet) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] av_packet_alloc failed."));
        return false;
    }

    // ----- swscale: BGRA -> YUV420P -----
    SwsCtx = sws_getContext(
        Width, Height, AV_PIX_FMT_BGRA,
        Width, Height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!SwsCtx) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] sws_getContext failed."));
        return false;
    }

    return true;
}

bool FRTMPStreamer::OpenAudioEncoder(int32 AudioSampleRate, int32 AudioBitRateKbps) {
    const int32 Channels = (AudioMode == ERTMPAudioMode::Stereo) ? 2 : 1;

    const AVCodec* AudioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!AudioCodec) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] AAC encoder not found in FFmpeg build."));
        return false;
    }

    AudioCodecCtx = avcodec_alloc_context3(AudioCodec);
    if (!AudioCodecCtx) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] Failed to allocate audio codec context."));
        return false;
    }

    AudioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    AudioCodecCtx->sample_rate = AudioSampleRate;
    av_channel_layout_default(&AudioCodecCtx->ch_layout, Channels);
    AudioCodecCtx->bit_rate = (int64_t)AudioBitRateKbps * 1000;
    AudioCodecCtx->time_base = { 1, AudioSampleRate };

    // Required by the FLV/RTMP muxer: without this, the AAC AudioSpecificConfig is
    // not written to extradata, so most decoders can't parse the audio track and the
    // stream stalls out shortly after audio packets start arriving.
    AudioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int Ret = avcodec_open2(AudioCodecCtx, AudioCodec, nullptr);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("avcodec_open2 (audio)"));
        return false;
    }

    AudioFrame = av_frame_alloc();
    if (!AudioFrame) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] Failed to allocate audio frame."));
        return false;
    }
    AudioFrame->format = AV_SAMPLE_FMT_FLTP;
    AudioFrame->sample_rate = AudioSampleRate;
    av_channel_layout_copy(&AudioFrame->ch_layout, &AudioCodecCtx->ch_layout);
    AudioFrame->nb_samples = AudioCodecCtx->frame_size > 0 ? AudioCodecCtx->frame_size : 1024;
    Ret = av_frame_get_buffer(AudioFrame, 0);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("av_frame_get_buffer (audio)"));
        return false;
    }

    AudioPacket = av_packet_alloc();
    if (!AudioPacket) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] av_packet_alloc (audio) failed."));
        return false;
    }

    AudioFifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, Channels, 1);
    if (!AudioFifo) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] av_audio_fifo_alloc failed."));
        return false;
    }

    AudioChannels = Channels;
    AudioTargetSampleRate = AudioSampleRate;

    return true;
}

bool FRTMPStreamer::EnsureResampler(int32 InChannels, int32 InSampleRate) {
    if (SwrCtx && InChannels == SwrInChannels && InSampleRate == SwrInSampleRate) {
        return true;
    }

    if (SwrCtx) {
        swr_free(&SwrCtx);
    }

    AVChannelLayout InLayout;
    av_channel_layout_default(&InLayout, InChannels);

    int Ret = swr_alloc_set_opts2(&SwrCtx, &AudioCodecCtx->ch_layout, AV_SAMPLE_FMT_FLTP, AudioTargetSampleRate,
        &InLayout, AV_SAMPLE_FMT_FLT, InSampleRate, 0, nullptr);
    av_channel_layout_uninit(&InLayout);

    if (Ret < 0 || !SwrCtx) {
        LogFFmpegError(Ret, TEXT("swr_alloc_set_opts2"));
        return false;
    }

    Ret = swr_init(SwrCtx);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("swr_init"));
        return false;
    }

    SwrInChannels = InChannels;
    SwrInSampleRate = InSampleRate;
    return true;
}

bool FRTMPStreamer::EnsureConvertedBuffer(int32 RequiredSamplesPerChannel) {
    if (AudioConvertedData && RequiredSamplesPerChannel <= AudioConvertedCapacitySamples) {
        return true;
    }

    if (AudioConvertedData) {
        av_freep(&AudioConvertedData[0]);
        av_freep(&AudioConvertedData);
        AudioConvertedCapacitySamples = 0;
    }

    // Add headroom so we don't reallocate on every small growth.
    const int32 CapacityToAlloc = FMath::Max(RequiredSamplesPerChannel, 4096);
    int Ret = av_samples_alloc_array_and_samples(&AudioConvertedData, &AudioConvertedLineSize, AudioChannels,
        CapacityToAlloc, AV_SAMPLE_FMT_FLTP, 0);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("av_samples_alloc_array_and_samples"));
        AudioConvertedData = nullptr;
        return false;
    }

    AudioConvertedCapacitySamples = CapacityToAlloc;
    return true;
}

void FRTMPStreamer::ProcessAudioSamples(const FQueuedAudioChunk& Chunk) {
    if (!AudioCodecCtx || !AudioFifo || Chunk.NumChannels <= 0 || Chunk.NumFrames <= 0) {
        return;
    }
    if (!EnsureResampler(Chunk.NumChannels, Chunk.SampleRate)) {
        return;
    }

    const uint8* InData[1] = { reinterpret_cast<const uint8*>(Chunk.Data.GetData()) };

    const int64 MaxOutSamples = av_rescale_rnd(
        swr_get_delay(SwrCtx, Chunk.SampleRate) + Chunk.NumFrames,
        AudioTargetSampleRate, Chunk.SampleRate, AV_ROUND_UP);

    if (!EnsureConvertedBuffer((int32)MaxOutSamples)) {
        return;
    }

    const int OutSamples = swr_convert(SwrCtx, AudioConvertedData, AudioConvertedCapacitySamples, InData, Chunk.NumFrames);
    if (OutSamples > 0) {
        av_audio_fifo_write(AudioFifo, (void**)AudioConvertedData, OutSamples);
    }

    const int32 EncoderFrameSize = AudioFrame->nb_samples;
    while (av_audio_fifo_size(AudioFifo) >= EncoderFrameSize) {
        av_frame_make_writable(AudioFrame);
        av_audio_fifo_read(AudioFifo, (void**)AudioFrame->data, EncoderFrameSize);

        const double ElapsedSeconds = Chunk.CaptureTimeSeconds - StreamStartTime;
        int64 AudioPts = (int64)FMath::RoundToDouble(ElapsedSeconds * AudioTargetSampleRate);
        if (AudioPts < 0) {
            AudioPts = 0;
        }

        AudioFrame->pts = AudioPts;

        int SendRet = avcodec_send_frame(AudioCodecCtx, AudioFrame);
        if (SendRet < 0) {
            LogFFmpegError(SendRet, TEXT("avcodec_send_frame (audio)"));
            break;
        }

        while (SendRet >= 0) {
            SendRet = avcodec_receive_packet(AudioCodecCtx, AudioPacket);
            if (SendRet == AVERROR(EAGAIN) || SendRet == AVERROR_EOF) {
                break;
            }
            if (SendRet < 0) {
                LogFFmpegError(SendRet, TEXT("avcodec_receive_packet (audio)"));
                break;
            }

            av_packet_rescale_ts(AudioPacket, AudioCodecCtx->time_base, AudioStream->time_base);
            AudioPacket->stream_index = AudioStream->index;

            {
                FScopeLock MuxLock(&MuxerMutex);
                int WriteRet = av_interleaved_write_frame(FormatCtx, AudioPacket);
                if (WriteRet < 0) {
                    LogFFmpegError(WriteRet, TEXT("av_interleaved_write_frame (audio)"));
                }
            }

            av_packet_unref(AudioPacket);
        }
    }
}

void FRTMPStreamer::SubmitAudioSamples(const float* Data, int32 NumSamples, int32 NumChannels, int32 SampleRate, double AudioClock) {
    // Called on the real-time audio render thread: must not allocate memory and must
    // only ever do a bounded amount of work (copy + queue push under a short lock).
    if (!bRunning || AudioMode == ERTMPAudioMode::None || !Data || NumSamples <= 0 || NumChannels <= 0) {
        return;
    }

    FQueuedAudioChunk Chunk;
    Chunk.NumChannels = NumChannels;
    Chunk.SampleRate = SampleRate;
    Chunk.NumFrames = NumSamples / NumChannels;
    Chunk.CaptureTimeSeconds = FPlatformTime::Seconds();
    Chunk.Data.Append(Data, NumSamples);

    {
        FScopeLock Lock(&AudioQueueMutex);
        if (AudioQueue.Count() >= (uint32)(AudioQueueCapacity - 1)) {
            FQueuedAudioChunk Discarded;
            AudioQueue.Dequeue(Discarded);
        }
        AudioQueue.Enqueue(MoveTemp(Chunk));
    }

    if (AudioDataEvent) {
        AudioDataEvent->Trigger();
    }
}

void FRTMPStreamer::FlushAudioEncoder() {
    if (!AudioCodecCtx || !AudioStream || !FormatCtx) {
        return;
    }

    // Signal EOF to the audio encoder (any partial FIFO remainder below one encoder
    // frame is dropped, which is a negligible amount of trailing audio).
    avcodec_send_frame(AudioCodecCtx, nullptr);

    int Ret = 0;
    while (Ret >= 0) {
        Ret = avcodec_receive_packet(AudioCodecCtx, AudioPacket);
        if (Ret == AVERROR(EAGAIN) || Ret == AVERROR_EOF) {
            break;
        }
        if (Ret < 0) {
            LogFFmpegError(Ret, TEXT("FlushAudioEncoder receive_packet"));
            break;
        }

        av_packet_rescale_ts(AudioPacket, AudioCodecCtx->time_base, AudioStream->time_base);
        AudioPacket->stream_index = AudioStream->index;

        {
            FScopeLock MuxLock(&MuxerMutex);
            av_interleaved_write_frame(FormatCtx, AudioPacket);
        }

        av_packet_unref(AudioPacket);
    }
}

void FRTMPStreamer::FSubmixListener::OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData,
    int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) {
    if (OnBuffer) {
        OnBuffer(AudioData, NumSamples, NumChannels, SampleRate, AudioClock);
    }
}

void FRTMPStreamer::RegisterAudioCapture() {
    if (AudioMode == ERTMPAudioMode::None || !GEngine) {
        return;
    }

    // Prefer the audio device owned by the world we're actually capturing (e.g. the
    // PIE world), since PIE worlds each get their own FAudioDevice instance that is
    // distinct from GEngine's main audio device. Falling back to the main audio
    // device keeps this working in standalone/packaged builds where there's only
    // ever one audio device.
    FAudioDeviceHandle AudioDevice;
    if (UWorld* World = CaptureWorld.Get()) {
        AudioDevice = World->GetAudioDevice();
    }
    if (!AudioDevice.IsValid()) {
        AudioDevice = GEngine->GetMainAudioDevice();
    }
    if (!AudioDevice.IsValid()) {
        UE_LOG(LogTemp, Warning, TEXT("[RTMPStreamer] No main audio device available; audio will not be streamed."));
        return;
    }

    USoundSubmix& MainSubmix = AudioDevice->GetMainSubmixObject();

    AudioListener = MakeShared<FSubmixListener, ESPMode::ThreadSafe>();
    AudioListener->OnBuffer = [this](const float* Data, int32 NumSamples, int32 NumChannels, int32 SampleRate, double AudioClock)
        {
            SubmitAudioSamples(Data, NumSamples, NumChannels, SampleRate, AudioClock);
        };

    AudioDevice->RegisterSubmixBufferListener(AudioListener.ToSharedRef(), MainSubmix);
    bAudioListenerRegistered = true;
}

void FRTMPStreamer::UnregisterAudioCapture() {
    if (!bAudioListenerRegistered || !AudioListener) {
        return;
    }

    FAudioDeviceHandle AudioDevice;
    if (UWorld* World = CaptureWorld.Get()) {
        AudioDevice = World->GetAudioDevice();
    }
    if (!AudioDevice.IsValid() && GEngine) {
        AudioDevice = GEngine->GetMainAudioDevice();
    }
    if (AudioDevice.IsValid()) {
        AudioDevice->UnregisterSubmixBufferListener(AudioListener.ToSharedRef(), AudioDevice->GetMainSubmixObject());
    }

    AudioListener.Reset();
    bAudioListenerRegistered = false;
}

bool FRTMPStreamer::OpenOutput(const FString& RTMPUrl) {
    int Ret = avformat_alloc_output_context2(&FormatCtx, nullptr, "flv", TCHAR_TO_UTF8(*RTMPUrl));
    if (Ret < 0 || !FormatCtx) {
        LogFFmpegError(Ret, TEXT("avformat_alloc_output_context2 (flv/rtmp)"));
        return false;
    }

    VideoStream = avformat_new_stream(FormatCtx, nullptr);
    if (!VideoStream) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] avformat_new_stream failed."));
        return false;
    }

    Ret = avcodec_parameters_from_context(VideoStream->codecpar, CodecCtx);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("avcodec_parameters_from_context"));
        return false;
    }

    VideoStream->time_base = CodecCtx->time_base;

    if (AudioMode != ERTMPAudioMode::None && AudioCodecCtx) {
        AudioStream = avformat_new_stream(FormatCtx, nullptr);
        if (!AudioStream) {
            UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] avformat_new_stream (audio) failed."));
            return false;
        }

        Ret = avcodec_parameters_from_context(AudioStream->codecpar, AudioCodecCtx);
        if (Ret < 0) {
            LogFFmpegError(Ret, TEXT("avcodec_parameters_from_context (audio)"));
            return false;
        }

        AudioStream->time_base = AudioCodecCtx->time_base;
    }

    // Open network I/O
    if (!(FormatCtx->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary* Opts = nullptr;
        av_dict_set(&Opts, "rtmp_live", "live", 0);
        av_dict_set_int(&Opts, "rw_timeout", 10'000'000, 0); // In microseconds

        Ret = avio_open2(&FormatCtx->pb, TCHAR_TO_UTF8(*RTMPUrl), AVIO_FLAG_WRITE, nullptr, &Opts);
        av_dict_free(&Opts);
        if (Ret < 0) {
            LogFFmpegError(Ret, TEXT("avio_open2 (rtmp)"));
            return false;
        }
    }

    Ret = avformat_write_header(FormatCtx, nullptr);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("avformat_write_header"));
        return false;
    }

    return true;
}

void FRTMPStreamer::ProcessVideoSamples(const FQueuedFrame& Frame) {
    // Rebuild swscale context if the incoming frame size changed (shouldn't happen in practice)
    if (Frame.Width != StreamWidth || Frame.Height != StreamHeight) {
        UE_LOG(LogTemp, Warning, TEXT("[RTMPStreamer] Frame size mismatch — dropping frame."));
        return;
    }

    // Make the frame writable (may be a no-op if already)
    av_frame_make_writable(YUVFrame);

    // Convert BGRA -> YUV420P
    const uint8* BGRAPtr = Frame.Data.GetData();
    int SrcStride[1] = { Frame.Width * 4 }; // 4 bytes per BGRA pixel
    sws_scale(SwsCtx, &BGRAPtr, SrcStride, 0, Frame.Height, YUVFrame->data, YUVFrame->linesize);

    // Derive PTS from the frame's actual capture wall-clock time rather than a naive
    // incrementing counter. If frames are assumed to always be exactly 1/FPS apart,
    // any dip in the game's real tick rate (e.g. CPU power-saving states) causes the
    // declared stream timeline to run faster than real time. That desync eventually
    // forces the player to stall waiting for data to catch up to the (falsely early)
    // declared presentation time, even though frames keep arriving at a steady byte
    // rate -- matching a periodic multi-second stutter with no dropped frames.
    const double ElapsedSeconds = Frame.CaptureTimeSeconds - StreamStartTime;
    int64 Pts = (int64)FMath::RoundToDouble(ElapsedSeconds * StreamFPS);

    // Guard against non-monotonic or duplicate timestamps (e.g. clock issues, or two
    // frames captured in the same instant) -- the encoder requires strictly
    // increasing PTS values.
    if (Pts <= LastVideoPts) {
        Pts++;
        if (Pts <= LastVideoPts) {
            UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Dropped a video frame (frame submission rate higher than stream frame rate)."));
            return;
        }
    }
    LastVideoPts = Pts;

    YUVFrame->pts = Pts;

    // Send frame to encoder
    int Ret = avcodec_send_frame(CodecCtx, YUVFrame);
    if (Ret < 0) {
        LogFFmpegError(Ret, TEXT("avcodec_send_frame"));
        return;
    }

    // Receive and write packets
    while (Ret >= 0) {
        Ret = avcodec_receive_packet(CodecCtx, Packet);
        if (Ret == AVERROR(EAGAIN) || Ret == AVERROR_EOF) {
            break;
        }
        if (Ret < 0) {
            LogFFmpegError(Ret, TEXT("avcodec_receive_packet"));
            break;
        }

        av_packet_rescale_ts(Packet, CodecCtx->time_base, VideoStream->time_base);
        Packet->stream_index = VideoStream->index;

        {
            FScopeLock MuxLock(&MuxerMutex);
            int WriteRet = av_interleaved_write_frame(FormatCtx, Packet);
            if (WriteRet < 0) {
                LogFFmpegError(WriteRet, TEXT("av_interleaved_write_frame"));
            }
        }

        av_packet_unref(Packet);
    }
}

void FRTMPStreamer::DrainEncoder() {
    if (!CodecCtx || !FormatCtx) {
        return;
    }

    // Signal EOF to the encoder
    avcodec_send_frame(CodecCtx, nullptr);

    int Ret = 0;
    while (Ret >= 0) {
        Ret = avcodec_receive_packet(CodecCtx, Packet);
        if (Ret == AVERROR(EAGAIN) || Ret == AVERROR_EOF) {
            break;
        }
        if (Ret < 0) {
            LogFFmpegError(Ret, TEXT("DrainEncoder receive_packet"));
            break;
        }

        av_packet_rescale_ts(Packet, CodecCtx->time_base, VideoStream->time_base);
        Packet->stream_index = VideoStream->index;
        {
            FScopeLock MuxLock(&MuxerMutex);
            av_interleaved_write_frame(FormatCtx, Packet);
        }
        av_packet_unref(Packet);
    }

    // Note: the audio encoder is flushed by FlushAudioEncoder() on the dedicated audio
    // worker thread, which Run() waits to complete before calling DrainEncoder(). This
    // guarantees all audio packets are written before we write the trailer below.

    {
        FScopeLock MuxLock(&MuxerMutex);
        av_write_trailer(FormatCtx);
    }
}

void FRTMPStreamer::Cleanup() {
    UnregisterAudioCapture();

    if (SwsCtx) { sws_freeContext(SwsCtx); SwsCtx = nullptr; }
    if (YUVFrame) { av_frame_free(&YUVFrame); YUVFrame = nullptr; }
    if (Packet) { av_packet_free(&Packet); Packet = nullptr; }
    if (CodecCtx) { avcodec_free_context(&CodecCtx); CodecCtx = nullptr; }

    if (SwrCtx) { swr_free(&SwrCtx); }
    if (AudioConvertedData) { av_freep(&AudioConvertedData[0]); av_freep(&AudioConvertedData); AudioConvertedCapacitySamples = 0; }
    if (AudioFifo) { av_audio_fifo_free(AudioFifo); AudioFifo = nullptr; }
    if (AudioFrame) { av_frame_free(&AudioFrame); AudioFrame = nullptr; }
    if (AudioPacket) { av_packet_free(&AudioPacket); AudioPacket = nullptr; }
    if (AudioCodecCtx) { avcodec_free_context(&AudioCodecCtx); AudioCodecCtx = nullptr; }

    if (FormatCtx) {
        if (FormatCtx->pb && !(FormatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&FormatCtx->pb);
        }
        avformat_free_context(FormatCtx);
        FormatCtx = nullptr;
    }

    VideoStream = nullptr;
    AudioStream = nullptr;
    CaptureWorld.Reset();
    SwrInChannels = 0;
    SwrInSampleRate = 0;
}
