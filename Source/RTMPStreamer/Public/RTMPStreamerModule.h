#pragma once

#include "Modules/ModuleManager.h"

class FRTMPStreamerModule : public IModuleInterface {
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void* DLL_avutil = nullptr;
    void* DLL_swresample = nullptr;
    void* DLL_avcodec = nullptr;
    void* DLL_swscale = nullptr;
    void* DLL_avformat = nullptr;
};
