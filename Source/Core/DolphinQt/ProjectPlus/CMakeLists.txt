# Enable Qt automatic meta-object code generation
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Add ProjectPlus sources to dolphin-qt target
target_sources(project-plus-dolphin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/InstallUpdateDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/InstallUpdateDialog.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UpdateDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UpdateDialog.h
    ${CMAKE_CURRENT_SOURCE_DIR}/DownloadWorker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DownloadWorker.h
)
