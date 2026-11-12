// OpenGL compatibility scenes for the Mesa/Metal bridge. Renders a scene into
// the window (with a depth buffer), reads it back and writes a PPM. Run once
// with Mesa alone and once with the bridge preloaded, then compare the images
// (gl_compat_compare.py). Scenes:
//   shader-depth  GLSL program, window depth test, overlapping triangles
//   ff            fixed function: projection/modelview matrices, two lights with
//                 material and colour material, mipmapped texture (modulate),
//                 linear fog, alpha test, VBO and client-memory vertex arrays,
//                 client-memory indices, triangle strips and fans
//   tex-params    one texture drawn four times with different wrap/filter
//                 parameters set between the draws (glTexParameteri, a sampler
//                 object, and a display list), the bridge's texture cache must follow
//   gcc -O2 gl_compat_test.c -lGL -lX11 -lm -o gl_compat_test
//   ./gl_compat_test SCENE OUT.ppm [frames]
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define W 320
#define H 240
static GLuint compile(GLenum stage, const char *source) {
    GLuint s = glCreateShader(stage); glShaderSource(s, 1, &source, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "compile: %s\n", log); exit(3); }
    return s;
}
static void shader_depth_scene(void) {
    static GLuint program, buffer;
    if (!program) {
        const char *vs = "#version 330\nlayout(location=0) in vec3 p;layout(location=1) in vec3 c;out vec3 v;void main(){v=c;gl_Position=vec4(p,1);}\n";
        const char *fs = "#version 330\nin vec3 v;out vec4 o;void main(){o=vec4(v,1);}\n";
        program = glCreateProgram(); glAttachShader(program, compile(GL_VERTEX_SHADER, vs)); glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs)); glLinkProgram(program);
        // Near triangle (red, z=-0.5) drawn first, far triangle (blue, z=0.5) second: depth must keep red in front.
        float data[] = { -0.8f,-0.8f,-0.5f, 1,0,0,  0.6f,-0.8f,-0.5f, 1,0,0,  -0.1f,0.7f,-0.5f, 1,0,0,
                         -0.6f,-0.6f, 0.5f, 0,0,1,  0.8f,-0.6f, 0.5f, 0,0,1,   0.1f,0.8f, 0.5f, 0,0,1,
                         -0.9f, 0.1f, 0.0f, 0,1,0,  0.9f, 0.0f, 0.2f, 0,1,0,   0.0f,0.9f,-0.9f, 0,1,0 };
        glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer); glBufferData(GL_ARRAY_BUFFER, sizeof data, data, GL_STATIC_DRAW);
    }
    glClearColor(0.1f, 0.1f, 0.1f, 1); glClearDepth(1); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program); glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
    glDrawArrays(GL_TRIANGLES, 0, 9);
    glUseProgram(0); glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glBindBuffer(GL_ARRAY_BUFFER, 0);
}
static GLuint checker_texture(void) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    for (int level = 0, size = 64; size >= 1; level++, size /= 2) {
        unsigned char *p = malloc(size * size * 4);
        for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
            int on = ((x * 8 / (size ? size : 1)) + (y * 8 / (size ? size : 1))) & 1; unsigned char *q = p + (y * size + x) * 4;
            q[0] = on ? 240 : 40; q[1] = on ? 200 : 90; q[2] = on ? 60 : 160; q[3] = (x + y) % 7 == 0 ? 40 : 255;
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, p); free(p);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return t;
}
static void ff_scene(int frame) {
    static GLuint texture, vbo;
    // Cube faces as triangle strips (VBO: position, normal, texcoord).
    static const float cube[] = {
        -1,-1, 1, 0,0,1, 0,0,   1,-1, 1, 0,0,1, 1,0,  -1, 1, 1, 0,0,1, 0,1,   1, 1, 1, 0,0,1, 1,1,
         1,-1,-1, 0,0,-1, 0,0, -1,-1,-1, 0,0,-1, 1,0,   1, 1,-1, 0,0,-1, 0,1, -1, 1,-1, 0,0,-1, 1,1,
        -1,-1,-1, -1,0,0, 0,0, -1,-1, 1, -1,0,0, 1,0,  -1, 1,-1, -1,0,0, 0,1, -1, 1, 1, -1,0,0, 1,1,
         1,-1, 1, 1,0,0, 0,0,   1,-1,-1, 1,0,0, 1,0,   1, 1, 1, 1,0,0, 0,1,   1, 1,-1, 1,0,0, 1,1,
        -1, 1, 1, 0,1,0, 0,0,   1, 1, 1, 0,1,0, 2,0,  -1, 1,-1, 0,1,0, 0,2,   1, 1,-1, 0,1,0, 2,2,
        -1,-1,-1, 0,-1,0, 0,0,  1,-1,-1, 0,-1,0, 1,0, -1,-1, 1, 0,-1,0, 0,1,  1,-1, 1, 0,-1,0, 1,1 };
    if (!texture) { texture = checker_texture(); glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW); glBindBuffer(GL_ARRAY_BUFFER, 0); }
    glClearColor(0.2f, 0.25f, 0.3f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glEnable(GL_CULL_FACE); glShadeModel(GL_SMOOTH);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-1.2, 1.2, -0.9, 0.9, 1.5, 20);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslatef(0, 0, -6);
    // Lights (eye space), material, colour material.
    glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_LIGHT1); glEnable(GL_NORMALIZE);
    float l0[] = { 0.5f, 1.0f, 1.0f, 0 }, l0d[] = { 0.9f, 0.85f, 0.8f, 1 }, l0s[] = { 1, 1, 1, 1 };
    float l1[] = { -2.0f, -1.0f, 2.0f, 1 }, l1d[] = { 0.2f, 0.3f, 0.6f, 1 }, amb[] = { 0.15f, 0.15f, 0.15f, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, l0); glLightfv(GL_LIGHT0, GL_DIFFUSE, l0d); glLightfv(GL_LIGHT0, GL_SPECULAR, l0s);
    glLightfv(GL_LIGHT1, GL_POSITION, l1); glLightfv(GL_LIGHT1, GL_DIFFUSE, l1d); glLightf(GL_LIGHT1, GL_LINEAR_ATTENUATION, 0.1f);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    float spec[] = { 0.6f, 0.6f, 0.6f, 1 }; glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec); glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 24);
    glEnable(GL_COLOR_MATERIAL); glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE); glColor4f(0.9f, 0.9f, 0.9f, 1);
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, texture); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_FOG); glFogi(GL_FOG_MODE, GL_LINEAR); glFogf(GL_FOG_START, 4); glFogf(GL_FOG_END, 9); float fog[] = { 0.2f, 0.25f, 0.3f, 1 }; glFogfv(GL_FOG_COLOR, fog);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f);
    // Cube from the VBO: one strip per face.
    glPushMatrix(); glTranslatef(-1.3f, 0, 0); glRotatef(25 + frame * 3, 1, 1, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(3, GL_FLOAT, 32, (void *)0);
    glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, 32, (void *)12);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 32, (void *)24);
    for (int f = 0; f < 6; f++) glDrawArrays(GL_TRIANGLE_STRIP, f * 4, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glPopMatrix();
    // Pyramid from client memory: per-vertex colours, client-memory indices.
    glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE);
    static const float pv[] = { 0,1.2f,0,  -1,-0.8f,1,  1,-0.8f,1,  1,-0.8f,-1,  -1,-0.8f,-1 };
    static const float pn[] = { 0,1,0,  -0.6f,0.3f,0.7f,  0.6f,0.3f,0.7f,  0.6f,0.3f,-0.7f,  -0.6f,0.3f,-0.7f };
    static const unsigned char pc[] = { 255,255,255,255, 255,60,60,255, 60,255,60,255, 60,60,255,255, 255,255,60,255 };
    static const unsigned short pi[] = { 0,1,2, 0,2,3, 0,3,4, 0,4,1 };
    glPushMatrix(); glTranslatef(1.3f, 0, -0.5f); glRotatef(frame * 4, 0, 1, 0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pv); glNormalPointer(GL_FLOAT, 0, pn);
    glEnableClientState(GL_COLOR_ARRAY); glColorPointer(4, GL_UNSIGNED_BYTE, 0, pc);
    glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, pi);
    glDisableClientState(GL_COLOR_ARRAY); glPopMatrix();
    // Unlit fan with flat shading and fog (floor).
    glDisable(GL_LIGHTING); glShadeModel(GL_FLAT); glColor3f(0.4f, 0.6f, 0.3f);
    static const float floorv[] = { 0,-1,-2,  -4,-1,2,  4,-1,2,  4,-1,-8,  -4,-1,-8,  -4,-1,2 };
    glDisableClientState(GL_NORMAL_ARRAY); glVertexPointer(3, GL_FLOAT, 0, floorv); glDrawArrays(GL_TRIANGLE_FAN, 0, 6);
    // Large indexed mesh: 12000 indices into a 6000-vertex array (exceeds one bridge packet).
    static float grid[6000 * 3]; static unsigned grid_index[12000]; static int built;
    if (!built) { built = 1; for (int i = 0; i < 6000; i++) { grid[i * 3] = -3 + (i % 100) * 0.06f; grid[i * 3 + 1] = -1.4f + (i / 100) * 0.02f; grid[i * 3 + 2] = -2.5f; }
        for (int q = 0; q < 2000; q++) { int x = q % 99, y = q / 99; unsigned a = y * 100 + x; grid_index[q * 6] = a; grid_index[q * 6 + 1] = a + 1; grid_index[q * 6 + 2] = a + 100; grid_index[q * 6 + 3] = a + 1; grid_index[q * 6 + 4] = a + 101; grid_index[q * 6 + 5] = a + 100; } }
    glShadeModel(GL_SMOOTH); glColor3f(0.8f, 0.4f, 0.2f); glVertexPointer(3, GL_FLOAT, 0, grid); glDrawElements(GL_TRIANGLES, 12000, GL_UNSIGNED_INT, grid_index);
    glDisableClientState(GL_VERTEX_ARRAY); glDisable(GL_FOG); glDisable(GL_COLOR_MATERIAL);
}
// Two texture units: RGB texture (MODULATE) and a LUMINANCE texture through
// GL_COMBINE (INTERPOLATE with the constant colour), eye-linear texgen on unit 1;
// a sphere-mapped object with an ALPHA texture (REPLACE keeps the colour).
static GLuint simple_texture(GLenum internal, GLenum format, int components, int size, int seed) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    unsigned char *p = malloc(size * size * components);
    for (int i = 0; i < size * size; i++) for (int c = 0; c < components; c++) p[i * components + c] = (unsigned char)(((i % size) * (7 + c + seed) + (i / size) * (3 + seed)) * 5);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, size, size, 0, format, GL_UNSIGNED_BYTE, p); free(p);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}
static void ff_multi_scene(int frame) {
    static GLuint rgb, lum, alpha;
    if (!rgb) { rgb = simple_texture(GL_RGB8, GL_RGB, 3, 32, 1); lum = simple_texture(GL_LUMINANCE8, GL_LUMINANCE, 1, 16, 2); alpha = simple_texture(GL_ALPHA8, GL_ALPHA, 1, 16, 3); }
    glClearColor(0.05f, 0.05f, 0.1f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(-2, 2, -1.5, 1.5, -5, 5);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glRotatef(frame * 2.0f, 0, 0, 1);
    static const float quad[] = { -1.8f,-1.2f,0, 0,0,  0.2f,-1.2f,0, 1,0,  -1.8f,1.2f,0, 0,1,  0.2f,1.2f,0, 1,1 };
    glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, rgb); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTexture(GL_TEXTURE1); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, lum);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE); glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
    glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_PREVIOUS); glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_CONSTANT); glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_TEXTURE);
    glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_COLOR); glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE); glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PREVIOUS);
    float constant[] = { 0.9f, 0.2f, 0.4f, 1 }; glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, constant);
    float plane_s[] = { 0.5f, 0, 0, 0.5f }, plane_t[] = { 0, 0.5f, 0, 0.5f };
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR); glTexGenfv(GL_S, GL_EYE_PLANE, plane_s); glEnable(GL_TEXTURE_GEN_S);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR); glTexGenfv(GL_T, GL_EYE_PLANE, plane_t); glEnable(GL_TEXTURE_GEN_T);
    glColor4f(1, 1, 1, 1);
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(3, GL_FLOAT, 20, quad);
    glClientActiveTexture(GL_TEXTURE0); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 20, quad + 3);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, alpha); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClientActiveTexture(GL_TEXTURE0); glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP); glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP); glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    static const float tri[] = { 0.4f,-1,0.5f,  1.8f,-1,-0.5f,  1.1f,1.2f,0 }; static const float nrm[] = { -0.5f,0,0.8f,  0.6f,0.2f,0.7f,  0,0.7f,0.7f };
    glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, 0, nrm); glVertexPointer(3, GL_FLOAT, 0, tri); glColor4f(0.3f, 0.9f, 0.5f, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_NORMAL_ARRAY); glDisableClientState(GL_VERTEX_ARRAY); glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}
// Point sprites (GL_COORD_REPLACE, additive blend), flat-shaded GL_QUADS and a
// smooth GL_QUAD_STRIP.
static void ff_points_scene(int frame) {
    static GLuint sprite;
    if (!sprite) sprite = simple_texture(GL_RGBA8, GL_RGBA, 4, 16, 4);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    static float quads[8 * 2]; static unsigned char colors[8 * 4];
    for (int i = 0; i < 8; i++) { int q = i / 4, k = i % 4; quads[i * 2] = 20 + q * 120 + (k == 1 || k == 2 ? 90 : 0); quads[i * 2 + 1] = 20 + (k >= 2 ? 80 : 0) + frame;
        colors[i * 4] = (unsigned char)(60 * i); colors[i * 4 + 1] = (unsigned char)(255 - 30 * i); colors[i * 4 + 2] = (unsigned char)(40 * k); colors[i * 4 + 3] = 255; }
    glShadeModel(GL_FLAT); glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, quads); glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors); glDrawArrays(GL_QUADS, 0, 8);
    static const float strip[] = { 20,140, 20,200, 100,140, 100,200, 180,150, 180,210, 260,140, 260,200 };
    glShadeModel(GL_SMOOTH); glVertexPointer(2, GL_FLOAT, 0, strip); glDrawArrays(GL_QUAD_STRIP, 0, 8);
    glDisableClientState(GL_COLOR_ARRAY);
    static float points[24 * 2]; for (int i = 0; i < 24; i++) { points[i * 2] = 30 + (i % 8) * 36 + frame; points[i * 2 + 1] = 120 + (i / 8) * 30; }
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, sprite); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_POINT_SPRITE); glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_TRUE); glPointSize(14);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); glColor4f(1, 0.8f, 0.5f, 0.9f);
    glVertexPointer(2, GL_FLOAT, 0, points); glDrawArrays(GL_POINTS, 0, 24);
    glDisable(GL_BLEND); glDisable(GL_POINT_SPRITE); glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_FALSE); glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_VERTEX_ARRAY); glPointSize(1);
}
static void tex_params_scene(int frame) {
    static GLuint tex, list, sampler;
    if (!tex) { tex = simple_texture(GL_RGBA8, GL_RGBA, 4, 8, 5); list = glGenLists(1); glNewList(list, GL_COMPILE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT); glEndList(); glGenSamplers(1, &sampler); }
    glClearColor(0.1f, 0.1f, 0.1f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_VERTEX_ARRAY); glClientActiveTexture(GL_TEXTURE0); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    float shift = (frame % 4) * 0.125f;
    for (int i = 0; i < 4; i++) {
        float x = 10 + (i % 2) * 155, y = 10 + (i / 2) * 115;
        float quad[] = { x, y, -1 + shift, -1, x + 145, y, 2 + shift, -1, x, y + 105, -1 + shift, 2, x + 145, y + 105, 2 + shift, 2 };
        if (i == 0) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); }
        if (i == 1) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); }
        if (i == 2) glCallList(list);
        if (i == 3) { glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_REPEAT); glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST); glBindSampler(0, sampler); }
        glVertexPointer(2, GL_FLOAT, 16, quad); glTexCoordPointer(2, GL_FLOAT, 16, quad + 2); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindSampler(0, 0); glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_VERTEX_ARRAY); glDisable(GL_TEXTURE_2D);
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s shader-depth|ff OUT.ppm [frames]\n", argv[0]); return 2; }
    int frames = argc > 3 ? atoi(argv[3]) : 30, ff = !strcmp(argv[1], "ff"), multi = !strcmp(argv[1], "ff-multi"), pts = !strcmp(argv[1], "ff-points"), params = !strcmp(argv[1], "tex-params");
    Display *d = XOpenDisplay(NULL); if (!d) { fprintf(stderr, "no display\n"); return 2; }
    int attributes[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 24, None };
    XVisualInfo *vi = glXChooseVisual(d, DefaultScreen(d), attributes); if (!vi) { fprintf(stderr, "no visual\n"); return 2; }
    XSetWindowAttributes swa = { .colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone), .event_mask = ExposureMask };
    Window win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(d, win); XSync(d, False);
    GLXContext ctx = glXCreateContext(d, vi, NULL, True); glXMakeCurrent(d, win, ctx);
    glViewport(0, 0, W, H);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < frames; f++) { if (ff) ff_scene(f); else if (multi) ff_multi_scene(f); else if (pts) ff_points_scene(f); else if (params) tex_params_scene(f); else shader_depth_scene(); if (f < frames - 1) glXSwapBuffers(d, win); }
    unsigned char *pixels = malloc(W * H * 3); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    FILE *out = fopen(argv[2], "wb"); fprintf(out, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--) fwrite(pixels + y * W * 3, 1, W * 3, out);
    fclose(out);
    printf("GL_COMPAT_TEST scene=%s frames=%d seconds=%.2f fps=%.2f renderer=%s error=0x%x\n", argv[1], frames, seconds, frames / seconds, glGetString(GL_RENDERER), glGetError());
    glXSwapBuffers(d, win); glXMakeCurrent(d, None, NULL); glXDestroyContext(d, ctx); XCloseDisplay(d);
    return 0;
}
