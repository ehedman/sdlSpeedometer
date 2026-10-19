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
static int win_w = 960, win_h = 540;
static float render_heading_deg = 0.0f;
/* Wind angle relative to the boat/compass rose: 0=bow, 90=starboard. */
static float wind_angle_deg = 240.0f;
/* Rudder indicator angle, independent of heading and wind. 0=bow, 90=starboard. */
static float rudder_angle_deg = 0.0f;
static float render_heel_deg = 0.0f;
static float camera_tilt_deg = 62.0f;
static float camera_distance = 4.2f;
static float compass_scale = 1.0f;
static int renderer_initialized = 0;

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
static void compass3d_set_wind_angle(float degrees, int doIt) {

    if (!doIt) degrees=0.0;

    wind_angle_deg = fmodf(degrees, 360.0f);
    if (wind_angle_deg < 0.0f) wind_angle_deg += 360.0f;
}

/* Set rudder indicator angle in degrees; 0=bow, 90=starboard. */
static void compass3d_set_rudder_angle(float degrees) {
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
         SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D TTF_Init: %s" ,TTF_GetError());
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
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D Could not open a system TTF font: %s" ,TTF_GetError());
        return 0;
    }

    SDL_Surface *atlas=SDL_CreateRGBSurfaceWithFormat(0,(int)strlen(atlas_chars)*atlas_cell_w,atlas_cell_h,32,SDL_PIXELFORMAT_RGBA32);

    if(!atlas) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D SDL_CreateRGBSurface: %s" ,TTF_GetError());
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
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D Text shader link error: %s" ,log);
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
    {
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

    /* Wind-angle marker on the outer rim. Its angle is independent of heading;
       the model transform makes it follow the compass's 3D geometry. The orange
       badge has a pointed tip directed radially inward, toward the dial centre. */
    {
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
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D Shader error:  %s" ,log);
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
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D Link error: %s" ,log);
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
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D TTF_Init: %s" ,TTF_GetError());
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

static void compass3d_render(double heading_degrees, double heel_degrees, double pitch_degrees, int x, int y, int width, int height)
{
    if (!renderer_initialized || width <= 0 || height <= 0)
        return;

    render_heading_deg = (float)heading_degrees;
    render_heel_deg    = (float)heel_degrees;
    double render_pitch_deg   = (float)pitch_degrees;

    win_w = width;
    win_h = height;

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
    /* TTF_Quit is intentionally not called: the host application may use SDL_ttf too. */
}

// Simple shaders for rendering 2D text
static const char* vertex_shader_source =
    "attribute vec4 position;\n"
    "attribute vec2 texCoord;\n"
    "varying vec2 v_texCoord;\n"
    "uniform vec2 u_resolution;\n" // Window width and height in pixels
    "void main() {\n"
    // Konvertera från pixlar (0 till vidd/höjd) till OpenGL-koordinater (-1.0 till 1.0)
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
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D Could not render text:  %s" ,TTF_GetError());
        return 0;
    }

    // Use ABGR8888 to match OpenGL ES's internal memory layout for GL_RGBA
    SDL_Surface* surface = SDL_ConvertSurfaceFormat(initial_surface, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(initial_surface);
    
    if (!surface) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D Could not convert pixel format:  %s" ,TTF_GetError());
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

// Helper function for finding the shortest path (-180 to 180 degrees)
static double shortest_angle_difference(double target, double current)
{
    double diff = target - current;
    
    // Normalize the difference to the interval (-360, 360) and shift it to [0, 360)
    diff = fmod(diff, 360.0);
    
    // Convert to the range [-180, 180]
    if (diff > 180.0) {
        diff -= 360.0;
    } else if (diff < -180.0) {
        diff += 360.0;
    }
    
    return diff;
}

static int angleLoad;

// Main function that updates the angle using analog inertia (LERP) and a dead zone
static double update_angle(double current, double target, double interpolation_factor)
{
    /*
     * Interpolation_factor = 0.05; Reacts slowly, filtering out significant jitter.
     * interpolation_factor = 0.20; Responsive pointer: Reacts quickly and snappily,
     * yet still decelerates smoothly as it reaches the target, thanks to LERP mathematics.
     */

    // Calculate the shortest rotation path
    double diff = shortest_angle_difference(target, current);

    if (diff>  0.1) {
        double newdiff = (diff * - 1.0) * 100;
        if (fabs(newdiff) < 50) {
        angleLoad += (int)fabs(newdiff);
        } else angleLoad = 0;
    }
    // Dead zone: If the pointer is extremely close to the target, "lock" it in place immediately.
    // This prevents microscopic fluttering and keeps it from becoming sluggish indefinitely.
    if (fabs(diff) < 0.01) {
        // Normalize the target angle to [0, 360) before returning it
        target = fmod(target, 360.0);
        if (target < 0.0) target += 360.0;

        return target; 
    }
    
    // Calculate the next step using smooth deceleration (LERP)
    double new_angle = current + (diff * interpolation_factor);
    
    // Keep the resulting angle strictly between 0 and 360 degrees.
    new_angle = fmod(new_angle, 360.0);
    if (new_angle < 0.0) {
        new_angle += 360.0;
    }
    
    return new_angle;
}

extern collected_nmea cnmea;
extern warnings warn;

// Main entry
int doCog3d(sdl2_app *sdlApp, configuration *configParams)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,4);

    SDL_Window *win = sdlApp->window;

    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    if(!ctx){
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "COG3D SDL_GL_CreateContext: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        return 1;
    }
    SDL_GL_SetSwapInterval(-1);
    if(!compass3d_init()){SDL_GL_DeleteContext(ctx);return 1;}

    float view_tilt=62.0f, distance=3.0f;
    int running=1;

    TextShader text_shader = init_text_shader();
    TTF_Font* fontLarge = TTF_OpenFont(sdlApp->fontPath, 32);

    double heading = 0.0;
    double heel= 0;
    double rsaAngle = 0;
    double angle_cw = 0;
    double angle_cc = 0;
    double angle_ch = 0;
    double angle_cp = 0;

    const double pScaleOffset=0.3;
    double m2f=cnmea.units==METRIC? 1 : 3.28084;

    int eType = 0;

    int w,h;
    SDL_GL_GetDrawableSize(win,&w,&h);
    int y_tilt=h/2;

    SDL_Rect exitBarR = {680,438,90,30};

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
        int doWind = 0;

        time_t ct = time(NULL);    // Get a timestamp for this turn
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if(e.type==SDL_QUIT) {
                running=0;
                eType=e.type;
            } else if(e.type==SDL_KEYDOWN) switch(e.key.keysym.sym) {
                case SDLK_q: heel-=2.0; break;
                case SDLK_e: heel+=2.0; break;
                default: break;
            } else if (e.type == SDL_MOUSEMOTION ) {
                if (e.motion.x > (w/2)-50 && e.motion.x < (w/2)+50 ) {
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
                }
            }
        }

        strftime(msg_tod, sizeof(msg_tod),TIMEDATFMT, localtime(&ct));

        SDL_LockMutex(sdlApp->conf->nm_mutex);

        // RMC - Recommended minimum specific GPS/Transit data
        if (!(ct - cnmea.rmc_ts > S_TIMEOUT))
            sprintf(msg_sog, "SOG: %.1f", cnmea.sog);

        // HDG - The direction the bow points. Magnetic BerryGPS or from Net/USB.
        if (!(ct - cnmea.hdg_i2cts > S_TIMEOUT)) {
            sprintf(msg_hdg, "HDG M: %.0f%s", cnmea.hdg, "\u00B0");
        } else if (!(ct - cnmea.net_ts > S_TIMEOUT)){
            sprintf(msg_hdg, "HDG: %.0f%s", cnmea.hdg, "\u00B0");
        }

        // Run the compass rose with smooth acceleration and handle 0/360 transitions
        angle_cc = update_angle(angle_cc, cnmea.hdg, 0.2);    
        heading=360-angle_cc;


        // COG - Course over ground
        if (!(ct - cnmea.cog_ts > S_TIMEOUT))
            sprintf(msg_cog, "COG: %.0f", cnmea.cog);

        // VHW - Water speed
        if (!(ct - cnmea.stw_ts > S_TIMEOUT))
            sprintf(msg_stw, "STW: %.1f", cnmea.stw);

        // DBT - Depth Below Transponder
        if (!(ct - cnmea.dbt_ts > S_TIMEOUT))
            sprintf(msg_dbt, cnmea.dbt > 70.0? "DBT: %.0f" : "DBT: %.1f", cnmea.dbt*m2f);

        // WND - Apparent wind speed
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT))
            sprintf(msg_mtw, "WND: %.1f", cnmea.vwrs);

        // Magnetic or Net Heel
        if (!(ct - cnmea.heel_i2cts > S_TIMEOUT)) {
            sprintf(msg_rol, "ROL: %.0f", fabs(cnmea.heel));
        } else if (!(ct - cnmea.xdr_ts > S_TIMEOUT)) {
            cnmea.heel_i2cts = cnmea.xdr_ts;
            sprintf(msg_rol, "ROL: %.0f", fabs(cnmea.heel));
        }

        // RUD - Rudder angle
       if (!(ct - cnmea.rsa_ts > S_TIMEOUT)) {
            sprintf(msg_rsa, "RUD: %.0f ", fabs(cnmea.rsa));
            rsaAngle = cnmea.rsa;
            rsaAngle += 180;            // The needle towards the south
            rsaAngle = 360 - rsaAngle;  // Mirror the needle motion
            rsaAngle -= heading;        // Normalize the angles towards the compass rose
        } else rsaAngle=180-heading;    // Go south
 
        // Calculate pitch and heel movement
        if (!(ct - cnmea.heel_i2cts > S_TIMEOUT) || (!(ct - cnmea.xdr_ts > S_TIMEOUT)) )
        {
            angle_ch = update_angle(angle_ch, cnmea.heel, 0.2);
            angle_cp = update_angle(angle_cp, cnmea.pitch, 0.3);
            sprintf(msg_ptc, "PTC: %.0f", fabs(cnmea.pitch));
        } else {angle_ch=angle_cp=0;}
 
        // Run red wind icon
        if (!(ct - cnmea.vwr_ts > S_TIMEOUT))
        {
            doWind = 1;

            double angle_a = cnmea.vwra;   // 0-180
            if (cnmea.vwrd == 1) angle_a = 360 - angle_a; // Mirror the needle motion
            angle_a -= 360;
            angle_a += cnmea.hdg;    // Normalize the angles towards the compass rose

            angle_cw = update_angle(angle_cw, angle_a, 0.1);
        }

        SDL_UnlockMutex(sdlApp->conf->nm_mutex);

        // Create a NEW texture for this specific frame based on the new string
        SDL_Color coln = {240, 255, 240, 255};
        SDL_Color colw = {255, 0, 0, 255};

        SDL_Color dcol = cnmea.dbt <= warn.depthw? colw : coln;

        int wi = 0;
        int hi = 0;

        // Create a NEW texture for this specific frame based on the new strings.
        // This is order-sensitive.
        int nTxt=9;
        GLuint garr[nTxt];
        int garrIndx = 0;
        garr[garrIndx++] = create_text_texture(fontLarge, msg_hdg, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_cog, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_sog, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_stw, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_dbt, dcol, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_mtw, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_rsa, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_tod, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, "DONE",  coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_rol, coln, &wi, &hi);
        garr[garrIndx++] = create_text_texture(fontLarge, msg_ptc, coln, &wi, &hi);
  

        glViewport(0,0,w,h);
        glClearColor(0.025f,0.028f,0.035f,1.0f);      
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        // Set view - mixed with pitch
        compass3d_set_view(view_tilt,distance);

        // Draw the compass
        compass3d_render(heading, angle_cp, angle_ch-pScaleOffset,0,0,w,h);

        // Draw the wind icon
        compass3d_set_wind_angle(angle_cw, doWind);

        // Draw the rudder angle
        compass3d_set_rudder_angle(rsaAngle);

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

            draw_text_gles2(text_shader, garr[garrIndx++], (w/2)-90, 20.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 30.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 64.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 98.0f, (float)wil, (float)hil, (float)w, (float)h);

            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f, 30.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f, 64.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f, 98.0f, (float)wil, (float)hil, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 438.0f, (float)wis, (float)his-10, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], exitBarR.x, exitBarR.y, (float)wis-100, (float)his, (float)w, (float)h);

            draw_text_gles2(text_shader, garr[garrIndx++], 30.0f, 396.0f, (float)wil-90, (float)hil-10, (float)w, (float)h);
            draw_text_gles2(text_shader, garr[garrIndx++], 580.0f+100, 396.0f,(float)wil-90, (float)hil-10, (float)w, (float)h);

            for (int i=0; i < garrIndx; i++) {
                glDeleteTextures(1, &garr[i]);
            }
        }

        SDL_GL_SwapWindow(win);

        //printf("angleLoad=%d\n", angleLoad);
        // Shorten delay if lots of movement
        SDL_Delay(90-angleLoad);
        angleLoad = 0;
    }

    compass3d_shutdown();
    SDL_GL_DeleteContext(ctx);
    TTF_CloseFont(fontLarge);

    return eType;
}
