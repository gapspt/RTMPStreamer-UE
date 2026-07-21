// FFmpeg-based RTMP streaming:
// BGRA -> YUV420P (swscale) -> H.264 (libx264 / NVENC) -> FLV container -> librtmp / avformat RTMP.

#include "RTMPStreamer.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformProcess.h"

// FFmpeg C headers
THIRD_PARTY_INCLUDES_START
#pragma warning(push)
#pragma warning(disable: 4510 4512 4610)
extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
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
}

FRTMPStreamer::~FRTMPStreamer() {
    Stop();
    if (FrameAvailableEvent) {
        FPlatformProcess::ReturnSynchEventToPool(FrameAvailableEvent);
        FrameAvailableEvent = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool FRTMPStreamer::Start(const FString& InRTMPUrl, int32 Width, int32 Height, int32 FPS, int32 BitRateKbps) {
    if (bRunning) {
        UE_LOG(LogTemp, Warning, TEXT("[RTMPStreamer] Already running. Call Stop() first."));
        return false;
    }

    StreamWidth = Width;
    StreamHeight = Height;
    StreamFPS = FPS;
    bStopRequested = false;
    StreamStartTime = FPlatformTime::Seconds();
    LastVideoPts = -1;

    if (!OpenEncoder(Width, Height, FPS, BitRateKbps)) {
        Cleanup();
        return false;
    }

    if (!OpenOutput(InRTMPUrl)) {
        Cleanup();
        return false;
    }

    bRunning = true;
    WorkerThread = FRunnableThread::Create(this, TEXT("RTMPStreamerWorker"),
        0, TPri_Normal);

    UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Stream started: %s  (%dx%d @ %dfps, %dkbps)"),
        *InRTMPUrl, Width, Height, FPS, BitRateKbps);
    return true;
}

void FRTMPStreamer::SubmitFrame(const TArray<uint8>& BGRAData, int32 Width, int32 Height) {
    if (!bRunning) { return; }

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
            UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Dropped a frame (frame queue full)."));
        }
        FrameQueue.Enqueue(MoveTemp(Frame));
    }

    FrameAvailableEvent->Trigger();
}

void FRTMPStreamer::Stop() {
    if (!bRunning || bStopRequested) { return; }

    bStopRequested = true;
    if (FrameAvailableEvent) {
        FrameAvailableEvent->Trigger(); // wake the worker
    }

    if (WorkerThread) {
        WorkerThread->WaitForCompletion();
        delete WorkerThread;
        WorkerThread = nullptr;
    }

    bRunning = false;
    UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Stream stopped."));
}

// ---------------------------------------------------------------------------
// FRunnable::Run — worker thread
// ---------------------------------------------------------------------------
uint32 FRTMPStreamer::Run() {
    while (!bStopRequested) {
        // Wait for a frame (with a generous timeout so we can check bStopRequested)
        FrameAvailableEvent->Wait(200 /*ms*/);

        while (true)
        {
            FQueuedFrame Frame;
            bool bGotFrame = false;
            {
                FScopeLock Lock(&QueueMutex);
                bGotFrame = FrameQueue.Dequeue(Frame);
            }
            if (!bGotFrame) { break; }

            EncodeAndSend(Frame);
        }
    }

    // Drain the remaining queued frames
    while (true) {
        FQueuedFrame Frame;
        bool bGotFrame = false;
        {
            FScopeLock Lock(&QueueMutex);
            bGotFrame = FrameQueue.Dequeue(Frame);
        }
        if (!bGotFrame) { break; }
        EncodeAndSend(Frame);
    }

    DrainEncoder();
    Cleanup();
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
    if (Ret < 0) { LogFFmpegError(Ret, TEXT("av_frame_get_buffer")); return false; }

    // ----- Packet -----
    Packet = av_packet_alloc();
    if (!Packet) { UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] av_packet_alloc failed.")); return false; }

    // ----- swscale: BGRA -> YUV420P -----
    SwsCtx = sws_getContext(
        Width, Height, AV_PIX_FMT_BGRA,
        Width, Height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!SwsCtx) { UE_LOG(LogTemp, Error, TEXT("[RTMPStreamer] sws_getContext failed.")); return false; }

    return true;
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

void FRTMPStreamer::EncodeAndSend(const FQueuedFrame& Frame) {
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

        int WriteRet = av_interleaved_write_frame(FormatCtx, Packet);
        if (WriteRet < 0) {
            LogFFmpegError(WriteRet, TEXT("av_interleaved_write_frame"));
        }

        av_packet_unref(Packet);
    }
}

void FRTMPStreamer::DrainEncoder() {
    if (!CodecCtx || !FormatCtx) { return; }

    // Signal EOF to the encoder
    avcodec_send_frame(CodecCtx, nullptr);

    int Ret = 0;
    while (Ret >= 0) {
        Ret = avcodec_receive_packet(CodecCtx, Packet);
        if (Ret == AVERROR(EAGAIN) || Ret == AVERROR_EOF) { break; }
        if (Ret < 0) { LogFFmpegError(Ret, TEXT("DrainEncoder receive_packet")); break; }

        av_packet_rescale_ts(Packet, CodecCtx->time_base, VideoStream->time_base);
        Packet->stream_index = VideoStream->index;
        av_interleaved_write_frame(FormatCtx, Packet);
        av_packet_unref(Packet);
    }

    av_write_trailer(FormatCtx);
}

void FRTMPStreamer::Cleanup() {
    if (SwsCtx) { sws_freeContext(SwsCtx); SwsCtx = nullptr; }
    if (YUVFrame) { av_frame_free(&YUVFrame); YUVFrame = nullptr; }
    if (Packet) { av_packet_free(&Packet); Packet = nullptr; }
    if (CodecCtx) { avcodec_free_context(&CodecCtx); CodecCtx = nullptr; }

    if (FormatCtx) {
        if (FormatCtx->pb && !(FormatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&FormatCtx->pb);
        }
        avformat_free_context(FormatCtx);
        FormatCtx = nullptr;
    }

    VideoStream = nullptr;
}
