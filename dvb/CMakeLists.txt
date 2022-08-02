project(ommdvb)
cmake_minimum_required(VERSION 3.16)

set(CMAKE_MODULE_PATH
${CMAKE_CURRENT_SOURCE_DIR}
${CMAKE_MODULE_PATH}
)

SET(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -Wno-format-security")

find_package(LinuxDVB)

if(LINUXDVB_FOUND)
message(STATUS "linux DVB headers found in " ${LINUXDVB_INCLUDE_DIR})

include_directories(
${LINUXDVB_INCLUDE_DIR}
)

find_package(POCO REQUIRED)
include_directories(
${POCO_INCLUDE_DIRS}
)

find_package(UDEV)
include_directories(
${UDEV_INCLUDE_DIRS}
)

include(AddResources)

add_resources(TransponderData
"transponder.zip"
)

set(OMM_DVB_LIB_SRCS
dvb.cpp
Sys.cpp
AvStream.cpp
Descriptor.cpp
Device.cpp
Log.cpp
Section.cpp
Stream.cpp
Service.cpp
Transponder.cpp
Frontend.cpp
Demux.cpp
#Mux.cpp
Remux.cpp
Dvr.cpp
TransportStream.cpp
ElementaryStream.cpp
)

add_library(ommdvb SHARED
${OMM_DVB_LIB_SRCS}
${TransponderData_CPP}
)

set(OMM_DVB
ommdvb
CACHE INTERNAL libommdvb
)

target_link_libraries(ommdvb
${UDEV_LIBRARIES}
${POCO_FOUNDATION}
${POCO_XML}
${POCO_ZIP}
${OMM_SYS}
${OMM_AVSTREAM}
)

install(TARGETS ommdvb
LIBRARY DESTINATION lib
ARCHIVE DESTINATION lib
)

#add_executable(scandvb
#ScanDvb.cpp
#)
#
#target_link_libraries(scandvb
#${OMM_DVB}
#)
#
#
#add_executable(tunedvb
#TuneDvb.cpp
#)
#
#target_link_libraries(tunedvb
#${OMM_UTIL}
#${OMM_DVB}
#)
#
#install(TARGETS scandvb tunedvb
#RUNTIME DESTINATION bin
#LIBRARY DESTINATION lib
#)

else(LINUXDVB_FOUND)
message(STATUS "linux DVB headers not found")
endif(LINUXDVB_FOUND)
