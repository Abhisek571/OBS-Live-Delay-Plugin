#pragma once
#include "flv-muxer.hpp"
// Real 64x64/30 fps x264 AVC configurations; media tags in unit tests remain labels.
namespace active_delay {
inline FlvCodecHeaders programme_test_headers(){return {{1,100,0,10,255,225,0,24,103,100,0,10,172,217,68,38,192,68,0,0,3,0,4,0,0,3,0,240,60,72,150,88,1,0,6,104,235,227,203,34,192,253,248,248,0},{17,144}};}
inline FlvCodecHeaders holding_test_headers(){return {{1,66,192,10,255,225,0,23,103,66,192,10,217,4,38,192,68,0,0,3,0,4,0,0,3,0,240,60,72,153,32,1,0,5,104,203,131,203,32},{17,144}};}
}
