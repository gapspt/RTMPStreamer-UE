# RTMPStreamer
Lightweight Unreal Engine plugin that allows RTMP streaming directly from the game.

It uses FFmpeg to encode a UTextureRenderTarget2D and the main sound submix to H.264/AAC FLV and push it to any RTMP endpoint.


## Dependencies
- The [Unreal Engine FFmpeg plugin](https://github.com/gapspt/FFmpeg-UE) must be installed.
