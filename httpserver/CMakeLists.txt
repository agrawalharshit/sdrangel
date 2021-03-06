project(httpserver)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(httpserver_SOURCES
   httpglobal.cpp
   httplistener.cpp
   httpconnectionhandler.cpp
   httpconnectionhandlerpool.cpp
   httprequest.cpp
   httpresponse.cpp
   httpcookie.cpp
   httprequesthandler.cpp
   httpsession.cpp
   httpsessionstore.cpp
   staticfilecontroller.cpp
)

set(httpserver_HEADERS
   httpglobal.h
   httplistener.h
   httpconnectionhandler.h
   httpconnectionhandlerpool.h
   httprequest.h
   httpresponse.h
   httpcookie.h
   httprequesthandler.h
   httpsession.h
   httpsessionstore.h
   staticfilecontroller.h
   httplistenersettings.h
   httpdocrootsettings.h
   httpsessionssettings.h
)

include_directories(
	.
    ${CMAKE_SOURCE_DIR}/exports
	${CMAKE_CURRENT_BINARY_DIR}
)

#include(${QT_USE_FILE})
add_definitions(${QT_DEFINITIONS})
add_definitions(-DQT_SHARED)

add_library(httpserver SHARED
	${httpserver_SOURCES}
	${httpserver_HEADERS_MOC}
)

set_target_properties(httpserver PROPERTIES DEFINE_SYMBOL "httpserver_EXPORTS")

target_link_libraries(httpserver
	${QT_LIBRARIES}
)

target_link_libraries(httpserver Qt5::Core Qt5::Network)

install(TARGETS httpserver DESTINATION lib)
