# Patch applied to libavoid (adaptagrams, LGPL-2.1+) when GRAPHER fetches it. This file is the complete change.
#
# vertices.h defines two `static const VertID` objects that every file including the router headers constructs, but
# class VertID is not marked AVOID_EXPORT like the library's other public classes, so a program linking libavoid as a DLL
# cannot reach VertID's constructor or VertID::PROP_OrthShapeEdge. Marking the class exported fixes that; nothing else
# changes. Idempotent.
#
# Usage: cmake -DSRC=<adaptagrams source dir> -P export-vertid.cmake

set(header "${SRC}/cola/libavoid/vertices.h")
file(READ "${header}" text)
string(FIND "${text}" "class AVOID_EXPORT VertID" already)
if(already EQUAL -1)
    string(REGEX REPLACE "(\n)class VertID(\r?\n)" "\\1class AVOID_EXPORT VertID\\2" patched "${text}")
    if(patched STREQUAL text)
        message(FATAL_ERROR "export-vertid: 'class VertID' not found in ${header}; the pinned adaptagrams changed")
    endif()
    file(WRITE "${header}" "${patched}")
endif()
