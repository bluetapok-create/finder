cmake_minimum_required(VERSION 3.16)
project(checker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(checker WIN32
    main.cpp
)

# Link required Windows libraries
target_link_libraries(checker PRIVATE
    gdi32
)

# Disable console window (WIN32 flag already does this)
# Additional linker flags for no subsystem console
if(MSVC)
    set_target_properties(checker PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup"
    )
endif()
