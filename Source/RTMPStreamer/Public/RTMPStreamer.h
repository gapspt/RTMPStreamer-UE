// Pure C++ (no UObject) FFmpeg -> RTMP streaming class.
// Thread-safe: SubmitFrame may be called from any thread.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/CircularQueue.h"
#include "Templates/UniquePtr.h"
#include "Templates/SharedPointer.h"
#include "ISubmixBufferListener.h"

// Forward-declare FFmpeg types to avoid including FFmpeg headers in this public
// header (consumers only need the API below).
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;
struct AVAudioFifo;
class USoundSubmix;

/** Audio capture/encoding mode for the outgoing RTMP stream. */
UENUM(BlueprintType)
enum class ERTMPAudioMode : uint8 {
    None    UMETA(DisplayName = "No Audio"),
    Mono    UMETA(DisplayName = "Mono"),
    Stereo  UMETA(DisplayName = "Stereo"),
};

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
class RTMPSTREAMER_API FRTMPStreamer {
public:
    FRTMPStreamer();
    virtual ~FRTMPStreamer();

    /**
     * Open the RTMP connection and start the encoder worker thread.
     * @param InRTMPUrl       Full RTMP destination, e.g. "rtmp://live.twitch.tv/live/KEY"
     * @param Width           Frame width  (must match the render target)
     * @param Height          Frame height (must match the render target)
     * @param FPS             Target frames per second
     * @param BitRateKbps     Target video bitrate in kbps
     * @param AudioMode       No Audio / Mono / Stereo. When not None, audio is captured from
     *                        the engine's main audio submix and encoded as AAC.
     * @param AudioSampleRate Target audio sample rate in Hz (ignored when AudioMode is None).
     * @param AudioBitRateKbps Target audio bitrate in kbps (ignored when AudioMode is None).
     * @return            true if the FFmpeg context was opened successfully
     */
    bool Start(const FString& InRTMPUrl, int32 Width, int32 Height, int32 FPS, int32 BitRateKbps,
        ERTMPAudioMode AudioMode = ERTMPAudioMode::None, int32 AudioSampleRate = 48000, int32 AudioBitRateKbps = 128,
        UWorld* InWorld = nullptr);

    /**
     * Submit a raw BGRA frame (8bpc, packed, row-major, no padding).
     * Safe to call from any thread; frames are queued and consumed by the
     * worker thread.  Frames are silently dropped when the queue is full.
     */
    void SubmitFrame(const TArray<uint8>& BGRAData, int32 Width, int32 Height);

    /** Flush the encoder, write the trailer, and close the connection. Blocks until done. */
    void Stop();

    bool IsRunning() const { return bRunning; }

private:
    // ---- FFmpeg context (video) ----
    AVFormatContext* FormatCtx = nullptr;
    AVCodecContext* CodecCtx = nullptr;
    AVStream* VideoStream = nullptr;
    SwsContext* SwsCtx = nullptr;
    AVFrame* YUVFrame = nullptr;
    AVPacket* Packet = nullptr;

    // ---- FFmpeg context (audio) ----
    AVCodecContext* AudioCodecCtx = nullptr;
    AVStream* AudioStream = nullptr;
    SwrContext* SwrCtx = nullptr;
    AVFrame* AudioFrame = nullptr;
    AVPacket* AudioPacket = nullptr;
    AVAudioFifo* AudioFifo = nullptr;

    // Preallocated scratch buffer for swr_convert output (planar float), reused
    // across calls to avoid a heap alloc/free on every audio buffer.
    uint8** AudioConvertedData = nullptr;
    int32   AudioConvertedLineSize = 0;
    int32   AudioConvertedCapacitySamples = 0;

    // Guards all writes into FormatCtx (av_interleaved_write_frame / av_write_trailer),
    // since the video worker thread and the audio worker thread both write to it.
    FCriticalSection MuxerMutex;

    // ---- Configuration (set by Start, read by worker) ----
    int32  StreamWidth = 0;
    int32  StreamHeight = 0;
    int32  StreamFPS = 30;

    ERTMPAudioMode AudioMode = ERTMPAudioMode::None;
    int32  AudioChannels = 0;
    int32  AudioTargetSampleRate = 48000;
    int32  SwrInChannels = 0;
    int32  SwrInSampleRate = 0;

    // Wall-clock time (FPlatformTime::Seconds()) at which streaming started. Used to
    // derive each frame's PTS from its actual capture time rather than assuming a
    // perfectly steady 1/FPS cadence, which would desync the stream's declared
    // timeline from real time whenever the game's tick rate dips below TargetFPS.
    double StreamStartTime = 0.0;
    int64  LastVideoPts = -1;

    // The world whose audio device should be captured. PIE worlds (and some
    // standalone configurations) each have their own FAudioDevice instance, so we
    // must capture from the world that's actually playing the sound rather than
    // assuming GEngine's main audio device, which may be a different instance.
    TWeakObjectPtr<UWorld> CaptureWorld;

    TAtomic<bool>    bRunning{ false };
    TAtomic<bool>    bStopRequested{ false };

    struct FQueuedFrame
    {
        TArray<uint8> Data;
        int32 Width = 0;
        int32 Height = 0;
        double CaptureTimeSeconds = 0.0; // FPlatformTime::Seconds() at capture
    };

    struct FQueuedAudioChunk
    {
        TArray<float> Data;
        int32 NumChannels = 0;
        int32 SampleRate = 0;
        int32 NumFrames = 0;
        double CaptureTimeSeconds = 0.0; // FPlatformTime::Seconds() at capture
    };

    // Queue capacity: cap at a few frames to bound memory
    static constexpr int32 QueueCapacity = 8;
    TCircularQueue<FQueuedFrame> FrameQueue{ QueueCapacity };
    FCriticalSection             QueueMutex;
    FEvent* FrameAvailableEvent = nullptr;

    FEvent* AudioDataEvent = nullptr;

    // ---- Audio submix capture ----
    // ISubmixBufferListener already derives from TSharedFromThis<ISubmixBufferListener, ESPMode::ThreadSafe>.
    class FSubmixListener : public ISubmixBufferListener
    {
    public:
        TFunction<void(const float* AudioData, int32 NumSamples, int32 NumChannels, int32 SampleRate, double AudioClock)> OnBuffer;
        virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples,
            int32 NumChannels, const int32 SampleRate, double AudioClock) override;
    };
    TSharedPtr<FSubmixListener, ESPMode::ThreadSafe> AudioListener;
    bool bAudioListenerRegistered = false;

    static constexpr int32 AudioQueueCapacity = 32;
    TCircularQueue<FQueuedAudioChunk> AudioQueue{ AudioQueueCapacity };
    FCriticalSection AudioQueueMutex;

    // Video worker thread
    class FVideoRunnable : public FRunnable
    {
    public:
        explicit FVideoRunnable(FRTMPStreamer* InOwner) : Owner(InOwner) {}
        virtual uint32 Run() override { return Owner->RunVideoThread(); }
    private:
        FRTMPStreamer* Owner;
    };
    TUniquePtr<FVideoRunnable> VideoRunnable;
    FRunnableThread* VideoWorkerThread = nullptr;

    // Dedicated audio worker thread: keeps audio resampling/encoding off the video
    // worker thread so a slow audio path can never starve video encoding.
    class FAudioRunnable : public FRunnable
    {
    public:
        explicit FAudioRunnable(FRTMPStreamer* InOwner) : Owner(InOwner) {}
        virtual uint32 Run() override { return Owner->RunAudioThread(); }
    private:
        FRTMPStreamer* Owner;
    };
    TUniquePtr<FAudioRunnable> AudioRunnable;
    FRunnableThread* AudioWorkerThread = nullptr;

    // ---- Internal helpers ----
    bool OpenEncoder(int32 Width, int32 Height, int32 FPS, int32 BitRateKbps);
    bool OpenAudioEncoder(int32 AudioSampleRate, int32 AudioBitRateKbps);
    bool OpenOutput(const FString& RTMPUrl);
    void ProcessVideoSamples(const FQueuedFrame& Frame);
    bool EnsureResampler(int32 InChannels, int32 InSampleRate);
    bool EnsureConvertedBuffer(int32 RequiredSamplesPerChannel);
    void ProcessAudioSamples(const FQueuedAudioChunk& Chunk);
    void SubmitAudioSamples(const float* Data, int32 NumSamples, int32 NumChannels, int32 SampleRate, double AudioClock);
    void FlushAudioEncoder();
    uint32 RunVideoThread();
    uint32 RunAudioThread();
    void RegisterAudioCapture();
    void UnregisterAudioCapture();
    void DrainEncoder();
    void Cleanup();

    static void LogFFmpegError(int ErrCode, const TCHAR* Context);
};
