// RTMPStreamComponent.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RTMPStreamer.h"
#include "RTMPStreamComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRTMPStreamStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRTMPStreamStopped, bool, bWasRunning);

/**
 * Actor component that reads a UTextureRenderTarget2D each tick and streams
 * the result over RTMP using FRTMPStreamer (FFmpeg H.264 / FLV).
 *
 * Drop this component onto any Actor, set StreamURL + StreamKey + InputRenderTarget,
 * then call StartStreaming() (or enable bAutoStart).
 */
UCLASS(ClassGroup = (Streaming), meta = (BlueprintSpawnableComponent, DisplayName = "RTMP Stream Component"))
class RTMPSTREAMER_API URTMPStreamComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    URTMPStreamComponent();
    virtual ~URTMPStreamComponent() override;

    // -------------------------------------------------------------------------
    // Configuration properties (set before calling StartStreaming)
    // -------------------------------------------------------------------------

    /** RTMP server URL, e.g. "rtmp://live.twitch.tv/live" or "rtmp://a.rtmp.youtube.com/live2" */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Connection")
    FString StreamURL = TEXT("rtmp://live.twitch.tv/live");

    /** Stream key provided by your streaming service. Combined with StreamURL as "{URL}/{Key}". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Connection")
    FString StreamKey = TEXT("");

    /** The render target to capture and stream. Size is read from the asset. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Input")
    UTextureRenderTarget2D* InputRenderTarget = nullptr;

    /** Target stream frame rate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Encoding", meta = (ClampMin = "1", ClampMax = "120"))
    int32 TargetFPS = 30;

    /** Target video bitrate in kbps (e.g. 4000 for 1080p30). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Encoding", meta = (ClampMin = "500", ClampMax = "40000"))
    int32 BitRateKbps = 4000;

    /** Audio capture mode: No Audio, Mono, or Stereo. Audio is captured from the engine's main audio submix. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Audio")
    ERTMPAudioMode AudioMode = ERTMPAudioMode::None;

    /** Audio sample rate in Hz (ignored when AudioMode is None). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Audio", meta = (ClampMin = "8000", ClampMax = "48000"))
    int32 AudioSampleRate = 48000;

    /** Target audio bitrate in kbps (ignored when AudioMode is None). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Audio", meta = (ClampMin = "32", ClampMax = "320"))
    int32 AudioBitRateKbps = 128;

    /** When true, StartStreaming() is called automatically on BeginPlay. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RTMP|Connection")
    bool bAutoStart = false;

    // -------------------------------------------------------------------------
    // Delegates
    // -------------------------------------------------------------------------
    UPROPERTY(BlueprintAssignable, Category = "RTMP")
    FOnRTMPStreamStarted OnStreamStarted;

    UPROPERTY(BlueprintAssignable, Category = "RTMP")
    FOnRTMPStreamStopped OnStreamStopped;

    // -------------------------------------------------------------------------
    // Blueprint-callable API
    // -------------------------------------------------------------------------

    /** Open the RTMP connection and begin streaming. Returns false if settings are invalid. */
    UFUNCTION(BlueprintCallable, Category = "RTMP Stream")
    bool StartStreaming();

    /** Flush the encoder and close the RTMP connection. */
    UFUNCTION(BlueprintCallable, Category = "RTMP Stream")
    void StopStreaming();

    /** Returns true if the stream is currently active. */
    UFUNCTION(BlueprintPure, Category = "RTMP Stream")
    bool IsStreaming() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    TUniquePtr<FRTMPStreamer> Streamer;

    // Throttle: accumulate time between captures
    float FrameAccumulator = 0.0f;

    // Read pixels from the render target and submit to the streamer
    void CaptureAndSubmit();

    FString BuildFullRTMPUrl() const;
};
