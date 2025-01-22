# GTK4 Media Player

## Overview

GTK4 Media Player is a simple media player built with **GTK4**, **FFmpeg**, and **PulseAudio**. It supports simultaneous video and audio playback from a single media file, demonstrating multithreading and circular buffer implementation for efficient processing.

## Features

- **Video Playback**: Displays video frames using GTK4's `GdkPixbuf`.
- **Audio Playback**: Decodes and plays audio using FFmpeg and PulseAudio.
- **Multithreading**: Handles video and audio decoding concurrently using threads.
- **Circular Buffers**: Efficiently manages video frames and audio samples.
- **Cross-Platform**: Works on Linux (e.g., Ubuntu) and compatible with WSL.

## Requirements

- **GTK4**: For GUI rendering.
- **FFmpeg**: For media decoding.
- **PulseAudio**: For audio playback.
- **GCC or Clang**: For compiling the source code.

## Installation

1. **Install Dependencies**:
   ```bash
   sudo apt update
   sudo apt install gcc pkg-config libgtk-4-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libpulse-dev
   ```

2. **Clone the Repository**:
   ```bash
   git clone https://github.com/gfkaceli/GTK4MediaPlayer.git
   cd GTK4MediaPlayer
   ```

3. **Compile the Program**:
   ```bash
   gcc mediaplayer.c -o mediaplayer \
   `pkg-config --cflags --libs gtk4 libavformat libavcodec libavutil libswscale libswresample` \
   -lpulse-simple -lpulse -pthread
   ```

4. **Run the Program**:
   ```bash
   ./mediaplayer <media_file> <frame_rate>
   ```
   Example:
   ```bash
   ./mediaplayer sample.mp4 30
   ```

## How It Works

- **Video Decoding**:
  - Frames are decoded from the video stream using FFmpeg.
  - Frames are converted to RGB format and stored in a circular buffer for display.
  - GTK4 displays frames using `GdkPixbuf`.

- **Audio Decoding**:
  - Audio packets are decoded and resampled to 44.1 kHz, stereo, 16-bit PCM.
  - Audio samples are played using PulseAudio.

- **Multithreading**:
  - Video and audio are handled in separate threads to ensure smooth playback.
  - Circular buffers synchronize the producer (decoder) and consumer (player).

## Project Structure

```plaintext
.
├── mediaplayer.c   # Main source file
├── README.md       # Documentation
```

## Known Issues

- **WSL Graphics**: Ensure WSLg is enabled for GPU rendering when using WSL.
- **Format Support**: Limited to formats supported by FFmpeg.

