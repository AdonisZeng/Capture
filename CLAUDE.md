# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Debug build
cd build && cmake .. -G "MinGW Makefiles" && mingw32-make

# Release build
cd build && cmake .. -G "MinGW Makefiles" && mingw32-make -j4

# Clean rebuild
cd build && rm -rf * && cmake .. -G "MinGW Makefiles" && mingw32-make
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      Qt6 GUI Layer                       │
│  MainWindow │ TrayIconManager │ WindowPicker │ RegionSelector │
├─────────────────────────────────────────────────────────┤
│                   Control Layer                          │
│  CaptureController │ RecordingController │ HotkeyManager │
├─────────────────────────────────────────────────────────┤
│                MediaFoundation Layer                     │
│  VideoCaptureDevice  │  AudioCaptureDevice               │
│  (DXGI Duplication)  │  (WASAPI Loopback)                │
├─────────────────────────────────────────────────────────┤
│                 Encoder / Writer Layer                   │
│  EncoderFactory (H.264/H.265) │ Mp4Writer (IMFMediaSink)  │
└─────────────────────────────────────────────────────────┘
```

## Key Components

**MainWindow**: 主界面，包含录制和截屏两个标签页，通过 CaptureController 处理截图，RecordingController 处理录制

**RecordingController**: 录制控制器，整合视频捕获、音频捕获和MP4写入，管理整个录制流程

**VideoCaptureDevice**: 视频捕获，使用 DXGI Output Duplication API 实现屏幕/窗口捕获，实现 IMFSourceReaderCallback 接口

**AudioCaptureDevice**: 音频捕获，使用 WASAPI 实现系统音频（Loopback）和麦克风混音捕获

**EncoderFactory**: 编码器工厂，创建 H.264/H.265 视频编码器和 AAC 音频编码器

**Mp4Writer**: MP4 写入器，使用 IMFMediaSink 将编码后的音视频数据写入 MP4 文件

## Dependencies

- Qt6: Core, GUI, Widgets, Multimedia
- Windows SDK: MediaFoundation, DXGI, Direct3D11, WASAPI
- Platform libs: dwmapi, dxgi, d3d11, mf.lib, mfplat.lib, mfreadwrite.lib