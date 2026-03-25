cmake_minimum_required(VERSION 3.10)
project(temp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Tell CLion where to look for Mac Homebrew installations
# (Covers both Apple Silicon /opt/homebrew and Intel /usr/local)
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local")

# Find the OpenCV package
find_package(OpenCV REQUIRED)

# Create the executable (Ensure your C++ file is actually named main.cpp!)
add_executable(temp cv_etch_a_sketch.cpp)

# Link OpenCV to your
target_include_directories(temp PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(temp PRIVATE ${OpenCV_LIBS})
