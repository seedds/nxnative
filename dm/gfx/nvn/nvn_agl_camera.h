// Observed agl camera uniform layout, recovered from consecutive Ocarina v131072 frames.
// This decodes a supplied uniform block, never a fixed guest address. Callers must select
// the actual camera binding and ensure its bytes match those consumed by the draw.
#ifndef NVN_AGL_CAMERA_H
#define NVN_AGL_CAMERA_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>
typedef struct {
    float clip_to_world[16], previous_world_to_clip[16]; // explicit rows, Metal [0,1] depth
    uint32_t width, height;
    float near_plane, far_plane, fov;
    float world_to_clip[16];
    uint32_t has_current_transform; // decoded current matrix, for previous-input continuity
    float world_to_view[16],view_to_clip[16]; // explicit rows, actual Metal-depth projection
    uint32_t has_render_matrices;
} nvn_camera_input;
static inline void nvn_cam_mul(const float a[16], const float b[16], float out[16]) {
    for (int i=0;i<4;i++) for(int j=0;j<4;j++) { double v=0; for(int k=0;k<4;k++) v+=(double)a[4*i+k]*b[4*k+j]; out[4*i+j]=(float)v; }
}
static inline int nvn_cam_inverse(const float in[16], float out[16]) {
    double a[4][8];
    for(int i=0;i<4;i++) for(int j=0;j<8;j++) a[i][j]=j<4?in[4*i+j]:(j-4==i);
    for(int k=0;k<4;k++) {
        int pivot=k; for(int i=k+1;i<4;i++) if(fabs(a[i][k])>fabs(a[pivot][k])) pivot=i;
        if(fabs(a[pivot][k])<1e-12) return 0;
        if(pivot!=k) for(int j=0;j<8;j++) {double t=a[k][j];a[k][j]=a[pivot][j];a[pivot][j]=t;}
        double d=a[k][k]; for(int j=0;j<8;j++) a[k][j]/=d;
        for(int i=0;i<4;i++) if(i!=k) {double d2=a[i][k];for(int j=0;j<8;j++) a[i][j]-=d2*a[k][j];}
    }
    for(int i=0;i<4;i++) for(int j=0;j<4;j++) {out[4*i+j]=(float)a[i][j+4];if(!isfinite(out[4*i+j]))return 0;}
    return 1;
}
static inline int nvn_cam_view_valid(const float v[12]) {
    for(int i=0;i<3;i++) for(int j=0;j<3;j++) {
        double dot=0;for(int k=0;k<3;k++) dot+=(double)v[i*4+k]*v[j*4+k];
        if(fabs(dot-(i==j))>.002) return 0;
    }
    return 1;
}
static inline void nvn_cam_clip(const float projection[16], const float view[12], float clip[16]) {
    float v[16]={0}, p[16]; memcpy(v,view,12*sizeof(float));v[15]=1;memcpy(p,projection,sizeof p);
    // The observed projection is OpenGL [-1,1]; NVN maps that range into Metal's [0,1].
    for(int j=0;j<4;j++) p[8+j]=.5f*(projection[8+j]+projection[12+j]);
    nvn_cam_mul(p,v,clip);
}
// A skipped presentation is not necessarily a missing camera input. The game's
// previous matrix must describe the actual color input retained by the interpolator.
static inline int nvn_camera_previous_matches(const nvn_camera_input *now,const nvn_camera_input *last) {
    if(!now->has_current_transform || !last->has_current_transform)return 0;
    for(int i=0;i<16;i++) {
        float a=now->previous_world_to_clip[i],b=last->world_to_clip[i];
        if(!isfinite(a) || !isfinite(b) || fabsf(a-b)>8*FLT_EPSILON*(1+fabsf(a)+fabsf(b)))return 0;
    }
    return 1;
}
static inline int nvn_agl_camera_decode(const void *bytes, size_t size, nvn_camera_input *out) {
    if(!bytes || !out || size<0x200) return 0;
    float f[128];memcpy(f,bytes,sizeof f);
    for(int i=0;i<128;i++) if(!isfinite(f[i])) return 0;
    if(!nvn_cam_view_valid(f) || !nvn_cam_view_valid(f+0x150/4)) return 0;
    const float *p=f+0x70/4,*pp=f+0x1c0/4;
    if(p[0]<=0 || p[5]<=0 || fabsf(p[14]+1)>.0001 || fabsf(p[15])>.0001 ||
       pp[0]<=0 || pp[5]<=0 || fabsf(pp[14]+1)>.0001 || fabsf(pp[15])>.0001) return 0;
    float near=f[0xe0/4],far=f[0xe4/4],w=f[0x120/4],h=f[0x124/4],fov=f[0x118/4];
    if(near<=0 || far<=near || w<16 || h<16 || w>16384 || h>16384 ||
       w!=floorf(w) || h!=floorf(h) || fov<=0 || fov>=3.141593f) return 0;
    if(fabsf(p[5]/p[0]-w/h)>.01f || fabsf(p[5]-1/tanf(fov/2))>.01f) return 0;
    float current[16];nvn_camera_input result={0};
    nvn_cam_clip(p,f,current);
    if(!nvn_cam_inverse(current,result.clip_to_world)) return 0;
    memcpy(result.world_to_clip,current,sizeof current);result.has_current_transform=1;
    memcpy(result.world_to_view,f,12*sizeof(float));result.world_to_view[15]=1;
    memcpy(result.view_to_clip,p,16*sizeof(float));
    for(int j=0;j<4;j++)result.view_to_clip[8+j]=.5f*(p[8+j]+p[12+j]);
    result.has_render_matrices=1;
    nvn_cam_clip(pp,f+0x150/4,result.previous_world_to_clip);
    result.width=(uint32_t)w;result.height=(uint32_t)h;result.near_plane=near;result.far_plane=far;result.fov=fov;
    *out=result; return 1;
}
#endif
