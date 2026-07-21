using UnrealBuildTool;

public class RTMPStreamer : ModuleRules
{
    public RTMPStreamer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "RHI",
            "RenderCore",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "FFmpeg",
        });
    }
}
