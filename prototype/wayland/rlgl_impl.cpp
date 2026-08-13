#define RLGL_IMPLEMENTATION
#include <rlgl.h>

bool gisland_rlgl_has_error() { return glGetError() != GL_NO_ERROR; }
