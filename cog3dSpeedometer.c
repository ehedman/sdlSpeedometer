/*
 * SDL2 + OpenGL ES 2.0 3D marine compass
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>
#include <SDL2/SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "compass3d.h"
#include "sdlSpeedometer.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VERTS 120000
#define MAX_TEXT_VERTS 4096
#define PI_F 3.14159265358979323846f

typedef struct { float x,y,z; } V3;
typedef struct { float x,y,z,r,g,b,a; } Vertex;
typedef struct { float x,y,z,u,v; } TextVertex;
static Vertex verts[MAX_VERTS];
static int vert_count = 0;
static GLuint program, vbo;
static GLuint text_program, text_vbo, text_texture;
static GLint text_uni_mvp, text_uni_tex;
static TextVertex text_verts[MAX_TEXT_VERTS];
static int text_vert_count = 0;
static TTF_Font *font = NULL;
static const char atlas_chars[] = "0123456789NESW";
static const int atlas_cell_w = 64, atlas_cell_h = 80;
static GLint attr_pos, attr_col, uni_mvp;
static float render_heading_deg = 0.0f;
/* Wind angle relative to the boat/compass rose: 0=bow, 90=starboard. */
static float wind_angle_deg = 0.0f;
static float course_angle_deg = 0.0f;
/* Rudder indicator angle, independent of heading and wind. 0=bow, 90=starboard. */
static float rudder_angle_deg = 0.0f;
static float render_heel_deg = 0.0f;
static float camera_tilt_deg = 62.0f;
static float camera_distance = 4.2f;
static float compass_scale = 1.0f;
static int renderer_initialized = 0;
static int first_run = 1;
static int torsional_load;
static float target_course;
static int do_wind = 0;
static int do_rudd = 0;
static int do_cog = 0;
static float compass_angle = 0.0f;
static float target_limit = 2;
int heading_ok = 0;
int course_ok = 0;


static V3 v3(float x,float y,float z){ V3 v={x,y,z}; return v; }
static V3 sub(V3 a,V3 b){return v3(a.x-b.x,a.y-b.y,a.z-b.z);}
static V3 mul(V3 a,float s){return v3(a.x*s,a.y*s,a.z*s);}
static float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static V3 cross(V3 a,V3 b){return v3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
static V3 norm(V3 a){float l=sqrtf(dot(a,a)); return l>1e-6f?mul(a,1.0f/l):a;}

static void vertex(V3 p, float r,float g,float b,float a) {
    if (vert_count >= MAX_VERTS) return;
    verts[vert_count++] = (Vertex){p.x,p.y,p.z,r,g,b,a};
}

static void tri(V3 a,V3 b,V3 c,float r,float g,float bcol) {
    vertex(a,r,g,bcol,1); vertex(b,r,g,bcol,1); vertex(c,r,g,bcol,1);
}

static void quad(V3 a,V3 b,V3 c,V3 d,float r,float g,float bl) {
    tri(a,b,c,r,g,bl); tri(a,c,d,r,g,bl);
}

static void stroke3(V3 a,V3 b,float half_width,float r,float g,float bl) {
    /* Antialiasing is provided by the multisampled SDL/OpenGL surface where available. */
    float dx=b.x-a.x, dy=b.y-a.y;
    float len=sqrtf(dx*dx+dy*dy);
    if(len < 1e-6f) return;
    float ox=-dy/len*half_width, oy=dx/len*half_width;
    V3 p0=v3(a.x+ox,a.y+oy,a.z), p1=v3(b.x+ox,b.y+oy,b.z);
    V3 p2=v3(b.x-ox,b.y-oy,b.z), p3=v3(a.x-ox,a.y-oy,a.z);
    quad(p0,p1,p2,p3,r,g,bl);
}

static void line3(V3 a,V3 b,float r,float g,float bl) {
    stroke3(a,b,0.0050f,r,g,bl);
}

static V3 polar(float radius,float angle,float z) {
    return v3(radius*sinf(angle), radius*cosf(angle), z);
}

/* Set apparent-wind direction relative to the boat. Accept any value and
   normalize it to [0, 360). */
static void compass3d_set_wind_angle(float degrees) {

    if (!do_wind) return;

    wind_angle_deg = fmodf(degrees, 360.0f);
    if (wind_angle_deg < 0.0f) wind_angle_deg += 360.0f;
}

/* Set apparent-wind direction relative to the boat. Accept any value and
   normalize it to [0, 360). */
static void compass3d_set_course_angle(float degrees) {

    course_angle_deg = fmodf(degrees, 360.0f);
    if (course_angle_deg < 0.0f) course_angle_deg += 360.0f;
}

/* Set rudder indicator angle in degrees; 0=bow, 90=starboard. */
static void compass3d_set_rudder_angle(float degrees) {

    if (!do_rudd) return;

    rudder_angle_deg = fmodf(degrees, 360.0f);
    if (rudder_angle_deg < 0.0f) rudder_angle_deg += 360.0f;
}

static GLuint compile_shader(GLenum type,const char *src);

static const char *text_vs_src =
    "attribute vec3 aPos; attribute vec2 aUV; uniform mat4 uMVP; varying vec2 vUV;"
    "void main(){ gl_Position=uMVP*vec4(aPos,1.0); vUV=aUV; }";

static const char *text_fs_src =
    "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;"
    "void main(){ vec4 c=texture2D(uTex,vUV); gl_FragColor=c; }";

static void push_text_quad(float cx,float cy,float z,float w,float h,int char_index,float angle_deg) {
    if(text_vert_count + 6 > MAX_TEXT_VERTS) return;
    float u0=(float)(char_index*atlas_cell_w)/(float)(sizeof(atlas_chars)-1)/(float)(1); /* overwritten below */
    (void)u0;
    /* Atlas is a single horizontal row of equally sized character cells. */
    float atlas_w=(float)(strlen(atlas_chars)*atlas_cell_w);
    float u_left=(float)(char_index*atlas_cell_w)/atlas_w;
    float u_right=(float)((char_index+1)*atlas_cell_w)/atlas_w;
    float v_top=0.0f, v_bottom=1.0f;
    float x0=cx-w*0.5f, x1=cx+w*0.5f;
    float y0=cy-h*0.5f, y1=cy+h*0.5f;
    TextVertex q[6]={
        {x0,y1,z,u_left,v_top},{x0,y0,z,u_left,v_bottom},{x1,y0,z,u_right,v_bottom},
        {x0,y1,z,u_left,v_top},{x1,y0,z,u_right,v_bottom},{x1,y1,z,u_right,v_top}
    };
    /* Rotate each glyph in the compass dial's own XY plane. This makes
       the label baseline tangent to the dial, hence perpendicular to its
       radial degree tick. The complete label remains attached to the rose. */
    float a=angle_deg*PI_F/180.0f, cs=cosf(a), sn=sinf(a);
    for(int i=0;i<6;i++) {
        float dx=q[i].x-cx, dy=q[i].y-cy;
        q[i].x=cx+dx*cs-dy*sn;
        q[i].y=cy+dx*sn+dy*cs;
    }
    memcpy(&text_verts[text_vert_count],q,sizeof(q)); text_vert_count+=6;
}

static int atlas_index(char ch) {
    const char *p=strchr(atlas_chars,ch);
    return p ? (int)(p-atlas_chars) : -1;
}

static void add_dial_text(const char *str,float cx,float cy,float z,float height,float angle_deg) {
    float charw=height*0.62f;
    float total=(float)strlen(str)*charw;
    float x=cx-total*0.5f;
    float a=angle_deg*PI_F/180.0f, cs=cosf(a), sn=sinf(a);
    for(size_t i=0;i<strlen(str);i++) {
        int idx=atlas_index(str[i]);
        if(idx>=0) {
            /* Rotate the character's position around the label anchor too,
               so multi-digit labels rotate as a single word, not in place. */
            float gx=x+charw*0.5f+(float)i*charw;
            float dx=gx-cx;
            float rx=cx+dx*cs;
            float ry=cy+dx*sn;
            push_text_quad(rx,ry,z,charw,height,idx,angle_deg);
        }
    }
}

static int init_textures(void) {
    if(TTF_Init()!=0) {
         SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: TTF_Init: %s" ,TTF_GetError());
        return 1;
    }

    const char *paths[]={
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
        NULL
    };

    for(int i=0;paths[i];i++) { font=TTF_OpenFont(paths[i],56); if(font) break; }

    if(!font) {
        // Install fonts-dejavu-core
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: Could not open a system TTF font: %s" ,TTF_GetError());
        return 0;
    }

    SDL_Surface *atlas=SDL_CreateRGBSurfaceWithFormat(0,(int)strlen(atlas_chars)*atlas_cell_w,atlas_cell_h,32,SDL_PIXELFORMAT_RGBA32);

    if(!atlas) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: SDL_CreateRGBSurface: %s" ,TTF_GetError());
        return 0;
    }

    SDL_FillRect(atlas,NULL,SDL_MapRGBA(atlas->format,0,0,0,0));
    SDL_Color white={255,245,232,255};
    for(size_t i=0;i<strlen(atlas_chars);i++) {
        char str[2]={atlas_chars[i],0};
        SDL_Surface *glyph=TTF_RenderUTF8_Blended(font,str,white);
        if(!glyph) continue;
        SDL_Rect dst={(int)i*atlas_cell_w+(atlas_cell_w-glyph->w)/2,(atlas_cell_h-glyph->h)/2,glyph->w,glyph->h};
        SDL_BlitSurface(glyph,NULL,atlas,&dst);
        SDL_FreeSurface(glyph);
    }

    glGenTextures(1,&text_texture); glBindTexture(GL_TEXTURE_2D,text_texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,atlas->w,atlas->h,0,GL_RGBA,GL_UNSIGNED_BYTE,atlas->pixels);
    SDL_FreeSurface(atlas);
    GLuint vs=compile_shader(GL_VERTEX_SHADER,text_vs_src), fs=compile_shader(GL_FRAGMENT_SHADER,text_fs_src);
    if(!vs||!fs) return 0;

    text_program=glCreateProgram(); glAttachShader(text_program,vs); glAttachShader(text_program,fs);
    glBindAttribLocation(text_program,0,"aPos"); glBindAttribLocation(text_program,1,"aUV");
    glLinkProgram(text_program); glDeleteShader(vs); glDeleteShader(fs);
    GLint ok=0; glGetProgramiv(text_program,GL_LINK_STATUS,&ok);
    if(!ok) {
        char log[1024];
        glGetProgramInfoLog(text_program,sizeof(log),NULL,log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: Text shader link error: %s" ,log);
        return 0;
    }

    text_uni_mvp=glGetUniformLocation(text_program,"uMVP"); text_uni_tex=glGetUniformLocation(text_program,"uTex");
    glGenBuffers(1,&text_vbo);
    return 1;
}

static void build_compass(void) {
    vert_count=0;
    const int N=144;
    const float bodyR=1.28f, bottomZ=-0.22f, topZ=0.04f;
    const float redR=0.95f, domeH=0.12f;
    /* Dark underside and red cylindrical side, with subtle vertical facets. */
    for(int i=0;i<N;i++) {
        float a=2*PI_F*i/N, b=2*PI_F*(i+1)/N;
        V3 p0=polar(bodyR,a,bottomZ), p1=polar(bodyR,b,bottomZ);
        V3 p2=polar(bodyR,b,topZ-0.015f), p3=polar(bodyR,a,topZ-0.015f);
        float shade=0.045f+0.035f*(0.5f+0.5f*cosf((a+b)*0.5f));
        quad(p0,p1,p2,p3,shade,shade,shade+0.008f);
        V3 q0=polar(redR,a,-0.02f), q1=polar(redR,b,-0.02f);
        V3 q2=polar(redR,b,topZ), q3=polar(redR,a,topZ);
        float light=0.34f+0.12f*(0.5f+0.5f*cosf((a+b)*0.5f));
        quad(q0,q1,q2,q3,light,0.035f,0.065f);
    }

    /* Beveled outer rim: dark lip, then red sloping shoulder. */
    for(int i=0;i<N;i++) {
        float a=2*PI_F*i/N,b=2*PI_F*(i+1)/N;
        V3 p0=polar(1.25f,a,topZ-0.015f),p1=polar(1.25f,b,topZ-0.015f);
        V3 p2=polar(1.14f,b,topZ+0.045f),p3=polar(1.14f,a,topZ+0.045f);
        quad(p0,p1,p2,p3,0.20f,0.025f,0.045f);
        V3 q0=polar(1.14f,a,topZ+0.045f),q1=polar(1.14f,b,topZ+0.045f);
        V3 q2=polar(0.0f,b,topZ+0.045f+domeH),q3=polar(0.0f,a,topZ+0.045f+domeH);
        /* dome is filled separately below */
        (void)q0;(void)q1;(void)q2;(void)q3;
    }

    /* Slightly domed red top surface, triangulated as concentric rings. */
    const int rings=18;
    for(int j=0;j<rings;j++) {
        float r0=redR*j/rings, r1=redR*(j+1)/rings;
        for(int i=0;i<N;i++) {
            float a=2*PI_F*i/N,b=2*PI_F*(i+1)/N;
            float z00=topZ+domeH*(1-(r0/redR)*(r0/redR));
            float z10=topZ+domeH*(1-(r1/redR)*(r1/redR));
            V3 p0=polar(r0,a,z00),p1=polar(r0,b,z00);
            V3 p2=polar(r1,b,z10),p3=polar(r1,a,z10);
            float f=0.38f+0.13f*(1.0f-r1/redR);
            tri(p0,p2,p1,f,0.035f,0.075f);
            tri(p0,p3,p2,f,0.035f,0.075f);
        }
    }

    /* Decorative central red disk, slightly darker than the rose surface. */
    {
        const float hubR = 0.19f;
        const float hubZ = topZ + domeH + 0.014f;
        V3 center = v3(0.0f, 0.0f, hubZ);
        for (int i=0; i<N; i++) {
            float a = 2.0f*PI_F*(float)i/(float)N;
            float b = 2.0f*PI_F*(float)(i+1)/(float)N;
            V3 p = polar(hubR, a, hubZ);
            V3 q = polar(hubR, b, hubZ);
            tri(center, p, q, 0.30f, 0.025f, 0.055f);
        }
    }

    /* Rudder-angle needle at the centre. It rotates independently, but is
       built in compass coordinates so pitch/heel and the 3D model transform
       affect it exactly like the rose. The pointed end indicates the angle. */
    if (do_rudd) {
        const float a = rudder_angle_deg * PI_F / 180.0f;
        const float z = topZ + domeH + 0.030f;
        const float tailR = -0.105f;
        const float tipR = 0.255f;
        const float halfW = 0.026f;
        V3 tail = polar(tailR, a, z);
        V3 tip = polar(tipR, a, z);
        V3 mid = polar(0.145f, a, z + 0.002f);
        /* Perpendicular offset to the needle axis. */
        float px = cosf(a) * halfW;
        float py = -sinf(a) * halfW;
        V3 left = v3(mid.x + px, mid.y + py, z + 0.002f);
        V3 right = v3(mid.x - px, mid.y - py, z + 0.002f);
        /* Dark outline/underside, then a bright red needle and arrowhead. */
        V3 tipBack = polar(tipR - 0.065f, a, z + 0.003f);
        tri(tail, left, right, 0.10f, 0.015f, 0.020f);
        tri(left, tipBack, right, 0.10f, 0.015f, 0.020f);
        V3 shaftL = v3(mid.x + px*0.62f, mid.y + py*0.62f, z + 0.006f);
        V3 shaftR = v3(mid.x - px*0.62f, mid.y - py*0.62f, z + 0.006f);
        tri(tail, shaftL, shaftR, 1.0f, 0.12f, 0.10f);
        tri(shaftL, tipBack, shaftR, 1.0f, 0.12f, 0.10f);
        tri(tip, left, tipBack, 1.0f, 0.12f, 0.10f);
        tri(tip, tipBack, right, 1.0f, 0.12f, 0.10f);
    }

    /* Tick marks follow the domed surface. North is toward +Y. */
    for(int deg=0;deg<360;deg+=5) {
        float a=deg*PI_F/180.0f;
        float inner=0.78f, outer=0.91f;
        float cr=0.96f, cg=0.84f, cb=0.43f;
        if(deg%30==0) { inner=0.66f; outer=0.90f; }
        else if(deg%10==0) inner=0.73f;
        /* Give the north index a distinctive shape/color so rotation is easy
         * to see even though the dial has many similar 5-degree ticks. */
        if(deg==0) { inner=0.53f; outer=0.94f; cr=1.0f; cg=0.20f; cb=0.12f; }
        float r0=inner, r1=outer;
        float z0=topZ+domeH*(1-(r0/redR)*(r0/redR))+0.009f;
        float z1=topZ+domeH*(1-(r1/redR)*(r1/redR))+0.009f;
        line3(polar(r0,a,z0),polar(r1,a,z1),cr,cg,cb);
    }

    if (target_course >= 0 && do_cog) {  // Crosshair Sight
        float course_angle_deg = compass_angle;     // Adjusted angle of the compass rose
        float lck_tolerance_deg = target_limit;     // The tolerance in degrees (from the LCK text)

        // Dexide on the color
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;

        if (course_ok) {
            // ON COURSE: Change the color to Green (R=0.0, G=1.0, B=0.0)
            r = 0.0f;
            g = 1.0f;
            b = 0.0f;
        } else {
            // OFF COURSE: Keep White (R=1.0, G=1.0, B=1.0)
            r = 1.0f;
            g = 1.0f;
            b = 1.0f;
        }

        // --- SETTINGS FOR COMPASS GEOMETRY ---
        const float markerR = 1.045f;
        const float markerZ = topZ + 0.078f;

        // --- CALCULATION OF DYNAMIC DIAMETER ---
        const float units_per_degree = markerR * (PI_F / 180.0f);
        const float circleDiameter = (lck_tolerance_deg * 2.0f) * units_per_degree;
        const float badgeR = circleDiameter / 2.0f;

        // --- CALCULATION OF POSITION (Fixed at the top) ---
        const float a = (0.0f - course_angle_deg) * PI_F / 180.0f;
        V3 c = polar(markerR, a, markerZ);

        // --- DRAWING OF THE RING ---
        for (int i = 0; i < 36; i++) {
            float a0 = 2.0f * PI_F * (float)i / 36.0f;
            float a1 = 2.0f * PI_F * (float)(i + 1) / 36.0f;

            V3 p = v3(c.x + badgeR * cosf(a0), c.y + badgeR * sinf(a0), markerZ);
            V3 q = v3(c.x + badgeR * cosf(a1), c.y + badgeR * sinf(a1), markerZ);

            line3(p, q, r, g, b); // Använder vår dynamiska färg här
        }

        // --- DRAWING OF THE CROSSHAIR LINES ---
        const float crosshairLength = 0.030f;
        float crossAngles[4] = { 0.0f, PI_F / 2.0f, PI_F, 3.0f * PI_F / 2.0f };

        for (int j = 0; j < 4; j++) {
            float ang = crossAngles[j];

            V3 p_start = v3(c.x + (badgeR - crosshairLength * 0.3f) * cosf(ang),
                            c.y + (badgeR - crosshairLength * 0.3f) * sinf(ang),
                            markerZ);

            V3 p_end   = v3(c.x + (badgeR + crosshairLength) * cosf(ang),
                            c.y + (badgeR + crosshairLength) * sinf(ang),
                            markerZ);

            line3(p_start, p_end, r, g, b); // Using our dynamic color here
        }
    }

    /* Wind-angle marker on the outer rim. Its angle is independent of heading;
       the model transform makes it follow the compass's 3D geometry. The orange
       badge has a pointed tip directed radially inward, toward the dial centre. */
    if (do_wind) {
        const float a = wind_angle_deg * PI_F / 180.0f;
        const float markerR = 1.045f;
        const float markerZ = topZ + 0.078f;
        const float badgeR = 0.070f;
        V3 c = polar(markerR, a, markerZ);
        for (int i=0; i<24; i++) {
            float a0 = 2.0f*PI_F*(float)i/24.0f;
            float a1 = 2.0f*PI_F*(float)(i+1)/24.0f;
            V3 p = v3(c.x + badgeR*cosf(a0), c.y + badgeR*sinf(a0), markerZ);
            V3 q = v3(c.x + badgeR*cosf(a1), c.y + badgeR*sinf(a1), markerZ);
            tri(c, p, q, 1.0f, 0.39f, 0.025f);
        }
        /* Inward-pointing arrowhead, drawn over the orange badge. */
        V3 tip = polar(markerR - 0.060f, a, markerZ + 0.004f);
        V3 base = polar(markerR + 0.030f, a, markerZ + 0.004f);
        float tx = cosf(a), ty = -sinf(a); /* tangent to the circle */
        V3 left  = v3(base.x + tx*0.043f, base.y + ty*0.043f, markerZ + 0.004f);
        V3 right = v3(base.x - tx*0.043f, base.y - ty*0.043f, markerZ + 0.004f);
        tri(tip, left, right, 0.12f, 0.055f, 0.015f);
    }

    if (target_course >= 0 && do_cog ) {  // As do_wind but for course settings and with yellow badge
        const float a = course_angle_deg * PI_F / 180.0f;
        const float markerR = 1.045f;
        const float markerZ = topZ + 0.078f;
        const float badgeR = 0.070f;
        V3 c = polar(markerR, a, markerZ);
        for (int i=0; i<24; i++) {
            float a0 = 2.0f*PI_F*(float)i/24.0f;
            float a1 = 2.0f*PI_F*(float)(i+1)/24.0f;
            V3 p = v3(c.x + badgeR*cosf(a0), c.y + badgeR*sinf(a0), markerZ);
            V3 q = v3(c.x + badgeR*cosf(a1), c.y + badgeR*sinf(a1), markerZ);
            tri(c, p, q, 0.0f, 1.0f, 0.0f);
        }
        /* Inward-pointing arrowhead, drawn over the orange badge. 0.360f=pilens längd*/
        V3 tip = polar(markerR - 0.360f, a, markerZ + 0.004f);
        V3 base = polar(markerR + 0.030f, a, markerZ + 0.004f);
        float tx = cosf(a), ty = -sinf(a); /* tangent to the circle */
        V3 left  = v3(base.x + tx*0.043f, base.y + ty*0.043f, markerZ + 0.004f);
        V3 right = v3(base.x - tx*0.043f, base.y - ty*0.043f, markerZ + 0.004f);
        tri(tip, left, right, 0.0f, 0.3f, 1.0f);
    }

    /* Small, plain degree labels directly on the red compass surface. */
    text_vert_count=0;
    for(int deg=0;deg<360;deg+=30) {
        float a=deg*PI_F/180.0f, rr=0.62f;
        V3 p=polar(rr,a,topZ+domeH*(1-(rr/redR)*(rr/redR))+0.010f);
        char buf[4];
        snprintf(buf,sizeof(buf),"%d",deg);
        /* Keep each label's position attached to the rose, but keep the
           glyphs readable on screen as heading rotates the rose. The baseline
           is tangent to the dial and is flipped by 180 degrees when needed. */
        float screen_angle = -(float)deg - render_heading_deg;
        while (screen_angle > 90.0f) screen_angle -= 180.0f;
        while (screen_angle < -90.0f) screen_angle += 180.0f;
        float text_angle = screen_angle + render_heading_deg;
        add_dial_text(buf,p.x,p.y,p.z,0.090f,text_angle);
    }

    /* Cardinal directions on the inner ring. Their positions rotate with the
       rose; their letters remain upright for the chart-table viewing angle. */
    const char *cardinal[] = {"N", "E", "S", "W"};
    const int cardinal_deg[] = {0, 90, 180, 270};
    for (int i=0; i<4; i++) {
        int deg = cardinal_deg[i];
        float a = (float)deg * PI_F / 180.0f;
        float rr = 0.39f;
        V3 p = polar(rr, a, topZ+domeH*(1-(rr/redR)*(rr/redR))+0.012f);
        float screen_angle = -(float)deg - render_heading_deg;
        while (screen_angle > 90.0f) screen_angle -= 180.0f;
        while (screen_angle < -90.0f) screen_angle += 180.0f;
        float text_angle = screen_angle + render_heading_deg;
        add_dial_text(cardinal[i], p.x, p.y, p.z, 0.120f, text_angle);
    }

}

static const char *vs_src =
    "attribute vec3 aPos; attribute vec4 aCol; uniform mat4 uMVP; varying vec4 vCol;"
    "void main(){ gl_Position=uMVP*vec4(aPos,1.0); vCol=aCol; }";

static const char *fs_src =
    "precision mediump float; varying vec4 vCol;"
    "void main(){ gl_FragColor=vCol; }";

static GLuint compile_shader(GLenum type,const char *src) {
    GLuint s=glCreateShader(type); glShaderSource(s,1,&src,NULL); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){
        char log[2048];
        glGetShaderInfoLog(s,sizeof(log),NULL,log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: Shader error:  %s" ,log);
        return 0;
    }
    return s;
}

static int init_gl(void) {
    GLuint vs=compile_shader(GL_VERTEX_SHADER,vs_src), fs=compile_shader(GL_FRAGMENT_SHADER,fs_src);
    if(!vs||!fs)return 0;
    program=glCreateProgram(); glAttachShader(program,vs); glAttachShader(program,fs);
    glBindAttribLocation(program,0,"aPos"); glBindAttribLocation(program,1,"aCol");
    glLinkProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok){
        char log[2048];
        glGetProgramInfoLog(program,sizeof(log),NULL,log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: Link error: %s" ,log);
        return 0;
    }
    attr_pos=glGetAttribLocation(program,"aPos"); attr_col=glGetAttribLocation(program,"aCol"); uni_mvp=glGetUniformLocation(program,"uMVP");
    glGenBuffers(1,&vbo);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glClearColor(0.025f,0.028f,0.035f,1);
    return 1;
}

static void mat_identity(float m[16]){memset(m,0,16*sizeof(float));m[0]=m[5]=m[10]=m[15]=1;}

static void mat_mul(float out[16],const float a[16],const float b[16]){
    float t[16];
    for(int c=0;c<4;c++)for(int r=0;r<4;r++)t[c*4+r]=a[0*4+r]*b[c*4+0]+a[1*4+r]*b[c*4+1]+a[2*4+r]*b[c*4+2]+a[3*4+r]*b[c*4+3];
    memcpy(out,t,sizeof(t));
}

static void rotate_x(float m[16],float deg){float a=deg*PI_F/180,c=cosf(a),s=sinf(a);mat_identity(m);m[5]=c;m[6]=s;m[9]=-s;m[10]=c;}
static void rotate_y(float m[16],float deg){float a=deg*PI_F/180,c=cosf(a),s=sinf(a);mat_identity(m);m[0]=c;m[2]=-s;m[8]=s;m[10]=c;}
static void rotate_z(float m[16],float deg){float a=deg*PI_F/180,c=cosf(a),s=sinf(a);mat_identity(m);m[0]=c;m[1]=s;m[4]=-s;m[5]=c;}
static void perspective(float m[16],float fovy,float aspect,float zn,float zf){
    memset(m,0,16*sizeof(float));float f=1.0f/tanf(fovy*PI_F/360.0f);
    m[0]=f/aspect;m[5]=f;m[10]=(zf+zn)/(zn-zf);m[11]=-1;m[14]=(2*zf*zn)/(zn-zf);
}

static void look_at(float m[16],V3 eye,V3 center,V3 up){
    V3 f=norm(sub(center,eye)),s=norm(cross(f,up)),u=cross(s,f);
    mat_identity(m);m[0]=s.x;m[4]=s.y;m[8]=s.z;m[1]=u.x;m[5]=u.y;m[9]=u.z;m[2]=-f.x;m[6]=-f.y;m[10]=-f.z;
    m[12]=-dot(s,eye);m[13]=-dot(u,eye);m[14]=dot(f,eye);
}

static int compass3d_init(void) {
    if (renderer_initialized) return 1;
    if (!TTF_WasInit() && TTF_Init()!=0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: TTF_Init: %s" ,TTF_GetError());
        return 0;
    }
    if (!init_gl() || !init_textures()) {
        compass3d_shutdown();
        return 0;
    }
    renderer_initialized = 1;
    return 1;
}

static void compass3d_set_view(float tilt_degrees, float distance) {
    camera_tilt_deg = fmaxf(25.0f, fminf(82.0f, tilt_degrees));
    camera_distance = fmaxf(2.0f, fminf(10.0f, distance));
}

static void compass3d_render(float heading_degrees, float heel_degrees, float pitch_degrees, int x, int y, int width, int height)
{
    if (!renderer_initialized || width <= 0 || height <= 0)
        return;

    render_heading_deg = (float)heading_degrees;
    render_heel_deg    = (float)heel_degrees;
    float render_pitch_deg   = (float)pitch_degrees;

    GLint old_viewport[4];
    glGetIntegerv(GL_VIEWPORT, old_viewport);

    glViewport(x, y, width, height);

    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    build_compass();

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(vert_count * sizeof(Vertex)),
                 verts, GL_DYNAMIC_DRAW);

    glUseProgram(program);

    glEnableVertexAttribArray((GLuint)attr_pos);
    glEnableVertexAttribArray((GLuint)attr_col);

    glVertexAttribPointer((GLuint)attr_pos, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex), (void*)offsetof(Vertex, x));

    glVertexAttribPointer((GLuint)attr_col, 4, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex), (void*)offsetof(Vertex, r));

    float proj[16], view[16];
    float rx[16], ry[16], rz[16];
    float model[16], tmp[16], mvp[16];

    float aspect = (float)width / (float)height;

    perspective(proj, 38.0f, aspect, 0.1f, 30.0f);

    float tilt = camera_tilt_deg;

    V3 eye = v3(
        0.0f,
        -camera_distance * cosf((90.0f - tilt) * PI_F / 180.0f),
         camera_distance * sinf((90.0f - tilt) * PI_F / 180.0f)
    );

    look_at(view, eye, v3(0, 0, 0), v3(0, 0, 1));

    /*
     * Heading rotates around the vertical Z axis.
     * Heel and pitch use opposite signs to simulate
     * compensation for the boat's roll and pitch.
     */
    rotate_x(rx, -render_heel_deg);
    rotate_y(ry, -render_pitch_deg);
    rotate_z(rz, -render_heading_deg);

    float scale_m[16];
    mat_identity(scale_m);

    scale_m[0]  = compass_scale;
    scale_m[5]  = compass_scale;
    scale_m[10] = compass_scale;

    /* Compose the model transformations. */
    mat_mul(tmp, rz, scale_m);
    mat_mul(model, ry, tmp);
    mat_mul(tmp, rx, model);
    memcpy(model, tmp, sizeof(model));

    mat_mul(tmp, view, model);
    mat_mul(mvp, proj, tmp);

    glUniformMatrix4fv(uni_mvp, 1, GL_FALSE, mvp);
    glDrawArrays(GL_TRIANGLES, 0, vert_count);

    /* Draw compass labels using the same model transformation. */
    if (text_vert_count > 0) {
        glUseProgram(text_program);

        glBindBuffer(GL_ARRAY_BUFFER, text_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(text_vert_count * sizeof(TextVertex)),
                     text_verts, GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);

        glVertexAttribPointer(
            0, 3, GL_FLOAT, GL_FALSE, sizeof(TextVertex),
            (void*)offsetof(TextVertex, x));

        glVertexAttribPointer(
            1, 2, GL_FLOAT, GL_FALSE, sizeof(TextVertex),
            (void*)offsetof(TextVertex, u));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, text_texture);
        glUniform1i(text_uni_tex, 0);
        glUniformMatrix4fv(text_uni_mvp, 1, GL_FALSE, mvp);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glDrawArrays(GL_TRIANGLES, 0, text_vert_count);

        glDisable(GL_BLEND);
    }

    glViewport(old_viewport[0], old_viewport[1],
               old_viewport[2], old_viewport[3]);
}

static void compass3d_shutdown(void) {
    if (vbo) glDeleteBuffers(1,&vbo);
    if (text_vbo) glDeleteBuffers(1,&text_vbo);
    if (text_texture) glDeleteTextures(1,&text_texture);
    if (program) glDeleteProgram(program);
    if (text_program) glDeleteProgram(text_program);
    vbo=text_vbo=text_texture=program=text_program=0;
    if (font) { TTF_CloseFont(font); font=NULL; }
    renderer_initialized=0;
}

// Simple shaders for rendering 2D text
static const char* vertex_shader_source =
    "attribute vec4 position;\n"
    "attribute vec2 texCoord;\n"
    "varying vec2 v_texCoord;\n"
    "uniform vec2 u_resolution;\n" // Window width and height in pixels
    "void main() {\n"
    // Convert from pixels (0 to width/height) to OpenGL coordinates (-1.0 to 1.0)
    "   vec2 zeroToOne = position.xy / u_resolution;\n"
    "   vec2 zeroToTwo = zeroToOne * 2.0;\n"
    "   vec2 clipSpace = zeroToTwo - 1.0;\n"
    "   gl_Position = vec4(clipSpace * vec2(1.0, -1.0), 0.0, 1.0);\n" // Invert Y to place (0,0) at the top left
    "   v_texCoord = texCoord;\n"
    "}\n";

static const char* fragment_shader_source =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "   gl_FragColor = texture2D(u_texture, v_texCoord);\n"
    "}\n";

static GLuint create_text_texture(TTF_Font* font, const char* text, SDL_Color color, int* width, int* height) {
    SDL_Surface* initial_surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!initial_surface) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: Could not render text:  %s" ,TTF_GetError());
        return 0;
    }

    // Use ABGR8888 to match OpenGL ES's internal memory layout for GL_RGBA
    SDL_Surface* surface = SDL_ConvertSurfaceFormat(initial_surface, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(initial_surface);
    
    if (!surface) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: Could not convert pixel format:  %s" ,TTF_GetError());
        return 0;
    }

    *width = surface->w;
    *height = surface->h;

    GLuint texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // We keep GL_RGBA and GL_UNSIGNED_BYTE here; the ABGR conversion above ensures that the channels match.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->w, surface->h, 0, 
                 GL_RGBA, GL_UNSIGNED_BYTE, surface->pixels);

    SDL_FreeSurface(surface);

    return texture_id;
}

// This structure keeps track of where the shader's variables are located.
typedef struct {
    GLuint program;
    GLint position_loc;
    GLint texCoord_loc;
    GLint resolution_loc;
    GLint texture_loc;
} TextShader;

static void draw_text_gles2(TextShader shader, GLuint texture_id, float x, float y, float width, float height, float screen_width, float screen_height) {
    // Restore any VBO buffers from the compass
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Enable shader and texture
    glUseProgram(shader.program);
    glUniform2f(shader.resolution_loc, screen_width, screen_height);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glUniform1i(shader.texture_loc, 0);

    // Enable transparency (Important for TTF text)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    // KORRIGERAT: Separate arrays for position and texture coordinates
    float positions[] = {
        x,         y,
        x + width, y,
        x,         y + height,
        x + width, y + height
    };

    float texCoords[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f
    };

    // Send positions (2 floats per corner)
    glEnableVertexAttribArray(shader.position_loc);
    glVertexAttribPointer(shader.position_loc, 2, GL_FLOAT, GL_FALSE, 0, positions);

    // Send texture coordinates (2 floats per vertex from their own array)
    glEnableVertexAttribArray(shader.texCoord_loc);
    glVertexAttribPointer(shader.texCoord_loc, 2, GL_FLOAT, GL_FALSE, 0, texCoords);

    // Draw the quadrilateral
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Clean things up so we don’t collide with the compass in the next frame.
    glDisableVertexAttribArray(shader.position_loc);
    glDisableVertexAttribArray(shader.texCoord_loc);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(0);
}

static TextShader init_text_shader() {
    TextShader ts;
    
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader(vertex_shader);

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader(fragment_shader);

    ts.program = glCreateProgram();
    glAttachShader(ts.program, vertex_shader);
    glAttachShader(ts.program, fragment_shader);
    glLinkProgram(ts.program);

    // Get the locations of all variables in the shader
    ts.position_loc = glGetAttribLocation(ts.program, "position");
    ts.texCoord_loc = glGetAttribLocation(ts.program, "texCoord");
    ts.resolution_loc = glGetUniformLocation(ts.program, "u_resolution");
    ts.texture_loc = glGetUniformLocation(ts.program, "u_texture");

    return ts;
}

// Brightness control
static GLuint overlay_program = 0;
static GLint overlay_alpha_loc = -1;

static const char *overlay_vs =
    "attribute vec2 aPos;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *overlay_fs =
    "precision mediump float;\n"
    "uniform float uAlpha;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(0.0, 0.0, 0.0, uAlpha);\n"
    "}\n";

static void draw_black_overlay(float alpha)
{
    if (alpha <= 0.0f)
        return;

    if (alpha > 1.0f)
        alpha = 1.0f;

    const GLfloat vertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(overlay_program);

    glUniform1f(overlay_alpha_loc, alpha);

    GLint pos_loc = glGetAttribLocation(overlay_program, "aPos");
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(
        pos_loc, 2, GL_FLOAT, GL_FALSE, 0, vertices
    );

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(pos_loc);

    if (!blend_was_enabled)
        glDisable(GL_BLEND);

    if (depth_was_enabled)
        glEnable(GL_DEPTH_TEST);

    glUseProgram(0);
}

// initialization of the overlay shader for brightness control
static int init_overlay(void)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, overlay_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, overlay_fs);

    if (!vs || !fs)
        return 0;

    overlay_program = glCreateProgram();
    glAttachShader(overlay_program, vs);
    glAttachShader(overlay_program, fs);

    glBindAttribLocation(overlay_program, 0, "aPos");
    glLinkProgram(overlay_program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(overlay_program, GL_LINK_STATUS, &ok);

    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(overlay_program, sizeof(log), NULL, log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: Overlay shader link error: %s", log);
        glDeleteProgram(overlay_program);
        overlay_program = 0;
        return 0;
    }

    overlay_alpha_loc =
        glGetUniformLocation(overlay_program, "uAlpha");

    return overlay_alpha_loc >= 0;
}

/* Draw screen-fixed steering arrows using the existing GLES2 shader.
   direction: -1 = steer starboard (>>), +1 = steer port (<<). */

/* Draw screen-fixed steering arrows using the existing GLES2 shader.
   direction: -1 = steer starboard (>>), +1 = steer port (<<). */

static void draw_steer_arrows(int w, int h, int direction, float arrowSpacing)
{
    Vertex v[64];
    int n = 0;

    if (w <= 0 || h <= 0)
        return;

    /* Position and dimensions in screen pixels. */
    const float cy = h * 0.50f;

    const float halfH = 42.0f;      // Arrow height
    const float halfW = 11.0f;      // Arrow width
    const float thickness = 7.0f;   // Arrow thickness

    /*
     * Each chevron consists of four triangles:
     *
     *       outer top
     *          /|
     *         / |
     * tip ---   |  (direction determines left/right)
     *         \ |
     *          \|
     *       outer bottom
     *
     * The upper and lower arms are triangulated separately.
     */
    #define ADD_TRI(ax, ay, bx, by, cx_, cy_) do { \
        const float sx_[3] = { (ax), (bx), (cx_) }; \
        const float sy_[3] = { (ay), (by), (cy_) }; \
        for (int j_ = 0; j_ < 3; ++j_) { \
            float nx_ = 2.0f * sx_[j_] / (float)w - 1.0f; \
            float ny_ = 1.0f - 2.0f * sy_[j_] / (float)h; \
            v[n++] = (Vertex){nx_, ny_, 0.0f, \
                              1.0f, 1.0f, 1.0f, 1.0f}; \
        } \
    } while (0)

    #define ADD_CHEVRON(cx_, dir_) do { \
        float tipX_ = (cx_) + (dir_) * halfW; \
        float backX_ = (cx_) - (dir_) * halfW; \
        float innerX_ = (cx_) - (dir_) * (halfW - thickness); \
        float topY_ = cy - halfH; \
        float innerTopY_ = cy - thickness; \
        float innerBotY_ = cy + thickness; \
        float botY_ = cy + halfH; \
        /* Upper arm: two triangles. */ \
        ADD_TRI(tipX_, cy, backX_, topY_, \
                innerX_, innerTopY_); \
        ADD_TRI(tipX_, cy, innerX_, innerTopY_, \
                (cx_) - (dir_) * thickness, cy); \
        /* Lower arm: two triangles. */ \
        ADD_TRI(tipX_, cy, \
                (cx_) - (dir_) * thickness, cy, \
                innerX_, innerBotY_); \
        ADD_TRI(tipX_, cy, innerX_, innerBotY_, \
                backX_, botY_); \
    } while (0)

    if (direction > 0) {
        /* Steer port: << */
        ADD_CHEVRON(60.0f, -1.0f);                 // Yttre: fast
        ADD_CHEVRON(30.0f + arrowSpacing, -1.0f);  // Inre: rörlig
    } else {
        /* Steer starboard: >> */
        ADD_CHEVRON((float)w - 60.0f, 1.0f);                // Yttre: fast
        ADD_CHEVRON((float)w - 30.0f - arrowSpacing, 1.0f); // Inre: rörlig
    }

    #undef ADD_CHEVRON
    #undef ADD_TRI

    float identity[16];
    mat_identity(identity);

    GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);

    glUseProgram(program);
    glUniformMatrix4fv(uni_mvp, 1, GL_FALSE, identity);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, n * sizeof(Vertex),
                 v, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray((GLuint)attr_pos);
    glEnableVertexAttribArray((GLuint)attr_col);

    glVertexAttribPointer((GLuint)attr_pos, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex), (void *)offsetof(Vertex, x));
    glVertexAttribPointer((GLuint)attr_col, 4, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex), (void *)offsetof(Vertex, r));

    glDrawArrays(GL_TRIANGLES, 0, n);

    if (depthWasEnabled)
        glEnable(GL_DEPTH_TEST);
}

// Helper function for finding the shortest path (-180 to 180 degrees)
static float shortest_angle_difference(float target, float current)
{
    float diff = target - current;
    
    // Normalize the difference to the interval (-360, 360) and shift it to [0, 360)
    diff = fmodf(diff, 360.0f);
    
    // Convert to the range [-180, 180]
    if (diff > 180.0f) {
        diff -= 360.0f;
    } else if (diff < -180.0f) {
        diff += 360.0f;
    }
    
    return diff;
}

// Main function that updates the angle using analog inertia (LERP) and a dead zone
static float update_angle(float current, float target, float interpolation_factor)
{
    /*
     * Interpolation_factor = 0.05; Reacts slowly, filtering out significant jitter.
     * interpolation_factor = 0.20; Responsive pointer: Reacts quickly and snappily,
     * yet still decelerates smoothly as it reaches the target, thanks to LERP mathematics.
     */

    // Calculate the shortest rotation path
    float diff = shortest_angle_difference(target, current);

    if (fabsf(diff) > 0.1f) {
        float newdiff = fabsf(diff) * 100.0f;

        if (newdiff < 50.0f) {
            torsional_load += (int)newdiff;
        } else {
            torsional_load = 0;
        }
    }

    // Dead zone: If the pointer is extremely close to the target, "lock" it in place immediately.
    // This prevents microscopic fluttering and keeps it from becoming sluggish indefinitely.
    if (fabsf(diff) < 0.01f) {
        // Normalize the target angle to [0, 360) before returning it
        target = fmodf(target, 360.0f);
        if (target < 0.0f) target += 360.0f;

        return target; 
    }
    
    // Calculate the next step using smooth deceleration (LERP)
    float new_angle = current + (diff * interpolation_factor);
    
    // Keep the resulting angle strictly between 0 and 360 degrees.
    new_angle = fmodf(new_angle, 360.0f);
    if (new_angle < 0.0f) {
        new_angle += 360.0f;
    }
    
    return new_angle;
}

/* Current direction minus target, normalized to [-180, 180). */
static float angle_error(float current, float target)
{
    return fmodf(current - target + 540.0f, 360.0f) - 180.0f;
}

extern collected_nmea cnmea;
extern warnings warn;

// Main entry
int doCog3d(sdl2_app *sdlApp)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,4);

    SDL_Window *win = sdlApp->window;

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: SDL_GL_CreateContext: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        return 1;
    }

    if (SDL_GL_SetSwapInterval(0) < 0) {
       SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D: VSync could not be enabled: %s", SDL_GetError());
    }

    if (!compass3d_init()) {
        SDL_GL_DeleteContext(ctx);
        return 1;
    }

    if (first_run) {
        target_course=-1.0f;
        first_run=0;
    }

    float view_tilt=62.0f, distance=3.0f;
    int running=1;

    TextShader text_shader = init_text_shader();
    TTF_Font* fontLarge = TTF_OpenFont(sdlApp->fontPath, 32);
    TTF_Font* fontSmall = TTF_OpenFont(sdlApp->fontPath, 12);

    // Non fatal
    int doBrn = init_overlay();

    float rsaAngle = 0;
    float angle_cw = 0;
    float angle_cc = 0;
    float angle_hd = 0;
    float angle_cg = 0;
    float angle_ch = 0;
    float angle_cp = 0;

    const float pScaleOffset=0.3;
    float m2f=cnmea.units==METRIC? 1 : 3.28084;
    target_limit = cnmea.calib->toledd;

    int eType = 0;

    int w,h;
    SDL_GL_GetDrawableSize(win,&w,&h);
    int y_tilt=h/2;

    SDL_Rect exitBarR = {680,438,90,30};        // Exit button (text)
    SDL_Rect hdgBarR =  {(w/2)-90, 6, 180, 38}; // Heading bar
    SDL_Rect cogBarR  = {30,30,180,38};         // Lock course button
    SDL_Rect couBarR =  {w/2-150,440,150,30};   // Unlock course button
    SDL_Rect hduBarR =  {couBarR.x+couBarR.w+5,440,couBarR.w,30};    // Unlock heading button
    SDL_Rect lckBarR =  {30,135,120,26};        // As above

    while(running) {

        char msg_hdg[40] = { "HDG: -" };
        char msg_cog[40] = { "COG: -" };
        char msg_sog[40] = { "SOG: -" };
        char msg_stw[40] = { "STW: -" };
        char msg_dbt[40] = { "DBT: -" };
        char msg_mtw[40] = { "WND: -" };
        char msg_rol[40] = { "ROL: -" };
        char msg_ptc[40] = { "PTC: -" };
        char msg_tod[40] = { "TOD: -" };
        char msg_rsa[40] = { "RUD: -" };
        char msg_lck[40] = { "LCK: -" };
        char msg_lkc[40] = { "COG: -" };
        char msg_lkh[40] = { "HDG: -" };
        do_rudd = do_wind = 0;
        int ap_lock = 0;

        time_t ct = time(NULL);    // Get a timestamp for this turn
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT) {
                running=0;
                eType=e.type;
            } else if (e.type == SDL_MOUSEMOTION ) {
                if (e.motion.x > (w/2)-50 && e.motion.x < (w/2)+50  &&  e.motion.y > 100 && e.motion.y < h-100) {
                    if (y_tilt < e.motion.y)
                        view_tilt-=1.0f;
                    else
                        view_tilt+=1.0f;

                    y_tilt=e.motion.y;
                }
            } else if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN) {
                SDL_Point p;
                int x, y;
                if (e.type == SDL_FINGERDOWN) {
                    x = e.tfinger.x* w;
                    y = e.tfinger.y* h;
                    p = (SDL_Point){ x, y };
                } else {
                    p = (SDL_Point){e.button.x, e.button.y};
                }
                if (SDL_PointInRect(&p, &exitBarR)) {
                    running=0;
                } else if ((SDL_PointInRect(&p, &cogBarR) || SDL_PointInRect(&p, &hdgBarR)) && do_cog) {
                    target_course = angle_cg;
                } else if (SDL_PointInRect(&p, &couBarR) || SDL_PointInRect(&p, &lckBarR) || SDL_PointInRect(&p, &hduBarR)) {
                    target_course = -1.0f;
                }
            }
        }

        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        SDL_LockMutex(sdlApp->conf->nm_mutex);

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
            sprintf(msg_sog, "SOG: %.1f", cnmea.sog);

        // COG - Course over ground
        if (!(ct - cnmea.cog_ts > S_TIMEOUT)) {
            sprintf(msg_cog, "COG: %.0f", cnmea.cog);
            do_cog = 1;
        } else do_cog = 0;

        angle_cg = update_angle(angle_cg, cnmea.cog, 0.2);

        // HDG - The direction the bow points. Magnetic BerryGPS or from Net/USB.
        if (!(ct - cnmea.hdg_i2cts > S_TIMEOUT)) {
            sprintf(msg_hdg, "HDG M: %.0f%s", cnmea.hdg, "\u00B0");
        } else if (!(ct - cnmea.net_ts > S_TIMEOUT)){
            sprintf(msg_hdg, "HDG: %.0f%s", cnmea.hdg, "\u00B0");
        }

        angle_hd = update_angle(angle_hd, cnmea.hdg, 0.2);

        // Run the compass rose with smooth acceleration and handle 0/360 transitions
        // Go Track-mode on COG lock
        angle_cc = update_angle(angle_cc, target_course >0 && do_cog? angle_cg : angle_hd, 0.06);
        compass_angle=360-angle_cc;

        // VHW - Water speed
        if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, cnmea.dbt > 70.0f? "DBT: %.0f" : "DBT: %.1f", cnmea.dbt*m2f);

        // WND - Apparent wind speed
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT)) {
            sprintf(msg_mtw, "WND: %.1f", cnmea.vwrs);
            do_wind = 1;
        }

        // Magnetic or Net Heel
        if (!(ct - cnmea.heel_i2cts > S_TIMEOUT)) {
            sprintf(msg_rol, "ROL: %.0f", fabsf(cnmea.heel));
        } else if (!(ct - cnmea.xdr_ts > S_TIMEOUT)) {
            cnmea.heel_i2cts = cnmea.xdr_ts;
            sprintf(msg_rol, "ROL: %.0f", fabsf(cnmea.heel));
        }

        // RUD - Rudder angle
       if (!(ct - cnmea.rsa_ts > S_TIMEOUT)) {
            sprintf(msg_rsa, "RUD: %.0f ", fabsf(cnmea.rsa));
            rsaAngle = cnmea.rsa;
            rsaAngle += 180;            // The needle towards the south
            rsaAngle = 360 - rsaAngle;  // Mirror the needle motion
            rsaAngle -= compass_angle;  // Normalize the angles towards the compass rose
            do_rudd = 1;
        }
 
        // Calculate pitch and heel movement
        if (!(ct - cnmea.heel_i2cts > S_TIMEOUT) || (!(ct - cnmea.xdr_ts > S_TIMEOUT)) )
        {
            angle_ch = update_angle(angle_ch, cnmea.heel * -1.0f, 0.2f);  // Switch sign to match 2D COG page
            angle_cp = update_angle(angle_cp, cnmea.pitch * -1.0f, 0.3f);
            sprintf(msg_ptc, "PTC: %.0f", fabsf(cnmea.pitch));
        } else {angle_ch=angle_cp=0;}
 
        // Run red wind badge
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT))
        {
            float angle_a = cnmea.vwra;    // 0-180
            if (cnmea.vwrd == 1) angle_a = 360 - angle_a; // Mirror the needle motion
            angle_a -= 360;
            angle_a += angle_hd;            // Normalize the angles towards the compass rose

            angle_cw = update_angle(angle_cw, angle_a, 0.1);
        }

        // Get AP Lock status
        if (!(ct - cnmea.apb_ts > S_TIMEOUT))
        {
            target_course = cnmea.apb;
            ap_lock=do_cog=1;
        }

        SDL_Color status_color_c, status_color_h;
        const SDL_Color green = {75, 220, 135, 255};
        const SDL_Color red   = {245, 75, 75, 255};

        if (target_course >= 0 && do_cog) {

            float hdg_err = angle_error(angle_hd, target_course);
            float cog_err = angle_error(angle_cg, target_course);

            heading_ok = fabsf(hdg_err) <= cnmea.calib->toledd;
            course_ok  = fabsf(cog_err) <= cnmea.calib->toledd;

            status_color_h = heading_ok ? green : red;
            status_color_c = course_ok  ? green : red;

            const char *lock_type = ap_lock==1? "AP_LCK" : "COG_LCK";
            const char* status_c =   course_ok? "ON COURSE" : cog_err < 0? "STEER S->> " : "<<- STEER P ";
            const char* status_h =  heading_ok? "ON COURSE" : "LEEWAY ";

            sprintf(msg_lkc, "%s%+.1f%s", status_c, cog_err, "\u00B0");
            sprintf(msg_lkh, "HDG %s%+.1f%s", status_h, hdg_err, "\u00B0");
            sprintf(msg_lck, "%s: %.0f%s ±%d%s", lock_type, target_course, "\u00B0", cnmea.calib->toledd, "\u00B0");

        } else target_course = -1.0f;

        // Create a NEW texture for this specific frame based on the new string
        const SDL_Color coln = {240, 255, 240, 255};
        const SDL_Color colw = {255, 0, 0, 255};
        SDL_Color dcol = cnmea.dbt <= warn.depthw? colw : coln;

        SDL_UnlockMutex(sdlApp->conf->nm_mutex);

        int wi = 0;
        int hi = 0;

        // Create a NEW texture for this specific frame based on the new strings.
        // This is order-sensitive.
        int nTxt=target_course >= 0 && do_cog? 14 : 11;
        GLuint garr[nTxt];
        int garrIndx = 0;
        SDL_Color lckColn = target_course >0 && do_cog? green : coln;
        garr[garrIndx++] = create_text_texture(fontLarge, target_course >0 && do_cog? msg_cog : msg_hdg, lckColn, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, target_course >0 && do_cog? msg_hdg : msg_cog, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_sog, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_stw, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_dbt, dcol, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_mtw, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_rsa, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_tod, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, "DONE",  coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_rol, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_ptc, coln, &wi, &hi);
        if (target_course >= 0 && do_cog) {
            garr[garrIndx++] = create_text_texture(fontSmall, msg_lkc, status_color_c, &wi, &hi);
            garr[garrIndx++] = create_text_texture(fontSmall, msg_lkh, status_color_h, &wi, &hi);
            garr[garrIndx++] = create_text_texture(fontSmall, msg_lck, green, &wi, &hi);
        }
  
        glViewport(0,0,w,h);
        glClearColor(0.025f,0.028f,0.035f,1.0f);      
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        // Set view - mixed with pitch
        compass3d_set_view(view_tilt,distance);

        // Draw the compass
        compass3d_render(compass_angle, angle_cp, angle_ch-pScaleOffset,0,0,w,h);

        // Draw the wind badge
        if (do_wind) {
            compass3d_set_wind_angle(angle_cw);
        }

        // Compass set course
        if (target_course >= 0 && do_cog) {
            compass3d_set_course_angle(target_course);
        }

        // Draw the rudder arrow
        if (do_rudd) {
            compass3d_set_rudder_angle(rsaAngle);
        }

        int passed = 1;
        for (int i=0; i < nTxt; i++) {
            if (garr[i] == 0) {
                passed = 0;
                break;
            }
        }

        // This is order-sensitive.
        if (passed)
        {
            int wil=180; int hil=38;
            int wis=180; int his=30;
            garrIndx=0;

            draw_text_gles2(text_shader, garr[garrIndx++], hdgBarR.x, hdgBarR.y, (float)hdgBarR.w, (float)hdgBarR.h, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], cogBarR.x, cogBarR.y, (float)cogBarR.w, (float)cogBarR.h, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 64.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 98.0f, (float)wil, (float)hil, (float)w, (float)h);

            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f, 30.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f, 64.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f, 98.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 438.0f, (float)wis, (float)his-10, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], exitBarR.x, exitBarR.y, (float)wis-100, (float)his, (float)w, (float)h);

            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 396.0f, (float)wil-90, (float)hil-10, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f+100.0f, 396.0f,(float)wil-90, (float)hil-10, (float)w, (float)h);

            if (target_course > 0 && do_cog) {
                draw_text_gles2(text_shader, garr[garrIndx++], couBarR.x, couBarR.y, (float)couBarR.w, (float)couBarR.h, (float)w, (float)h);
                draw_text_gles2(text_shader, garr[garrIndx++], hduBarR.x, hduBarR.y, (float)hduBarR.w, (float)hduBarR.h, (float)w, (float)h);
                draw_text_gles2(text_shader, garr[garrIndx++], lckBarR.x, lckBarR.y, lckBarR.w, lckBarR.h, (float)w, (float)h);
            }

            for (int i=0; i < garrIndx; i++) {
                glDeleteTextures(1, &garr[i]);
            }
        }

        // Shorten SDL_Delay() if lots of movement
        int delay = 70 - torsional_load;
        if (delay < 0)  delay = 50;
        if (delay > 70) delay = 70;

        // Steering guidance: show only when course lock is active and COG is outside the allowed tolerance+10.
        // The animation: Let `arrowSpacing` vary over time between, for example, 55.0f and 85.0f.

        Uint32 uticks = SDL_GetTicks();
        static Uint32 ticks;
        static float arrowSpacing = 55.0f;

        if (target_course >= 0.0f && do_cog && !course_ok) {

            if (uticks >= ticks+50) {
                arrowSpacing += 1;
                ticks = uticks;
            }

            delay = 50;

            float cog_err = angle_error(angle_cg, target_course);

            if (fabsf(cog_err)  > 10.0f) {  // Early warning was already given when the marker left the crosshair.
                draw_steer_arrows(w, h, cog_err > 0.0f ? 1 : -1, arrowSpacing);
                if (arrowSpacing > 65.0f) {
                    arrowSpacing = 55.0f;
                }

                // Play an audible spoken warning
                if (cog_err > 0.0f) {warn.turnsb =0; warn.turnp =1;}
                else {warn.turnp =0; warn.turnsb =1;}
            }
        } else {warn.turnp = 0; warn.turnsb = 0;}

        // Set brightness. Controlled from the doCompass() function (COG-page)
        if (sdlApp->conf->br_percent < 1.0f && doBrn)
        {
            float alpha = 1.0f - sdlApp->conf->br_percent;

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            draw_black_overlay(alpha);  // Covers the entire viewport
            glDisable(GL_BLEND);
        }

        SDL_GL_SwapWindow(win);

        SDL_Delay((Uint32)delay);
        torsional_load = 0;
    }

    compass3d_shutdown();
    SDL_GL_DeleteContext(ctx);
    TTF_CloseFont(fontLarge);
    TTF_CloseFont(fontSmall);

    return eType;
}
