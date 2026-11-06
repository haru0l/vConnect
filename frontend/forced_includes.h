/*
 * forced_includes.h — headers the 2011 vConnect sources assumed were pulled in
 * transitively by their toolchain (MSVC and older libstdc++ both provided
 * these incidentally). Forced via -include so the vendored sources stay
 * byte-identical to upstream.
 */
#ifndef VCONNECT_FORCED_INCLUDES_H
#define VCONNECT_FORCED_INCLUDES_H

#include <climits>   // PATH_MAX
#include <cstring>   // memset et al.
#include <cstdint>   // uint32_t et al.
#include <cstdio>    // snprintf
#include <algorithm> // std::for_each, std::sort
#include <cstdlib>   // atoi, atof

#endif
