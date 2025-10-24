# Developer Documentation - softhddevice-drm-gles

This document contains technical documentation for developers, including the playback state machine and video data flow.

## State Diagram

### Simple Version

```mermaid
stateDiagram-v2
    PrepareContinuePlay: PREPARE CONTINUE PLAY<br/>• Halt ("pause") decoding thread<br/>• Wait filter thread idle<br/>• Cancel filter thread<br />• Continue ("pause", "close") decoder thread<br/>• Resume audio<br/>• Reset trick speed<br/>• Reset cVideoRender pause flag<br/>• Start filter thread lazily

    PrepareNewPlay: PREPARE NEW PLAYBACK<br/>• Start decoding & display thread<br/>• Start filter thread lazily

    PrepareStop: PREPARE STOP<br/>• Halt ("close") decoding thread<br/>• Clear decoding queue<br/>• Cancel filter thread<br/>• Free FFMPEG context<br/>• Flush audio

    PreparePause: PREPARE PAUSE<br/>• Halt ("pause") decoding thread<br/>• Wait filter thread idle<br/>• Cancel filter thread<br />• Pause audio<br/>• Set cVideoRender pause flag

    PrepareStillPicture: PREPARE STILL PICTURE<br/>• Audio pause<br/>• cVideoStream StillPicture() logic<br/>• Audio clear/resume

    [*] --> Stop: Initialize

    PrepareStop --> Stop
    Stop --> PrepareNewPlay

    PrepareNewPlay --> Play
    PrepareContinuePlay --> Play
    Play --> PreparePause: Freeze()
    Play --> TrickSpeed: TrickSpeed()
    Play --> PrepareStop: PlayMode(pmNone)
    Play --> PrepareStillPicture: StillPicture()

    PreparePause --> Pause
    Pause --> PrepareContinuePlay: Play()
    Pause --> PrepareStop: PlayMode(pmNone)
    Pause --> TrickSpeed: TrickSpeed()
    Pause --> PrepareStillPicture: StillPicture()

    TrickSpeed --> PrepareContinuePlay: Play()
    TrickSpeed --> PreparePause: Freeze()
    TrickSpeed --> PrepareStop: PlayMode(pmNone)
    TrickSpeed --> PrepareStillPicture: StillPicture()

    PrepareStillPicture --> StillPicture
    StillPicture --> PrepareContinuePlay: Play()
    StillPicture --> PrepareStop: PlayMode(pmNone)
    StillPicture --> TrickSpeed: TrickSpeed()

    classDef stopState fill:#e57373,stroke:#d32f2f,stroke-width:2px,color:#000
    classDef playState fill:#81c784,stroke:#388e3c,stroke-width:2px,color:#000
    classDef pauseState fill:#fff59d,stroke:#fbc02d,stroke-width:2px,color:#000
    classDef trickspeedState fill:#64b5f6,stroke:#1976d2,stroke-width:2px,color:#000
    classDef stillpictureState fill:#ba68c8,stroke:#7b1fa2,stroke-width:2px,color:#000

    class Stop stopState
    class Play playState
    class Pause pauseState
    class TrickSpeed trickspeedState
    class StillPicture stillpictureState
```

### Fat Version

```mermaid
stateDiagram-v2
    PrepareContinuePlay: PREPARE CONTINUE PLAY<br/>→ Play()<br/>• Halt ("pause") decoding thread<br/>• Wait filter thread idle<br/>• Cancel filter thread<br />• Continue ("pause", "close") decoder thread<br/>• Resume audio<br/>• Reset trick speed<br/>• Reset cVideoRender pause flag<br/>• Start filter thread lazily

    PrepareNewPlay: PREPARE NEW PLAYBACK<br/>→ SetPlayMode()<br/>• Start decoding & display thread<br/>• Start filter thread lazily

    PrepareStop: PREPARE STOP<br/>→ PlayMode(pmNone)<br/>• Halt ("close") decoding thread<br/>• Cancel filter thread<br/>• Clear decoding queue<br/>• Free FFMPEG context<br/>• Flush audio

    PreparePause: PREPARE PAUSE<br/>→ Freeze()<br/>• Halt ("pause") decoding thread<br/>• Wait filter thread idle<br/>• Cancel filter thread<br />• Pause audio<br/>• Set cVideoRender pause flag

    PrepareStillPicture: PREPARE STILL PICTURE<br/>• Audio pause<br/>• cVideoStream StillPicture() logic<br/>• Audio clear/resume

    FastTrickSpeed: Fast Reverse/Forward

    [*] --> Stop: Initialize

    PrepareStop --> Stop
    Stop --> PrepareNewPlay

    PrepareNewPlay --> Play
    PrepareContinuePlay --> Play
    Play --> PreparePause
    Play --> FastTrickSpeed: Clear()
    Play --> PrepareStop
    Play --> Play: SkipSeconds → Clear()
    Play --> PrepareStillPicture: Clear()

    PreparePause --> Pause
    Pause --> PrepareContinuePlay
    Pause --> PrepareStop
    Pause --> SlowForward
    Pause --> SlowReverse
    Pause --> PrepareContinuePlay: SkipSeconds → Clear()
    Pause --> PrepareStillPicture: Clear()

    FastTrickSpeed --> PrepareContinuePlay: Clear()
    FastTrickSpeed --> PreparePause: Clear()
    FastTrickSpeed --> PrepareStop
    FastTrickSpeed --> PrepareContinuePlay: SkipSeconds → 2x Clear()
    FastTrickSpeed --> PrepareStillPicture: Clear()

    SlowForward --> PrepareContinuePlay: [no clear]
    SlowForward --> PreparePause: [no clear]
    SlowForward --> PrepareStop
    SlowForward --> SlowReverse: Clear()
    SlowForward --> PrepareContinuePlay: Clear()
    SlowForward --> PrepareStillPicture: Clear()

    SlowReverse --> PrepareContinuePlay: Clear()
    SlowReverse --> PreparePause: Clear()
    SlowReverse --> PrepareStop
    SlowReverse --> SlowForward: Clear()
    SlowReverse --> PrepareContinuePlay: 1-2x Clear()
    SlowReverse --> PrepareStillPicture: Clear()

    PrepareStillPicture --> StillPicture
    StillPicture --> PrepareContinuePlay: Clear()
    StillPicture --> PrepareStop
    StillPicture --> SlowForward: [no clear]
    StillPicture --> SlowReverse: Clear()

    classDef stopState fill:#e57373,stroke:#d32f2f,stroke-width:2px,color:#000
    classDef playState fill:#81c784,stroke:#388e3c,stroke-width:2px,color:#000
    classDef pauseState fill:#fff59d,stroke:#fbc02d,stroke-width:2px,color:#000
    classDef fastTrickspeedState fill:#64b5f6,stroke:#1976d2,stroke-width:2px,color:#000
        classDef slowForwardState fill:#4dd0e1,stroke:#0097a7,stroke-width:2px,color:#000
    classDef slowReverseState fill:#ffb74d,stroke:#f57c00,stroke-width:2px,color:#000
    classDef stillpictureState fill:#ba68c8,stroke:#7b1fa2,stroke-width:2px,color:#000

    class Stop stopState
    class Play playState
    class Pause pauseState
    class FastTrickSpeed fastTrickspeedState
    class SlowForward slowForwardState
    class SlowReverse slowReverseState
    class StillPicture stillpictureState
```

## Video Data Flow Call Graph

This section shows the complete data flow of video frames from VDR through the plugin to the display hardware.

## Overview

The video pipeline consists of 4 main threads:

1. 🔵**VDR Thread** - Receives video data from VDR
2. 🟣**cDecodingThread** - Decodes video packets using FFmpeg
3. 🟠**cFilterThread** - Applies filters (deinterlacing, scaling)
4. 🟢**cDisplayThread** - Syncs with audio and commits frames to DRM/KMS

- 🔴 Hardware (DRM/KMS display hardware)
- 🟡 Buffers (frame buffers and queues)

## Detailed Call Graph

```mermaid
graph TD
    %% VDR Thread
    VDR[VDR Core] --> |PES Packet|PlayVideo["cSoftHdDevice::PlayVideo"]
    PlayVideo --> |PES Packet|PushPes["cVideoStream::PushPesPacket"]
    PushPes --> |Reassambled Codec Packet|CreatePkt["CreateAvPacket"]
    CreatePkt --> |AVPacket|PktQueue["cVideoStream::m_packets"]

    %% Decoding Thread
    PktQueue --> DecThread["cDecodingThread::Action"]
    DecThread --> DecInput["cVideoStream::DecodeInput"]
    DecInput --> |AVPacket|SendPkt["cVideoDecoder::SendPacket"]
    SendPkt --> |MPEG2/H.264/HEVC Decoding|RecvFrame["cVideoDecoder::ReceiveFrame"]
    RecvFrame --> |AVFrame|DecInput
    DecInput --> |AVFrame|RenderFrame["cVideoRender::RenderFrame"]

    %% Filter Thread Decision
    RenderFrame --> |AVFrame|FormatCheck{Frame format?}
    FormatCheck -->|YUV420P or<br/>interlaced DRM_PRIME| FilterPush["cFilterThread::PushFrame"]
    FormatCheck -->|Progressive DRM_PRIME| RenderRB["cVideoRender::m_framesRb"]
    FormatCheck -->|NV12 Format| EnqFB["cVideoRender::EnqueueFB"]

    %% Filter Thread Path
    FilterPush --> FilterQueue["cFilterThread::m_frames"]
    FilterQueue --> FilterAction["cFilterThread::Action"]
    FilterAction --> |Interlaced DRM_PRIME|HWDeint["FFMPEG filter:<br />HW Deinterlacer"]
    FilterAction --> |YUV420P|SWDeint["FFMPEG filter:<br/>Convert to NV12 with optional SW Deinterlacing"]
    HWDeint -->|Progressive DRM_PRIME| RenderRB["cVideoRender::m_framesRb"]
    SWDeint --> |NV12 Format|EnqFB["cVideoRender::EnqueueFB"]

    %% EnqueueFB Path (Software frames need buffer prep)
    EnqFB --> |Progressive DRM_PRIME|RenderRB

    %% Display Thread
    RenderRB --> |AVFrame|DispThread["cDisplayThread::Action"]
    DispThread --> DisplayFrame["cVideoRender::DisplayFrame<br/>A/V sync"]
    DisplayFrame --> GetFrame["cVideoRender::GetFrame"]
    GetFrame -->|AVFrame| DisplayFrame

    DisplayFrame --> |AVFrame|GetBuffer["cVideoRender::GetBuffer"]
    GetBuffer -->|cDrmBuffer| DisplayFrame

    DisplayFrame -->|AVFrame| SetFrame["cDrmBuffer::SetFrame"]
    SetFrame --> DisplayFrame
    DisplayFrame --> |AVFrame & cDrmBuffer|PageFlipVid["cVideoRender::PageFlipVideo"]

    PageFlipVid --> |AVFrame & cDrmBuffer|PageFlip["PageFlip"]
    PageFlip --> |cDrmBuffer|CommitBuf["CommitBuffer"]

    CommitBuf --> |cDrmDevice|DrmCommit["drmModeAtomicCommit"]

    DrmCommit --> Hardware["Display Hardware"]

    %% Styling
    classDef vdrThread fill:#90caf9,stroke:#1976d2,stroke-width:2px,color:#000000
    classDef decThread fill:#ce93d8,stroke:#8e24aa,stroke-width:2px,color:#000000
    classDef filterThread fill:#ffb74d,stroke:#f57c00,stroke-width:2px,color:#000000
    classDef displayThread fill:#81c784,stroke:#388e3c,stroke-width:2px,color:#000000
    classDef hardware fill:#e57373,stroke:#d32f2f,stroke-width:3px,color:#000000
    classDef buffer fill:#fff59d,stroke:#fbc02d,stroke-width:2px,color:#000000

    class VDR,PlayVideo,PushPes,CreatePkt vdrThread
    class PktQueue,DecThread,DecInput,SendPkt,RecvFrame,RenderFrame,FilterPush decThread
    class FilterQueue,FilterAction,HWDeint,SWDeint filterThread
    class DispThread,DisplayFrame,GetFrame,PageFlipVid,PageFlip,CommitBuf,DrmCommit,GetBuffer,SetFrame displayThread
    class Hardware hardware
    class RenderRB,EnqFB buffer
    class FormatCheck decThread
```
