#ifndef COMPASS3D_H
#define COMPASS3D_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call after SDL video initialization and after creating/making current an
 * SDL OpenGL ES 2.0 context. Returns 1 on success, 0 on failure. */
static int compass3d_init(void);

/* Draws the compass into the specified OpenGL viewport rectangle.
 * x/y/width/height are OpenGL viewport coordinates (origin at bottom-left).
 * Does not clear the whole window and does not swap the SDL window. */
static void compass3d_render(double heading_degrees, double heel_degrees, double pitch,
                      int x, int y, int width, int height);

/* Camera angle from horizontal and camera distance. Suggested defaults: 62, 4.2. */
static void compass3d_set_view(float tilt_degrees, float distance);

/* Call before destroying the OpenGL context. */
static void compass3d_shutdown(void);

static void compass3d_set_rudder_angle(float degrees);

#ifdef __cplusplus
}
#endif
#endif
