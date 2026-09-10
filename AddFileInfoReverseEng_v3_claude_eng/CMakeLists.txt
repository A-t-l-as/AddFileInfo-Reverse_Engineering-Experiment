cmake_minimum_required(VERSION 3.16)

project(AddFileInfoReverseEng_v3_claude_eng LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(AddFileInfoReverseEng_v3_claude_eng main.cpp)

include(GNUInstallDirs)
install(TARGETS AddFileInfoReverseEng_v3_claude_eng
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
