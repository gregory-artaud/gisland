#define RLGL_IMPLEMENTATION
#include <rlgl.h>

bool gisland_rlgl_has_error() {
  bool found = false;
  while (glGetError() != GL_NO_ERROR) {
    found = true;
  }
  return found;
}

void gisland_rlgl_clear_errors() {
  while (glGetError() != GL_NO_ERROR) {
  }
}

bool gisland_rlgl_texture_exists(unsigned int id) { return glIsTexture(id) == GL_TRUE; }

bool gisland_rlgl_texture_filter_matches(unsigned int id, bool linear) {
  GLint previous = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
  glBindTexture(GL_TEXTURE_2D, id);
  GLint minimum = 0;
  GLint magnification = 0;
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minimum);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magnification);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
  const GLint expected = linear ? GL_LINEAR : GL_NEAREST;
  return minimum == expected && magnification == expected;
}

bool gisland_rlgl_framebuffer_exists(unsigned int id) { return glIsFramebuffer(id) == GL_TRUE; }

bool gisland_rlgl_program_exists(unsigned int id) { return glIsProgram(id) == GL_TRUE; }

void gisland_rlgl_read_default_rgba8(int width, int height, unsigned char *pixels) {
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

void gisland_rlgl_blit_to_default(unsigned int framebuffer, int width, int height) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool gisland_rlgl_marker_matches(unsigned int id, const unsigned char *expected) {
  if (id == 0 || glIsTexture(id) != GL_TRUE) {
    return false;
  }
  GLint previous = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
  glBindTexture(GL_TEXTURE_2D, id);
  GLint width = 0;
  GLint height = 0;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
  unsigned char pixel[4]{};
  if (width == 1 && height == 1) {
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  }
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
  return width == 1 && height == 1 && pixel[0] == expected[0] && pixel[1] == expected[1] &&
         pixel[2] == expected[2] && pixel[3] == expected[3];
}
