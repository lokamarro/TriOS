// Fixed-function OpenGL on the Metal path (included by mesa_metal_dispatch.c).
//
// A compatibility-context draw with no program bound uses the GL 1.x pipeline.
// The bridge describes that state as a key, generates an equivalent GLSL 3.30
// program per key (cached), and draws it like an application program: Mesa
// compiles and links it (and stays the semantic owner), the host translates the
// same source to Metal. All per-draw fixed-function values travel in one std140
// uniform block filled from the GL state, so no GL buffer, binding or program
// state of the application is touched. Vertex data is read straight from the
// fixed-function arrays (buffer objects or client memory).
//
// Implemented: modelview/projection/texture matrices; per-vertex lighting with
// up to 8 directional, point or spot lights, material, colour material (all
// modes), scene ambient, local viewer, separate specular colour, normalize /
// rescale-normal; texture unit 0 (2D) with MODULATE, REPLACE, DECAL, ADD, BLEND
// and projective coordinates; linear/exp/exp2 fog on eye depth; alpha test; flat
// and smooth shading. Anything else (two-sided lighting, texture generation,
// other texture units or targets, clip planes, stipple, ARB programs, secondary
// colour or fog-coordinate arrays, combiners) keeps Mesa, with a named reason.
#define FF_PROGRAMS 64
enum {
    FF_LIGHTING = 1u << 0, FF_TEXTURE = 1u << 1, FF_FLAT = 1u << 2, FF_LOCAL_VIEWER = 1u << 3, FF_SEPARATE_SPECULAR = 1u << 4,
    FF_NORMALIZE = 1u << 5, FF_TEXTURE_ALPHA = 1u << 6, FF_ALPHA_TEST = 1u << 7, FF_FOG = 1u << 8, FF_POINTS = 1u << 9,
};
#define FF_UNITS 4
// Per texture unit: base format, environment (classic or COMBINE terms) and
// texture coordinate generation (per coordinate S,T,R,Q).
enum { FF_BASE_RGBA = 1, FF_BASE_RGB, FF_BASE_L, FF_BASE_A, FF_BASE_LA, FF_BASE_I };
enum { FF_GEN_NONE = 0, FF_GEN_OBJECT, FF_GEN_EYE, FF_GEN_SPHERE };
struct ff_unit {
    uint8_t enabled, base, texgen[4], coord_replace;
    uint16_t env;                                  // GL_MODULATE... or GL_COMBINE
    uint16_t combine_rgb, combine_alpha, source_rgb[3], source_alpha[3], operand_rgb[3], operand_alpha[3];
};
struct ff_key {
    uint32_t bits;
    uint8_t lights, point_lights, spot_lights;   // masks over GL_LIGHT0..7
    uint8_t color_material;                       // 0 off, else GL mode index (1..5)
    uint16_t texenv, alpha_func, fog_mode;        // GL enums (low 16 bits); texenv unused (per unit below)
    struct ff_unit unit[FF_UNITS];
};
// std140: every member is a vec4 or mat4, so the C layout is the block layout.
struct ff_block {
    float mvp[16], modelview[16], texture[16], normal[16];   // texture: unit 0 (also in texture_unit[0])
    float material_ambient[4], material_diffuse[4], material_specular[4], material_emission[4];
    float scene_ambient[4];
    float params[4];        // x shininess, y alpha reference, z fog start, w fog end
    float fog[4];           // x density, y 1/(end-start)
    float fog_color[4], env_color[4];
    float light_position[8][4], light_ambient[8][4], light_diffuse[8][4], light_specular[8][4];
    float light_attenuation[8][4], light_spot_direction[8][4], light_spot[8][4];
    float texture_unit[FF_UNITS][16];
    float unit_env_color[FF_UNITS][4], unit_scale[FF_UNITS][4];   // scale: x RGB_SCALE, y ALPHA_SCALE
    float object_plane[FF_UNITS][4][4], eye_plane[FF_UNITS][4][4];
    float point[4];         // x size, y min, z max, w fade threshold
    float point_attenuation[4];   // GL_POINT_DISTANCE_ATTENUATION a, b, c; w 1 when attenuation is on
};
static struct { uintptr_t group; struct ff_key key; GLuint program; GLint sampler; } ff_programs[FF_PROGRAMS];
static unsigned ff_programs_next;
static pthread_mutex_t ff_lock = PTHREAD_MUTEX_INITIALIZER;
static LR_TLS struct ff_block ff_block_current;
static LR_TLS uint8_t ff_vertex_units[FF_UNITS],ff_vertex_normal;   // inputs the generated vertex stage reads

// Highest texture unit the application ever selected, plus one: units it never
// selected cannot be enabled, so the per-draw state scan stops there.
static _Atomic unsigned ff_units_seen = 1;
static void ff_note_unit(GLenum texture) {
    unsigned u = texture - GL_TEXTURE0; if (u >= 32) return;
    unsigned seen = atomic_load(&ff_units_seen);
    while (u + 1 > seen && !atomic_compare_exchange_weak(&ff_units_seen, &seen, u + 1)) {}
}
void glActiveTexture(GLenum texture) { typeof(&glActiveTexture) real = original("glActiveTexture"); ff_note_unit(texture); if (real) real(texture); }
void glActiveTextureARB(GLenum texture) { typeof(&glActiveTextureARB) real = original("glActiveTextureARB"); ff_note_unit(texture); if (real) real(texture); }
static int ff_disabled(void) { const char *s = config("LR_MESA_METAL_FIXED"); return s && !strcmp(s, "0"); }
static void ff_multiply(float out[16], const float a[16], const float b[16]) {   // column-major a*b
    float r[16];
    for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) {
        float v = 0; for (int k = 0; k < 4; k++) v += a[k * 4 + row] * b[c * 4 + k]; r[c * 4 + row] = v;
    }
    memcpy(out, r, sizeof r);
}
// Inverse-transpose of the upper 3x3 of the modelview, stored in a mat4.
static void ff_normal_matrix(float out[16], const float m[16]) {
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g, det = a * A + b * B + c * C;
    float s = fabsf(det) > 1e-20f ? 1.0f / det : 0;
    memset(out, 0, 16 * sizeof(float)); out[15] = 1;
    // inverse = adj/det; inverse-transpose element (row r, col c) = cofactor(r, c)/det
    float cof[3][3] = { { A, B, C }, { -(b * i - c * h), a * i - c * g, -(a * h - b * g) }, { b * f - c * e, -(a * f - c * d), a * e - b * d } };
    for (int r = 0; r < 3; r++) for (int col = 0; col < 3; col++) out[col * 4 + r] = cof[r][col] * s;
}
static int ff_texture_unit_enabled(unsigned unit) {
    GLenum targets[] = { GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_RECTANGLE };
    for (unsigned t = 0; t < 5; t++) if (glIsEnabled(targets[t])) return (int)t + 1;
    (void)unit; return 0;
}
// Fixed-function state -> key and block. Returns NULL when supported.
static const char *ff_collect(struct ff_key *key, struct ff_block *b, GLenum mode) {
    memset(key, 0, sizeof *key); memset(b, 0, sizeof *b);
    int sprite = 0;
    if (mode == GL_POINTS) {
        // Square points of GL_POINT_SIZE; sprites take texture coordinates from
        // gl_PointCoord (upper-left origin, as Metal's point coordinates).
        if (glIsEnabled(GL_POINT_SMOOTH)) return "ff:point-smooth";
        // Distance attenuation (GL 1.4): size * sqrt(1/(a + b d + c d^2)), clamped to
        // [min, max]; below the fade threshold the size stays at the threshold
        // and alpha fades by (size/threshold)^2.
        glGetFloatv(GL_POINT_DISTANCE_ATTENUATION, b->point_attenuation);
        b->point_attenuation[3] = (b->point_attenuation[0] != 1 || b->point_attenuation[1] != 0 || b->point_attenuation[2] != 0) ? 1 : 0;
        key->bits |= FF_POINTS; glGetFloatv(GL_POINT_SIZE, &b->point[0]);
        glGetFloatv(GL_POINT_SIZE_MIN, &b->point[1]); glGetFloatv(GL_POINT_SIZE_MAX, &b->point[2]); glGetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, &b->point[3]);
        if (b->point[2] <= 0) b->point[2] = 1024;
        b->point[3] = 0;   // fade applies only with multisampling, which the bridge never draws (multisample targets keep Mesa)
        sprite = glIsEnabled(GL_POINT_SPRITE);
        if (sprite) { GLint origin = GL_UPPER_LEFT; glGetIntegerv(GL_POINT_SPRITE_COORD_ORIGIN, &origin); if (origin != GL_UPPER_LEFT) return "ff:point-sprite-origin"; }
    }
    if (glIsEnabled(GL_VERTEX_PROGRAM_ARB) || glIsEnabled(GL_FRAGMENT_PROGRAM_ARB)) return "ff:arb-program";
    if (!glIsEnabled(GL_VERTEX_ARRAY)) return "ff:no-vertex-array";
    if (glIsEnabled(GL_SECONDARY_COLOR_ARRAY) || glIsEnabled(GL_FOG_COORD_ARRAY)) return "ff:secondary-or-fog-array";
    if (glIsEnabled(GL_POLYGON_STIPPLE) || glIsEnabled(GL_LINE_STIPPLE)) return "ff:stipple";
    for (unsigned i = 0; i < 6; i++) if (glIsEnabled(GL_CLIP_PLANE0 + i)) return "ff:clip-plane";
    if (glIsEnabled(GL_COLOR_SUM)) return "ff:color-sum";
    typeof(&glActiveTexture) select_unit = original("glActiveTexture"); if (!select_unit) return "ff:no-active-texture";
    GLint active = GL_TEXTURE0, units = (GLint)atomic_load(&ff_units_seen); glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    const char *reject = NULL;
    for (GLint u = 0; u < units && u < 8 && !reject; u++) {
        select_unit(GL_TEXTURE0 + u);
        int target = ff_texture_unit_enabled(u);
        if (!target) continue;
        if (u >= FF_UNITS) { reject = "ff:texture-unit-4+"; break; }
        if (target != 2) { reject = "ff:texture-target"; break; }
        struct ff_unit *t = &key->unit[u]; t->enabled = 1;
        GLint texture = 0, internal = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        if (texture) glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
        switch (internal) {
            case GL_RGBA8: case GL_RGBA: case GL_SRGB8_ALPHA8: case GL_RGBA16F: case GL_RGBA4: case GL_RGB5_A1: case 4: t->base = FF_BASE_RGBA; break;
            case GL_RGB8: case GL_RGB: case GL_RGB5: case GL_RGB565: case 3: t->base = FF_BASE_RGB; break;
            case GL_LUMINANCE8: case GL_LUMINANCE: case 1: t->base = FF_BASE_L; break;
            case GL_ALPHA8: case GL_ALPHA: t->base = FF_BASE_A; break;
            case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE_ALPHA: case 2: t->base = FF_BASE_LA; break;
            case GL_INTENSITY8: case GL_INTENSITY: t->base = FF_BASE_I; break;
            default: if (texture) reject = "ff:texture-base-format"; t->base = FF_BASE_RGBA; break;
        }
        GLint env = GL_MODULATE; glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env); t->env = (uint16_t)env;
        if (env != GL_MODULATE && env != GL_REPLACE && env != GL_DECAL && env != GL_ADD && env != GL_BLEND && env != GL_COMBINE) { reject = "ff:texenv"; break; }
        if (env == GL_COMBINE) {
            GLint v;
            glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &v); t->combine_rgb = (uint16_t)v;
            glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, &v); t->combine_alpha = (uint16_t)v;
            for (unsigned i = 0; i < 3; i++) {
                glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC0_RGB + i, &v); t->source_rgb[i] = (uint16_t)v;
                glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC0_ALPHA + i, &v); t->source_alpha[i] = (uint16_t)v;
                glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_RGB + i, &v); t->operand_rgb[i] = (uint16_t)v;
                glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA + i, &v); t->operand_alpha[i] = (uint16_t)v;
            }
            glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, &b->unit_scale[u][0]); glGetTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE, &b->unit_scale[u][1]);
        }
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, b->unit_env_color[u]);
        if (sprite) { GLint replace = GL_FALSE; glGetTexEnviv(GL_POINT_SPRITE, GL_COORD_REPLACE, &replace); t->coord_replace = replace != GL_FALSE; }
        glGetFloatv(GL_TEXTURE_MATRIX, b->texture_unit[u]);   // of the active unit; the matrix mode is not touched
        GLenum gens[4] = { GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R, GL_TEXTURE_GEN_Q }, coords[4] = { GL_S, GL_T, GL_R, GL_Q };
        for (unsigned k = 0; k < 4 && !reject; k++) {
            if (!glIsEnabled(gens[k])) continue;
            GLint mode = GL_EYE_LINEAR; glGetTexGeniv(coords[k], GL_TEXTURE_GEN_MODE, &mode);
            if (mode == GL_OBJECT_LINEAR) { t->texgen[k] = FF_GEN_OBJECT; glGetTexGenfv(coords[k], GL_OBJECT_PLANE, b->object_plane[u][k]); }
            else if (mode == GL_EYE_LINEAR) { t->texgen[k] = FF_GEN_EYE; glGetTexGenfv(coords[k], GL_EYE_PLANE, b->eye_plane[u][k]); }   // stored in eye space
            else if (mode == GL_SPHERE_MAP && k < 2) t->texgen[k] = FF_GEN_SPHERE;
            else reject = "ff:texgen-mode";
        }
        key->bits |= FF_TEXTURE;
    }
    select_unit(active);
    if (reject) return reject;
    memcpy(b->texture, b->texture_unit[0], sizeof b->texture);
    GLint shade = GL_SMOOTH; glGetIntegerv(GL_SHADE_MODEL, &shade); if (shade == GL_FLAT) key->bits |= FF_FLAT;
    float projection[16]; glGetFloatv(GL_MODELVIEW_MATRIX, b->modelview); glGetFloatv(GL_PROJECTION_MATRIX, projection);
    ff_multiply(b->mvp, projection, b->modelview); ff_normal_matrix(b->normal, b->modelview);
    if (glIsEnabled(GL_NORMALIZE) || glIsEnabled(GL_RESCALE_NORMAL)) key->bits |= FF_NORMALIZE;
    if (glIsEnabled(GL_LIGHTING)) {
        key->bits |= FF_LIGHTING;
        GLint two_side = 0, local = 0, control = GL_SINGLE_COLOR;
        glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &two_side); if (two_side) return "ff:two-side-lighting";
        glGetIntegerv(GL_LIGHT_MODEL_LOCAL_VIEWER, &local); if (local) key->bits |= FF_LOCAL_VIEWER;
        glGetIntegerv(GL_LIGHT_MODEL_COLOR_CONTROL, &control); if (control == GL_SEPARATE_SPECULAR_COLOR) key->bits |= FF_SEPARATE_SPECULAR;
        glGetFloatv(GL_LIGHT_MODEL_AMBIENT, b->scene_ambient);
        glGetMaterialfv(GL_FRONT, GL_AMBIENT, b->material_ambient); glGetMaterialfv(GL_FRONT, GL_DIFFUSE, b->material_diffuse);
        glGetMaterialfv(GL_FRONT, GL_SPECULAR, b->material_specular); glGetMaterialfv(GL_FRONT, GL_EMISSION, b->material_emission);
        glGetMaterialfv(GL_FRONT, GL_SHININESS, &b->params[0]);
        if (glIsEnabled(GL_COLOR_MATERIAL)) {
            GLint mode = GL_AMBIENT_AND_DIFFUSE; glGetIntegerv(GL_COLOR_MATERIAL_PARAMETER, &mode);
            key->color_material = mode == GL_AMBIENT ? 1 : mode == GL_DIFFUSE ? 2 : mode == GL_SPECULAR ? 3 : mode == GL_EMISSION ? 4 : 5;
        }
        for (unsigned i = 0; i < 8; i++) {
            if (!glIsEnabled(GL_LIGHT0 + i)) continue;
            key->lights |= 1u << i;
            glGetLightfv(GL_LIGHT0 + i, GL_POSITION, b->light_position[i]); glGetLightfv(GL_LIGHT0 + i, GL_AMBIENT, b->light_ambient[i]);
            glGetLightfv(GL_LIGHT0 + i, GL_DIFFUSE, b->light_diffuse[i]); glGetLightfv(GL_LIGHT0 + i, GL_SPECULAR, b->light_specular[i]);
            glGetLightfv(GL_LIGHT0 + i, GL_CONSTANT_ATTENUATION, &b->light_attenuation[i][0]);
            glGetLightfv(GL_LIGHT0 + i, GL_LINEAR_ATTENUATION, &b->light_attenuation[i][1]);
            glGetLightfv(GL_LIGHT0 + i, GL_QUADRATIC_ATTENUATION, &b->light_attenuation[i][2]);
            float cutoff = 180; glGetLightfv(GL_LIGHT0 + i, GL_SPOT_CUTOFF, &cutoff);
            if (b->light_position[i][3] != 0) key->point_lights |= 1u << i;
            if (cutoff != 180) {
                key->spot_lights |= 1u << i;
                glGetLightfv(GL_LIGHT0 + i, GL_SPOT_DIRECTION, b->light_spot_direction[i]);   // eye space
                b->light_spot_direction[i][3] = cosf(cutoff * (float)M_PI / 180.0f);
                glGetLightfv(GL_LIGHT0 + i, GL_SPOT_EXPONENT, &b->light_spot[i][0]);
            }
        }
    }
    if (glIsEnabled(GL_ALPHA_TEST)) {
        GLint func = GL_ALWAYS; glGetIntegerv(GL_ALPHA_TEST_FUNC, &func); glGetFloatv(GL_ALPHA_TEST_REF, &b->params[1]);
        if (func != GL_ALWAYS) { key->bits |= FF_ALPHA_TEST; key->alpha_func = (uint16_t)func; }
    }
    if (glIsEnabled(GL_FOG)) {
        GLint mode = GL_EXP, source = GL_FRAGMENT_DEPTH; glGetIntegerv(GL_FOG_MODE, &mode); glGetIntegerv(GL_FOG_COORD_SRC, &source);
        if (source != GL_FRAGMENT_DEPTH) return "ff:fog-coordinate";
        key->bits |= FF_FOG; key->fog_mode = (uint16_t)mode;
        glGetFloatv(GL_FOG_START, &b->params[2]); glGetFloatv(GL_FOG_END, &b->params[3]); glGetFloatv(GL_FOG_DENSITY, &b->fog[0]);
        b->fog[1] = b->params[3] != b->params[2] ? 1.0f / (b->params[3] - b->params[2]) : 0; glGetFloatv(GL_FOG_COLOR, b->fog_color);
    }
    return NULL;
}
// ---- GLSL ----------------------------------------------------------------
#define FF_APPEND(...) do { int n_ = snprintf(out + used, size - used, __VA_ARGS__); if (n_ < 0 || (size_t)n_ >= size - used) return 0; used += n_; } while (0)
static const char ff_block_glsl[] =
    "layout(std140) uniform LRFixedFunction {\n"
    " mat4 mvp, modelview, texture_matrix, normal_matrix;\n"
    " vec4 material_ambient, material_diffuse, material_specular, material_emission, scene_ambient;\n"
    " vec4 params, fog, fog_color, env_color;\n"
    " vec4 light_position[8], light_ambient[8], light_diffuse[8], light_specular[8];\n"
    " vec4 light_attenuation[8], light_spot_direction[8], light_spot[8];\n"
    " mat4 texture_unit[4];\n"
    " vec4 unit_env_color[4], unit_scale[4];\n"
    " vec4 object_plane[16], eye_plane[16];\n"
    " vec4 point, point_attenuation;\n"
    "};\n";
static int ff_vertex_source(const struct ff_key *k, char *out, size_t size) {
    size_t used = 0; const char *flat = (k->bits & FF_FLAT) ? "flat " : "";
    FF_APPEND("#version 330\n%s", ff_block_glsl);
    FF_APPEND("layout(location=0) in vec4 lr_position;\nlayout(location=1) in vec3 lr_normal;\nlayout(location=2) in vec4 lr_color;\n");
    // Varyings in the same order in both stages (the Metal translation assigns
    // locations by declaration order): colour, specular, fog depth, texcoords.
    for (unsigned u = 0; u < FF_UNITS; u++) if (k->unit[u].enabled) FF_APPEND("layout(location=%u) in vec4 lr_texcoord%u;\n", 3 + u, u);
    FF_APPEND("%sout vec4 lr_v_color;\n%sout vec4 lr_v_specular;\nout float lr_v_fog_depth;\n", flat, flat);
    for (unsigned u = 0; u < FF_UNITS; u++) if (k->unit[u].enabled) FF_APPEND("out vec4 lr_v_tc%u;\n", u);
    FF_APPEND("void main() {\n vec4 eye = modelview * lr_position;\n gl_Position = mvp * lr_position;\n lr_v_fog_depth = abs(eye.z);\n lr_v_specular = vec4(0.0);\n");
    if (k->bits & FF_POINTS) {
        FF_APPEND(" float size = point.x;\n if (point_attenuation.w > 0.0) { float d = length(eye.xyz); size *= inversesqrt(max(point_attenuation.x + point_attenuation.y * d + point_attenuation.z * d * d, 1e-6)); }\n");
        FF_APPEND(" size = clamp(size, point.y, point.z);\n float fade = 1.0;\n if (point_attenuation.w > 0.0 && size < point.w) { fade = (size / point.w) * (size / point.w); size = point.w; }\n gl_PointSize = size;\n");
    }
    int sphere = 0; for (unsigned u = 0; u < FF_UNITS; u++) for (unsigned c = 0; c < 2; c++) if (k->unit[u].enabled && k->unit[u].texgen[c] == FF_GEN_SPHERE) sphere = 1;
    if (sphere) FF_APPEND(" vec3 sr = reflect(normalize(eye.xyz), normalize(mat3(normal_matrix) * lr_normal));\n float sm = 2.0 * sqrt(sr.x * sr.x + sr.y * sr.y + (sr.z + 1.0) * (sr.z + 1.0));\n");
    for (unsigned u = 0; u < FF_UNITS; u++) {
        if (!k->unit[u].enabled) continue;
        FF_APPEND(" { vec4 tc = lr_texcoord%u;\n", u);
        static const char comp[4] = { 'x', 'y', 'z', 'w' };
        for (unsigned c = 0; c < 4; c++) switch (k->unit[u].texgen[c]) {
            case FF_GEN_OBJECT: FF_APPEND("  tc.%c = dot(object_plane[%u], lr_position);\n", comp[c], u * 4 + c); break;
            case FF_GEN_EYE: FF_APPEND("  tc.%c = dot(eye_plane[%u], eye);\n", comp[c], u * 4 + c); break;
            case FF_GEN_SPHERE: FF_APPEND("  tc.%c = sr.%c / sm + 0.5;\n", comp[c], comp[c]); break;
        }
        FF_APPEND("  lr_v_tc%u = texture_unit[%u] * tc; }\n", u, u);
    }
    if (!(k->bits & FF_LIGHTING)) { FF_APPEND(" lr_v_color = lr_color;\n%s}\n", (k->bits & FF_POINTS) ? " lr_v_color.a *= fade;\n" : ""); return 1; }
    FF_APPEND(" vec3 n = mat3(normal_matrix) * lr_normal;\n");
    if (k->bits & FF_NORMALIZE) FF_APPEND(" n = normalize(n);\n");
    FF_APPEND(" vec4 ma = material_ambient, md = material_diffuse, ms = material_specular, me = material_emission;\n");
    switch (k->color_material) {
        case 1: FF_APPEND(" ma = lr_color;\n"); break;
        case 2: FF_APPEND(" md = lr_color;\n"); break;
        case 3: FF_APPEND(" ms = lr_color;\n"); break;
        case 4: FF_APPEND(" me = lr_color;\n"); break;
        case 5: FF_APPEND(" ma = lr_color; md = lr_color;\n"); break;
    }
    FF_APPEND(" vec3 c = me.rgb + ma.rgb * scene_ambient.rgb;\n vec3 s = vec3(0.0);\n");
    FF_APPEND(" vec3 view = %s;\n", (k->bits & FF_LOCAL_VIEWER) ? "normalize(-eye.xyz)" : "vec3(0.0, 0.0, 1.0)");
    for (unsigned i = 0; i < 8; i++) {
        if (!(k->lights & (1u << i))) continue;
        FF_APPEND(" {\n  vec3 l; float att = 1.0;\n");
        if (k->point_lights & (1u << i)) {
            FF_APPEND("  vec3 d = light_position[%u].xyz - eye.xyz; float dist = length(d); l = d / dist;\n", i);
            FF_APPEND("  att = 1.0 / (light_attenuation[%u].x + light_attenuation[%u].y * dist + light_attenuation[%u].z * dist * dist);\n", i, i, i);
        } else FF_APPEND("  l = normalize(light_position[%u].xyz);\n", i);
        if (k->spot_lights & (1u << i))
            FF_APPEND("  float sd = dot(-l, normalize(light_spot_direction[%u].xyz));\n  att *= sd >= light_spot_direction[%u].w ? pow(max(sd, 0.0), light_spot[%u].x) : 0.0;\n", i, i, i);
        FF_APPEND("  float ndl = max(dot(n, l), 0.0);\n  c += att * (light_ambient[%u].rgb * ma.rgb + ndl * light_diffuse[%u].rgb * md.rgb);\n", i, i);
        FF_APPEND("  if (ndl > 0.0) { float ndh = max(dot(n, normalize(l + view)), 0.0); float f = params.x > 0.0 ? pow(ndh, params.x) : 1.0; s += att * f * light_specular[%u].rgb * ms.rgb; }\n }\n", i);
    }
    if (k->bits & FF_SEPARATE_SPECULAR) FF_APPEND(" lr_v_color = vec4(clamp(c, 0.0, 1.0), clamp(md.a, 0.0, 1.0));\n lr_v_specular = vec4(clamp(s, 0.0, 1.0), 0.0);\n");
    else FF_APPEND(" lr_v_color = vec4(clamp(c + s, 0.0, 1.0), clamp(md.a, 0.0, 1.0));\n");
    FF_APPEND("%s}\n", (k->bits & FF_POINTS) ? " lr_v_color.a *= fade;\n" : "");
    return 1;
}
static const char *ff_compare(uint16_t func) {
    switch (func) { case GL_NEVER: return "false"; case GL_LESS: return "a < r"; case GL_EQUAL: return "a == r"; case GL_LEQUAL: return "a <= r";
        case GL_GREATER: return "a > r"; case GL_NOTEQUAL: return "a != r"; case GL_GEQUAL: return "a >= r"; default: return "true"; }
}
// GLSL for one COMBINE argument: source (with this unit's base format) and operand.
static void ff_source(char *out, size_t size, unsigned unit, uint16_t source, int alpha_channel) {
    const char *v;
    switch (source) {
        case GL_TEXTURE: v = NULL; break;
        case GL_CONSTANT: snprintf(out, size, "unit_env_color[%u]", unit); return;
        case GL_PRIMARY_COLOR: v = "lr_v_color"; break;
        case GL_PREVIOUS: v = "vec4(cp, ap)"; break;
        default:
            if (source >= GL_TEXTURE0 && source < GL_TEXTURE0 + FF_UNITS) { snprintf(out, size, "vec4(ct%u, at%u)", source - GL_TEXTURE0, source - GL_TEXTURE0); return; }
            v = "vec4(cp, ap)"; break;   // undefined source: previous
    }
    if (!v) snprintf(out, size, "vec4(ct%u, at%u)", unit, unit); else snprintf(out, size, "%s", v);
    (void)alpha_channel;
}
static void ff_operand(char *out, size_t size, const char *src, uint16_t operand, int alpha) {
    if (alpha) snprintf(out, size, operand == GL_ONE_MINUS_SRC_ALPHA ? "(1.0 - (%s).a)" : "(%s).a", src);
    else switch (operand) {
        case GL_ONE_MINUS_SRC_COLOR: snprintf(out, size, "(vec3(1.0) - (%s).rgb)", src); break;
        case GL_SRC_ALPHA: snprintf(out, size, "vec3((%s).a)", src); break;
        case GL_ONE_MINUS_SRC_ALPHA: snprintf(out, size, "vec3(1.0 - (%s).a)", src); break;
        default: snprintf(out, size, "(%s).rgb", src); break;
    }
}
static int ff_combine(char *out, size_t size, size_t *used_out, const struct ff_unit *t, unsigned u, int alpha) {
    size_t used = *used_out; char a[3][96];
    for (unsigned i = 0; i < 3; i++) {
        char src[64]; ff_source(src, sizeof src, u, alpha ? t->source_alpha[i] : t->source_rgb[i], alpha);
        ff_operand(a[i], sizeof a[i], src, alpha ? t->operand_alpha[i] : t->operand_rgb[i], alpha);
    }
    uint16_t op = alpha ? t->combine_alpha : t->combine_rgb; const char *dst = alpha ? "ap" : "cp", *scale = alpha ? "y" : "x";
    switch (op) {
        case GL_REPLACE: FF_APPEND("  %s = %s;\n", dst, a[0]); break;
        case GL_MODULATE: FF_APPEND("  %s = %s * %s;\n", dst, a[0], a[1]); break;
        case GL_ADD: FF_APPEND("  %s = %s + %s;\n", dst, a[0], a[1]); break;
        case GL_ADD_SIGNED: FF_APPEND("  %s = %s + %s - 0.5;\n", dst, a[0], a[1]); break;
        case GL_INTERPOLATE: FF_APPEND("  %s = mix(%s, %s, %s);\n", dst, a[1], a[0], a[2]); break;
        case GL_SUBTRACT: FF_APPEND("  %s = %s - %s;\n", dst, a[0], a[1]); break;
        case GL_DOT3_RGB: case GL_DOT3_RGBA:
            if (alpha) return 0;   // alpha handled by the RGB term for DOT3_RGBA
            FF_APPEND("  { float d = 4.0 * dot(%s - 0.5, %s - 0.5); cp = vec3(d);%s }\n", a[0], a[1], op == GL_DOT3_RGBA ? " ap = d; dot3_alpha = true;" : ""); break;
        default: return 0;
    }
    FF_APPEND("  %s = clamp(%s * unit_scale[%u].%s, 0.0, 1.0);\n", dst, dst, u, scale);
    *used_out = used; return 1;
}
static int ff_fragment_source(const struct ff_key *k, char *out, size_t size) {
    size_t used = 0; const char *flat = (k->bits & FF_FLAT) ? "flat " : "";
    FF_APPEND("#version 330\n%s", ff_block_glsl);
    FF_APPEND("%sin vec4 lr_v_color;\n%sin vec4 lr_v_specular;\nin float lr_v_fog_depth;\nout vec4 lr_fragment;\n", flat, flat);
    for (unsigned u = 0; u < FF_UNITS; u++) if (k->unit[u].enabled) FF_APPEND("in vec4 lr_v_tc%u;\n", u);
    for (unsigned u = 0; u < FF_UNITS; u++) if (k->unit[u].enabled) FF_APPEND("uniform sampler2D lr_texture%u;\n", u);
    FF_APPEND("void main() {\n vec3 cp = lr_v_color.rgb; float ap = lr_v_color.a;\n");
    // Every enabled unit's texel first (COMBINE may reference any unit), as (Ct, At) by base format.
    for (unsigned u = 0; u < FF_UNITS; u++) {
        const struct ff_unit *t = &k->unit[u]; if (!t->enabled) continue;
        if (t->coord_replace) FF_APPEND(" vec4 t%u = texture(lr_texture%u, gl_PointCoord);\n", u, u);
        else FF_APPEND(" vec4 t%u = textureProj(lr_texture%u, lr_v_tc%u.xyw);\n", u, u, u);
        switch (t->base) {
            case FF_BASE_RGB: FF_APPEND(" vec3 ct%u = t%u.rgb; float at%u = 1.0;\n", u, u, u); break;
            case FF_BASE_L: FF_APPEND(" vec3 ct%u = vec3(t%u.r); float at%u = 1.0;\n", u, u, u); break;
            case FF_BASE_A: FF_APPEND(" vec3 ct%u = vec3(0.0); float at%u = t%u.a;\n", u, u, u); break;
            case FF_BASE_LA: FF_APPEND(" vec3 ct%u = vec3(t%u.r); float at%u = t%u.a;\n", u, u, u, u); break;
            case FF_BASE_I: FF_APPEND(" vec3 ct%u = vec3(t%u.r); float at%u = t%u.r;\n", u, u, u, u); break;
            default: FF_APPEND(" vec3 ct%u = t%u.rgb; float at%u = t%u.a;\n", u, u, u, u); break;
        }
    }
    // Texture environments in unit order (GL 2.1 tables 3.22/3.23; COMBINE: 3.24/3.25).
    for (unsigned u = 0; u < FF_UNITS; u++) {
        const struct ff_unit *t = &k->unit[u]; if (!t->enabled) continue;
        int b = t->base; FF_APPEND(" {\n");
        switch (t->env) {
            case GL_REPLACE:
                if (b == FF_BASE_A) FF_APPEND("  ap = at%u;\n", u);
                else if (b == FF_BASE_L || b == FF_BASE_RGB) FF_APPEND("  cp = ct%u;\n", u);
                else FF_APPEND("  cp = ct%u; ap = at%u;\n", u, u);
                break;
            case GL_MODULATE:
                if (b == FF_BASE_A) FF_APPEND("  ap *= at%u;\n", u);
                else if (b == FF_BASE_L || b == FF_BASE_RGB) FF_APPEND("  cp *= ct%u;\n", u);
                else FF_APPEND("  cp *= ct%u; ap *= at%u;\n", u, u);
                break;
            case GL_DECAL:
                if (b == FF_BASE_RGB) FF_APPEND("  cp = ct%u;\n", u);
                else if (b == FF_BASE_RGBA) FF_APPEND("  cp = mix(cp, ct%u, at%u);\n", u, u);
                break;
            case GL_BLEND:
                if (b == FF_BASE_A) FF_APPEND("  ap *= at%u;\n", u);
                else if (b == FF_BASE_L || b == FF_BASE_RGB) FF_APPEND("  cp = mix(cp, unit_env_color[%u].rgb, ct%u);\n", u, u);
                else if (b == FF_BASE_I) FF_APPEND("  cp = mix(cp, unit_env_color[%u].rgb, ct%u); ap = mix(ap, unit_env_color[%u].a, at%u);\n", u, u, u, u);
                else FF_APPEND("  cp = mix(cp, unit_env_color[%u].rgb, ct%u); ap *= at%u;\n", u, u, u);
                break;
            case GL_ADD:
                if (b == FF_BASE_A) FF_APPEND("  ap *= at%u;\n", u);
                else if (b == FF_BASE_L || b == FF_BASE_RGB) FF_APPEND("  cp = min(cp + ct%u, 1.0);\n", u);
                else if (b == FF_BASE_I) FF_APPEND("  cp = min(cp + ct%u, 1.0); ap = min(ap + at%u, 1.0);\n", u, u);
                else FF_APPEND("  cp = min(cp + ct%u, 1.0); ap *= at%u;\n", u, u);
                break;
            case GL_COMBINE: {
                FF_APPEND("  bool dot3_alpha = false; vec3 cp_in = cp; float ap_in = ap;\n");
                // Both terms read the previous values: compute alpha from saved inputs.
                char tmp[2048]; size_t tused = 0;
                if (!ff_combine(tmp, sizeof tmp, &tused, t, u, 0)) return 0;
                FF_APPEND("%s", tmp);
                FF_APPEND("  if (!dot3_alpha) { vec3 cp_rgb = cp; cp = cp_in; float ap_save = ap; ap = ap_in;\n");
                tused = 0; if (!ff_combine(tmp, sizeof tmp, &tused, t, u, 1)) return 0;
                FF_APPEND("%s   cp = cp_rgb; }\n", tmp);
                break;
            }
        }
        FF_APPEND(" }\n");
    }
    FF_APPEND(" vec4 c = vec4(cp, ap);\n");
    if (k->bits & FF_SEPARATE_SPECULAR) FF_APPEND(" c.rgb = min(c.rgb + lr_v_specular.rgb, 1.0);\n");
    if (k->bits & FF_ALPHA_TEST) FF_APPEND(" { float a = c.a, r = params.y; if (!(%s)) discard; }\n", ff_compare(k->alpha_func));
    if (k->bits & FF_FOG) {
        if (k->fog_mode == GL_LINEAR) FF_APPEND(" float f = (params.w - lr_v_fog_depth) * fog.y;\n");
        else if (k->fog_mode == GL_EXP2) FF_APPEND(" float f = exp(-(fog.x * lr_v_fog_depth) * (fog.x * lr_v_fog_depth));\n");
        else FF_APPEND(" float f = exp(-fog.x * lr_v_fog_depth);\n");
        FF_APPEND(" c.rgb = mix(fog_color.rgb, c.rgb, clamp(f, 0.0, 1.0));\n");
    }
    FF_APPEND(" lr_fragment = c;\n}\n");
    return 1;
}
// Program for a key: cached per share group; Mesa compiles it, the bridge's
// glLinkProgram hook captures its sources like any application program.
static GLuint ff_program(struct bridge_context *c, const struct ff_key *key) {
    pthread_mutex_lock(&ff_lock);
    for (unsigned i = 0; i < FF_PROGRAMS; i++)
        if (ff_programs[i].program && ff_programs[i].group == c->group && !memcmp(&ff_programs[i].key, key, sizeof *key)) { GLuint p = ff_programs[i].program; pthread_mutex_unlock(&ff_lock); return p; }
    pthread_mutex_unlock(&ff_lock);
    static char vs[16384], fs[8192];
    if (!ff_vertex_source(key, vs, sizeof vs) || !ff_fragment_source(key, fs, sizeof fs)) return 0;
    const char *sources[2] = { vs, fs }; GLenum stages[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLuint program = glCreateProgram(), shaders[2] = { 0, 0 };
    for (unsigned i = 0; i < 2; i++) {
        shaders[i] = glCreateShader(stages[i]); glShaderSource(shaders[i], 1, &sources[i], NULL); glCompileShader(shaders[i]);
        GLint ok = 0; glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512] = { 0 }; glGetShaderInfoLog(shaders[i], sizeof log, NULL, log); note("ff_compile_failed stage=%u log=%s", i, log); goto fail; }
        glAttachShader(program, shaders[i]);
    }
    glObjectLabel(GL_PROGRAM, program, -1, "LinuxRunner fixed function");
    glLinkProgram(program);   // the bridge hook: captures the sources for Metal
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { char log[512] = { 0 }; glGetProgramInfoLog(program, sizeof log, NULL, log); note("ff_link_failed log=%s", log); goto fail; }
    GLuint block = glGetUniformBlockIndex(program, "LRFixedFunction"); if (block != GL_INVALID_INDEX) glUniformBlockBinding(program, block, 0);
    GLint sampler = -1;
    for (unsigned u = 0; u < FF_UNITS; u++) { char name[16]; snprintf(name, sizeof name, "lr_texture%u", u); GLint at = glGetUniformLocation(program, name); if (at >= 0) glProgramUniform1i(program, at, (GLint)u); if (!u) sampler = at; }
    pthread_mutex_lock(&ff_lock);
    unsigned slot = ff_programs_next++ % FF_PROGRAMS;
    if (ff_programs[slot].program) glDeleteProgram(ff_programs[slot].program);   // same share group: least recently created
    ff_programs[slot].group = c->group; ff_programs[slot].key = *key; ff_programs[slot].program = program; ff_programs[slot].sampler = sampler;
    pthread_mutex_unlock(&ff_lock);
    note("ff_program program=%u lights=%x units=%u%u%u%u env0=%x fog=%x alpha=%x bits=%x", program, key->lights, key->unit[0].enabled, key->unit[1].enabled, key->unit[2].enabled, key->unit[3].enabled, key->unit[0].env, key->fog_mode, key->alpha_func, key->bits);
    return program;
fail:
    for (unsigned i = 0; i < 2; i++) if (shaders[i]) glDeleteShader(shaders[i]);
    glDeleteProgram(program); return 0;
}
// Fixed-function draw -> program to use (0 and a reason when Mesa keeps it).
static GLuint ff_prepare(struct bridge_context *c, GLenum mode, const char **reason) {
    struct ff_key key; const char *why = ff_collect(&key, &ff_block_current, mode);
    if (why) { *reason = why; return 0; }
    for (unsigned u = 0; u < FF_UNITS; u++) ff_vertex_units[u] = key.unit[u].enabled;
    ff_vertex_normal = (key.bits & FF_LIGHTING) != 0;
    for (unsigned u = 0; u < FF_UNITS; u++) for (unsigned k = 0; k < 4; k++) if (key.unit[u].texgen[k] == FF_GEN_SPHERE) ff_vertex_normal = 1;
    GLuint program = ff_program(c, &key);
    if (!program) { *reason = "ff:program"; return 0; }
    return program;
}
// ---- vertices --------------------------------------------------------------
// One fixed-function array (or the current value when disabled) into attribute
// `location` of every vertex in the packet.
static int ff_array(struct lr_gl_draw *draw, unsigned location, GLenum array, GLenum size_q, GLenum type_q, GLenum stride_q, GLenum pointer_q, GLenum buffer_q,
                    GLenum current_q, int fixed_size, int normalized_integers, const float defaults[4]) {
    float value[4]; memcpy(value, defaults, sizeof value);
    if (!glIsEnabled(array)) {
        if (current_q) glGetFloatv(current_q, value);
        if (current_q == GL_CURRENT_NORMAL) value[3] = 0;
        for (unsigned j = 0; j < draw->count; j++) memcpy(draw->vertex[j].attribute[location], value, 16);
        return 1;
    }
    GLint size = fixed_size, type = GL_FLOAT, stride = 0, buffer = 0; void *pointer = NULL;
    if (size_q) glGetIntegerv(size_q, &size);
    glGetIntegerv(type_q, &type); glGetIntegerv(stride_q, &stride); glGetIntegerv(buffer_q, &buffer); glGetPointerv(pointer_q, &pointer);
    int bgra = size == GL_BGRA; if (bgra) { if (type != GL_UNSIGNED_BYTE) return 0; size = 4; }   // GL_BGRA arrays: 4 bytes, red and blue swapped
    unsigned cb = component_bytes(type); if (!cb || size < 1 || size > 4) return 0;
    size_t element = (size_t)cb * size; if (!stride) stride = (GLint)element;
    if (!draw->count) return 1;
    size_t bytes = (size_t)(draw->count - 1) * stride + element; if (bytes > 8u * 1024u * 1024u) return 0;
    const unsigned char *data; unsigned char *copy = NULL;
    size_t start = (size_t)draw_vertex_base * (size_t)stride;   // the packet's first vertex
    if (buffer) { copy = malloc(bytes); if (!copy || !read_buffer_bytes(buffer, (uintptr_t)pointer + start, bytes, copy)) { free(copy); return 0; } data = copy; }
    else { if (!pointer) return 0; data = (const unsigned char *)pointer + start; }   // client memory of this process (compatibility contexts)
    for (unsigned j = 0; j < draw->count; j++) {
        float *v = draw->vertex[j].attribute[location]; memcpy(v, defaults, 16);
        attribute_value(data + (size_t)j * stride, type, normalized_integers && type != GL_FLOAT, 0, v, size);
        if (bgra) { float t = v[0]; v[0] = v[2]; v[2] = t; }
    }
    free(copy); return 1;
}
static int ff_vertices(struct lr_gl_draw *draw) {
    static const float position[4] = { 0, 0, 0, 1 }, normal[4] = { 0, 0, 1, 0 }, color[4] = { 1, 1, 1, 1 }, texcoord[4] = { 0, 0, 0, 1 };
    if (!ff_array(draw, 0, GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE, GL_VERTEX_ARRAY_POINTER, GL_VERTEX_ARRAY_BUFFER_BINDING, 0, 0, 0, position)) return 0;
    if (ff_vertex_normal && !ff_array(draw, 1, GL_NORMAL_ARRAY, 0, GL_NORMAL_ARRAY_TYPE, GL_NORMAL_ARRAY_STRIDE, GL_NORMAL_ARRAY_POINTER, GL_NORMAL_ARRAY_BUFFER_BINDING, GL_CURRENT_NORMAL, 3, 1, normal)) return 0;
    if (!ff_array(draw, 2, GL_COLOR_ARRAY, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE, GL_COLOR_ARRAY_POINTER, GL_COLOR_ARRAY_BUFFER_BINDING, GL_CURRENT_COLOR, 0, 1, color)) return 0;
    GLint client = GL_TEXTURE0, active = GL_TEXTURE0; glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &client); glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    int ok = 1;
    for (unsigned u = 0; u < FF_UNITS && ok; u++) {
        if (!ff_vertex_units[u]) continue;
        glClientActiveTexture(GL_TEXTURE0 + u); glActiveTexture(GL_TEXTURE0 + u);
        ok = ff_array(draw, 3 + u, GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY_POINTER, GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING, GL_CURRENT_TEXTURE_COORDS, 0, 0, texcoord);
    }
    glClientActiveTexture(client); glActiveTexture(active);
    return ok;
}
