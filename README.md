# video-resize

A C program that currently converts a video file to an MP4 file with HEVC encoding using libavcodec.

## Usage

`cmake --build cmake-build-debug --target video_resize` to build the executable. `libavcodec`, `libavformat`, and `libavutil` must be in your shared library directory.

`./cmake-build-debug/video_resize` will start the program. It will prompt you for an input and output file.