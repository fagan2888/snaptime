%module SnapTime
%include "std_string.i"
%{
        #include "Snap.h"
	#include "SnapTime.hpp"
        #include "stime.hpp"
%}
%include "../../../snap-python/swig/snap.i"
%include "Snap.h"
%include "SnapTime.hpp"
%include "stime.hpp"