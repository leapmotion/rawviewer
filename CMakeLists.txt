cmake_minimum_required(VERSION 2.8)

project(LeapViewer)

add_executable(LeapViewer v4l2sdl.c)

target_link_libraries(LeapViewer SDL2 pthread)

#install (TARGETS LeapViewer DESTINATION bin)

