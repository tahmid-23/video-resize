cmake_minimum_required(VERSION 3.26)
project(video_resize)

add_executable(video_resize main.c)

target_link_libraries(video_resize avcodec avformat avutil)
