# Video Data Flow Call Graph - softhddevice-drm-gles

This document shows the complete data flow of video frames from VDR through the plugin to the display hardware.

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
