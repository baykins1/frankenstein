cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ot_cli)
add_compile_definitions(CONFIG_OPENTHREAD_TASK_STACK_SIZE=8192)