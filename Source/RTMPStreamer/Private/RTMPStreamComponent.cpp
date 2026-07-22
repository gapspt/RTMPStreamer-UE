#include "RTMPStreamComponent.h"
#include "TextureResource.h"

// ---------------------------------------------------------------------------
URTMPStreamComponent::URTMPStreamComponent() {
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false; // enabled only when streaming
}

URTMPStreamComponent::~URTMPStreamComponent() {
    StopStreaming();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void URTMPStreamComponent::BeginPlay() {
    Super::BeginPlay();
    if (bAutoStart) {
        StartStreaming();
    }
}

void URTMPStreamComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
    StopStreaming();
    Super::EndPlay(EndPlayReason);
}

void URTMPStreamComponent::TickComponent(
    float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!Streamer || !Streamer->IsRunning()) {
        return;
    }

    const float FrameInterval = 1.0f / (TargetFPS > 0 ? TargetFPS : 30);
    FrameAccumulator += DeltaTime;

    if (FrameAccumulator >= FrameInterval) {
        FrameAccumulator -= FrameInterval;
        CaptureAndSubmit();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool URTMPStreamComponent::StartStreaming() {
    if (Streamer && Streamer->IsRunning()) {
        UE_LOG(LogTemp, Warning, TEXT("[RTMPStreamComponent] Already streaming."));
        return true;
    }

    if (!InputRenderTarget) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamComponent] InputRenderTarget is null."));
        return false;
    }

    const FString FullUrl = BuildFullRTMPUrl();
    if (FullUrl.IsEmpty()) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamComponent] StreamURL is empty."));
        return false;
    }

    const int32 Width = InputRenderTarget->SizeX;
    const int32 Height = InputRenderTarget->SizeY;
    if (Width <= 0 || Height <= 0) {
        UE_LOG(LogTemp, Error, TEXT("[RTMPStreamComponent] Render target has invalid dimensions (%dx%d)."),
            Width, Height);
        return false;
    }

    Streamer = MakeUnique<FRTMPStreamer>();
    if (!Streamer->Start(FullUrl, Width, Height, TargetFPS, BitRateKbps, AudioMode, AudioSampleRate, AudioBitRateKbps,
        GetWorld())) {
        Streamer.Reset();
        return false;
    }

    FrameAccumulator = 0.0f;
    SetComponentTickEnabled(true);

    OnStreamStarted.Broadcast();
    return true;
}

void URTMPStreamComponent::StopStreaming() {
    SetComponentTickEnabled(false);

    const bool bWasRunning = Streamer && Streamer->IsRunning();
    if (Streamer) {
        Streamer->Stop();
        Streamer.Reset();
    }

    OnStreamStopped.Broadcast(bWasRunning);
}

bool URTMPStreamComponent::IsStreaming() const {
    return Streamer && Streamer->IsRunning();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
FString URTMPStreamComponent::BuildFullRTMPUrl() const {
    FString Base = StreamURL.TrimEnd();
    if (Base.IsEmpty()) return TEXT("");

    if (!StreamKey.IsEmpty())
    {
        // Avoid double slashes
        if (!Base.EndsWith(TEXT("/")))
        {
            Base += TEXT("/");
        }
        Base += StreamKey;
    }
    return Base;
}

void URTMPStreamComponent::CaptureAndSubmit() {
    if (!InputRenderTarget) { return; }

    // ReadPixels is a synchronous GPU readback — the cost is one frame of latency
    // on the render thread.  This is acceptable for live streaming.
    TArray<FColor> Pixels;
    FRenderTarget* RT = InputRenderTarget->GameThread_GetRenderTargetResource();
    if (!RT) { return; }

    // ReadPixels reads BGRA (FColor is B8G8R8A8 on Windows)
    const bool bOK = RT->ReadPixels(Pixels);
    if (!bOK || Pixels.IsEmpty()) { return; }

    // Reinterpret the array as raw bytes — FColor is 4 bytes, packed BGRA
    const int32 Width = InputRenderTarget->SizeX;
    const int32 Height = InputRenderTarget->SizeY;
    const int32 NumBytes = Pixels.Num() * sizeof(FColor);

    TArray<uint8> RawBytes;
    RawBytes.SetNumUninitialized(NumBytes);
    FMemory::Memcpy(RawBytes.GetData(), Pixels.GetData(), NumBytes);

    Streamer->SubmitFrame(RawBytes, Width, Height);
}
