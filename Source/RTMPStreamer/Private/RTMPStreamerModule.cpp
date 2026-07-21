#include "RTMPStreamerModule.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FRTMPStreamerModule"

static void* SafeLoadDLL(const FString& DLLName) {
    void* Handle = FPlatformProcess::GetDllHandle(*DLLName);
    if (!Handle) {
        UE_LOG(LogTemp, Warning, TEXT("[RTMPStreamer] Could not load DLL: %s"), *DLLName);
    }
    return Handle;
}

static void SafeFreeDLL(void*& H) {
    if (H) {
        FPlatformProcess::FreeDllHandle(H); H = nullptr;
    }
}

void FRTMPStreamerModule::StartupModule() {
#if PLATFORM_WINDOWS
    // Load order matters: dependencies first
    DLL_avutil = SafeLoadDLL(TEXT("avutil-60.dll"));
    DLL_swresample = SafeLoadDLL(TEXT("swresample-6.dll"));
    DLL_avcodec = SafeLoadDLL(TEXT("avcodec-62.dll"));
    DLL_swscale = SafeLoadDLL(TEXT("swscale-9.dll"));
    DLL_avformat = SafeLoadDLL(TEXT("avformat-62.dll"));
#endif

    UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Module started."));
}

void FRTMPStreamerModule::ShutdownModule() {
    // Free in reverse order
    SafeFreeDLL(DLL_avformat);
    SafeFreeDLL(DLL_swscale);
    SafeFreeDLL(DLL_avcodec);
    SafeFreeDLL(DLL_swresample);
    SafeFreeDLL(DLL_avutil);

    UE_LOG(LogTemp, Log, TEXT("[RTMPStreamer] Module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRTMPStreamerModule, RTMPStreamer)
