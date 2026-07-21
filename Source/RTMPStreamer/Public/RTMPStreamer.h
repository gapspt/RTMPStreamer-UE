// Pure C++ (no UObject) FFmpeg -> RTMP streaming class.
// Thread-safe: SubmitFrame may be called from any thread.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/CircularQueue.h"
#include "Templates/UniquePtr.h"

// Forward-declare FFmpeg types to avoid including FFmpeg headers in this public
// header (consumers only need the API below).
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * Lightweight RTMP streamer using FFmpeg.
 *
 * Usage:
 *   FRTMPStreamer Streamer;
 *   if (Streamer.Start("rtmp://live.twitch.tv/live/MY_STREAM_KEY", 1920, 1080, 30, 4000))
 *   {
 *       // Each game tick / render-target readback:
 *       Streamer.SubmitFrame(BGRAPixels, Width, Height);
 *   }
 *   Streamer.Stop();
 */
class RTMPSTREAMER_API FRTMPStreamer : public FRunnable {
public:
    FRTMPStreamer();
    virtual ~FRTMPStreamer();

    /**
     * Open the RTMP connection and start the encoder worker thread.
     * @param InRTMPUrl   Full RTMP destination, e.g. "rtmp://live.twitch.tv/live/KEY"
     * @param Width       Frame width  (must match the render target)
     * @param Height      Frame height (must match the render target)
     * @param FPS         Target frames per second
     * @param BitRateKbps Target video bitrate in kbps
     * @return            true if the FFmpeg context was opened successfully
     */
    bool Start(const FString& InRTMPUrl, int32 Width, int32 Height, int32 FPS, int32 BitRateKbps);

    /**
     * Submit a raw BGRA frame (8bpc, packed, row-major, no padding).
     * Safe to call from any thread; frames are queued and consumed by the
     * worker thread.  Frames are silently dropped when the queue is full.
     */
    void SubmitFrame(const TArray<uint8>& BGRAData, int32 Width, int32 Height);

    /** Flush the encoder, write the trailer, and close the connection. Blocks until done. */
    void Stop();

    bool IsRunning() const { return bRunning; }

    // -------------------------------------------------------------------------
    // FRunnable interface (worker thread body)
    // -------------------------------------------------------------------------
    virtual bool   Init() override { return true; }
    virtual uint32 Run()  override;
    virtual void   Stop(bool) { bStopRequested = true; }

private:
    // ---- FFmpeg context ----
    AVFormatContext* FormatCtx = nullptr;
    AVCodecContext* CodecCtx = nullptr;
    AVStream* VideoStream = nullptr;
    SwsContext* SwsCtx = nullptr;
    AVFrame* YUVFrame = nullptr;
    AVPacket* Packet = nullptr;

    // ---- Configuration (set by Start, read by worker) ----
    int32  StreamWidth = 0;
    int32  StreamHeight = 0;
    int32  StreamFPS = 30;

    // Wall-clock time (FPlatformTime::Seconds()) at which streaming started. Used to
    // derive each frame's PTS from its actual capture time rather than assuming a
    // perfectly steady 1/FPS cadence, which would desync the stream's declared
    // timeline from real time whenever the game's tick rate dips below TargetFPS.
    double StreamStartTime = 0.0;
    int64  LastVideoPts = -1;

    // ---- Worker thread ----
    FRunnableThread* WorkerThread = nullptr;
    TAtomic<bool>    bRunning{ false };
    TAtomic<bool>    bStopRequested{ false };

    // ---- Frame queue ----
    struct FQueuedFrame
    {
        TArray<uint8> Data;
        int32 Width = 0;
        int32 Height = 0;
        double CaptureTimeSeconds = 0.0; // FPlatformTime::Seconds() at capture
    };

    // Queue capacity: cap at a few frames to bound memory
    static constexpr int32 QueueCapacity = 8;
    TCircularQueue<FQueuedFrame> FrameQueue{ QueueCapacity };
    FCriticalSection             QueueMutex;
    FEvent* FrameAvailableEvent = nullptr;

    // ---- Internal helpers ----
    bool OpenEncoder(int32 Width, int32 Height, int32 FPS, int32 BitRateKbps);
    bool OpenOutput(const FString& RTMPUrl);
    void EncodeAndSend(const FQueuedFrame& Frame);
    void DrainEncoder();
    void Cleanup();

    static void LogFFmpegError(int ErrCode, const TCHAR* Context);
};
