#pragma once
// 11 visualization modes for 96×16 HUB75 LED matrix.
//
// Speed optimizations:
//   - wf_buf / ew_buf / fire_g allocated in PSRAM (heap_caps_malloc) to free DRAM
//   - 256-entry DRAM sin LUT replaces sinf() per pixel in plasma (~40× faster)
//   - XOR-shift PRNG replaces Arduino random() in fire and starfield (~4× faster)
//   - memmove single-call waterfall scroll instead of 15 memcpy calls
//   - drawRGBBitmap batch render for waterfall and echo wave
//   - Fire inner loop removes modulo for non-edge pixels
//   - All float arithmetic (ESP32-S3 FPU is hardware single-precision only)

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "music_player.h"
#include <math.h>
#include <esp_heap_caps.h>
#include <Fonts/TomThumb.h>

// SD_W is set at runtime from NVS panel count; defined in the main .ino.cpp
extern int SD_W;
#define SD_H  PANEL_RES_Y                   // 16

// ── public ────────────────────────────────────────────────────────────────────
#define VIZ_COUNT    11   // regular audio-driven modes (0-10)
#define VIZ_BREATHE  11   // 4-7-8 breathing light
#define VIZ_COLORMIX 12   // RGB color-mixer fidget
const char* const VIZ_NAMES[VIZ_COUNT] = {
    "Spectrum","Mirror","Waterfall","Color Organ",
    "Oscilloscope","Echo Wave","Fire",
    "VU Meter","Beat Flash","Plasma","Starfield"
};
int g_viz_mode = 0;
extern uint32_t g_breathe_start;   // set by /breathe endpoint
extern uint8_t  g_mix_r, g_mix_g, g_mix_b;  // set by /mix endpoint

// ── XOR-shift PRNG ────────────────────────────────────────────────────────────
// ~4× faster than Arduino random(); adequate entropy for visual noise
static uint32_t _rng = 0xDEADBEEF;
static inline uint8_t fast_rand8() {
    _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
    return (uint8_t)_rng;
}

// ── sin lookup table (DRAM, 1 KB) ─────────────────────────────────────────────
// DRAM_ATTR keeps it in data RAM for consistent latency (not flash cache).
// 256 entries: 1.4° resolution — imperceptible in animation.
static DRAM_ATTR float _sin_tab[256];
static bool            _sin_ready = false;
static void _build_sin_tab() {
    if (_sin_ready) return;
    for (int i = 0; i < 256; i++)
        _sin_tab[i] = sinf(i * (2.0f * (float)M_PI / 256.0f));
    _sin_ready = true;
}
// fast_sin: radians → [-1, 1]; ~40× faster than sinf() on Xtensa LX7
static inline float fast_sin(float x) {
    return _sin_tab[(uint8_t)(int)(x * 40.7437f)];  // 256 / (2π)
}

// ── colour helpers ────────────────────────────────────────────────────────────
static inline uint16_t hue565(float h) {
    h = fmodf(h,1.f); if(h<0) h+=1.f;
    float s=h*6.f; int i=(int)s; float f=s-i;
    uint8_t r,g,b;
    switch(i){
        case 0: r=255;g=(uint8_t)(f*255);    b=0;             break;
        case 1: r=(uint8_t)((1-f)*255);g=255;b=0;             break;
        case 2: r=0;g=255;            b=(uint8_t)(f*255);     break;
        case 3: r=0;g=(uint8_t)((1-f)*255);  b=255;           break;
        case 4: r=(uint8_t)(f*255);   g=0;   b=255;           break;
        default:r=255;g=0;            b=(uint8_t)((1-f)*255); break;
    }
    return ((uint16_t)(r>>3)<<11)|((uint16_t)(g>>2)<<5)|(b>>3);
}
static inline uint16_t dim565(uint16_t c, float br) {
    if(br<=0) return 0; if(br>=1) return c;
    return ((uint16_t)(uint8_t)(((c>>11)&0x1F)*br)<<11)
          |((uint16_t)(uint8_t)(((c>>5 )&0x3F)*br)<<5)
          |           (uint8_t)(( c     &0x1F)*br);
}
static inline uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){
    return((uint16_t)(r>>3)<<11)|((uint16_t)(g>>2)<<5)|(b>>3);
}
static inline uint16_t fire_pal(uint8_t v){
    if(v<128) return rgb565(v*2,0,0);
    if(v<192) return rgb565(255,(v-128)*4,0);
    return rgb565(255,255,(v-192)*4);
}

// ── PSRAM frame buffers ───────────────────────────────────────────────────────
// Allocated in PSRAM to free DRAM for WiFi stack / task stacks.
// Falls back to DRAM silently if PSRAM is unavailable or exhausted.
static uint16_t *wf_buf = nullptr;   // waterfall  SD_H×SD_W×2 bytes
static uint16_t *ew_buf = nullptr;   // echo wave  SD_H×SD_W×2 bytes
static uint8_t  *fire_g = nullptr;   // fire grid  (SD_H+2)×SD_W bytes

static void initVisualizer() {
    static bool done = false;
    if (done) return;
    done = true;
    _build_sin_tab();

    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    wf_buf = (uint16_t*)heap_caps_malloc(SD_H     * SD_W * sizeof(uint16_t), caps);
    ew_buf = (uint16_t*)heap_caps_malloc(SD_H     * SD_W * sizeof(uint16_t), caps);
    fire_g = (uint8_t *) heap_caps_malloc((SD_H+2) * SD_W * sizeof(uint8_t),  caps);

    // DRAM fallback
    if (!wf_buf) wf_buf = (uint16_t*)calloc(SD_H     * SD_W, sizeof(uint16_t));
    if (!ew_buf) ew_buf = (uint16_t*)calloc(SD_H     * SD_W, sizeof(uint16_t));
    if (!fire_g) fire_g = (uint8_t *) calloc((SD_H+2) * SD_W, sizeof(uint8_t));

    if (wf_buf) memset(wf_buf, 0, SD_H     * SD_W * sizeof(uint16_t));
    if (ew_buf) memset(ew_buf, 0, SD_H     * SD_W * sizeof(uint16_t));
    if (fire_g) memset(fire_g, 0, (SD_H+2) * SD_W * sizeof(uint8_t));

    Serial.printf("[viz] PSRAM free: %u B  DRAM free: %u B\n",
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// ── per-mode state ────────────────────────────────────────────────────────────
static float  sp_hue_off = 0;
static int    sp_peak[SPECTRUM_BINS]      = {};
static float  sp_hold[SPECTRUM_BINS]      = {};

static float    bf_fade = 0;
static uint16_t bf_col  = 0xFFFF;
static float    bf_avg  = 0.1f;

struct Star { float x,y,z,hue; };
static Star  stars[150];
static bool  stars_ok = false;

// ── timing ────────────────────────────────────────────────────────────────────
static unsigned long _vlast = 0;
static inline float _dt(){
    unsigned long n=millis(); float d=(n-_vlast)*0.001f;
    _vlast=n; return d>0.15f?0.15f:d;
}

// ── 0: Spectrum Bars ─────────────────────────────────────────────────────────
static void viz_spectrum(MatrixPanel_I2S_DMA* d){
    float dt=_dt();
    sp_hue_off=fmodf(sp_hue_off+dt*0.08f,1.f);
    for(int b=0;b<SPECTRUM_BINS;b++){
        int bH=constrain((int)(g_spectrum[b]*SD_H+.5f),0,SD_H);
        if(bH>=sp_peak[b]){sp_peak[b]=bH;sp_hold[b]=0.5f;}
        else{sp_hold[b]-=dt;if(sp_hold[b]<=0&&sp_peak[b]>0)sp_peak[b]--;}
        float hue=sp_hue_off+(float)b/SPECTRUM_BINS;
        int x0=b*2, yS=SD_H-bH;
        if(bH<SD_H){d->drawFastVLine(x0,0,SD_H-bH,0);d->drawFastVLine(x0+1,0,SD_H-bH,0);}
        for(int r=0;r<bH;r++){
            uint16_t px=dim565(hue565(hue+r*0.005f),1.f-(float)r/SD_H*0.6f);
            d->drawPixel(x0,yS+r,px); d->drawPixel(x0+1,yS+r,px);
        }
        if(bH>0){d->drawPixel(x0,yS,0xFFFF);d->drawPixel(x0+1,yS,0xFFFF);}
        int py=SD_H-sp_peak[b];
        if(sp_peak[b]>bH&&py>=0&&py<SD_H){
            d->drawPixel(x0,py,0xFFE0);d->drawPixel(x0+1,py,0xFFE0);}
    }
}

// ── 1: Mirror ────────────────────────────────────────────────────────────────
static void viz_mirror(MatrixPanel_I2S_DMA* d){
    float dt=_dt();
    sp_hue_off=fmodf(sp_hue_off+dt*0.08f,1.f);
    int cy=SD_H/2;
    for(int b=0;b<SPECTRUM_BINS;b++){
        int half=constrain((int)(g_spectrum[b]*cy+.5f),0,cy);
        float hue=sp_hue_off+(float)b/SPECTRUM_BINS;
        int x0=b*2, blank=cy-half;
        if(blank>0){
            d->drawFastVLine(x0,0,blank,0);       d->drawFastVLine(x0+1,0,blank,0);
            d->drawFastVLine(x0,cy+half,blank,0); d->drawFastVLine(x0+1,cy+half,blank,0);
        }
        for(int i=0;i<half;i++){
            float br=1.f-(float)i/cy*0.55f;
            uint16_t px=dim565(hue565(hue+i*0.004f),br);
            d->drawPixel(x0,cy-1-i,px); d->drawPixel(x0+1,cy-1-i,px);
            d->drawPixel(x0,cy+i,  px); d->drawPixel(x0+1,cy+i,  px);
        }
        if(half>0){
            d->drawPixel(x0,cy-half,  0xFFFF);d->drawPixel(x0+1,cy-half,  0xFFFF);
            d->drawPixel(x0,cy+half-1,0xFFFF);d->drawPixel(x0+1,cy+half-1,0xFFFF);
        }
    }
}

// ── 2: Waterfall ─────────────────────────────────────────────────────────────
static void viz_waterfall(MatrixPanel_I2S_DMA* d){
    _dt();
    if(!wf_buf) return;
    // Single memmove shifts all rows down in one call; no loop needed
    memmove(wf_buf + SD_W, wf_buf, (SD_H-1) * SD_W * sizeof(uint16_t));
    for(int b=0;b<SPECTRUM_BINS;b++){
        float m=g_spectrum[b];
        uint16_t c=(m>0.02f)?dim565(hue565((float)b/SPECTRUM_BINS),m*0.9f+0.1f):0;
        wf_buf[b*2]=c; wf_buf[b*2+1]=c;
    }
    // Batch render: one call vs SD_H*SD_W individual drawPixel calls
    d->drawRGBBitmap(0,0,wf_buf,SD_W,SD_H);
}

// ── 3: Color Organ ───────────────────────────────────────────────────────────
static void viz_colorgan(MatrixPanel_I2S_DMA* d){
    _dt();
    float vals[3]={0,0,0};
    for(int b= 0;b<16;b++) vals[0]+=g_spectrum[b]; vals[0]/=16;
    for(int b=16;b<32;b++) vals[1]+=g_spectrum[b]; vals[1]/=16;
    for(int b=32;b<48;b++) vals[2]+=g_spectrum[b]; vals[2]/=16;
    const int bw = SD_W/3;
    const int xs[3] ={0, bw, bw*2};
    const float hlo[3]={0.00f,0.33f,0.55f};
    const float hhi[3]={0.12f,0.45f,0.65f};
    for(int band=0;band<3;band++){
        int bH=constrain((int)(vals[band]*SD_H+.5f),0,SD_H);
        if(bH<SD_H) d->fillRect(xs[band],0,bw,SD_H-bH,0);
        for(int row=0;row<bH;row++){
            float t=1.f-(float)row/max(bH,1);
            float hue=hlo[band]+(hhi[band]-hlo[band])*(1.f-t);
            d->fillRect(xs[band],SD_H-bH+row,bw,1,dim565(hue565(hue),0.4f+0.6f*t));
        }
    }
}

// ── 4: Oscilloscope ──────────────────────────────────────────────────────────
static void viz_osc(MatrixPanel_I2S_DMA* d){
    float dt=_dt();
    static float osc_hue=0; osc_hue=fmodf(osc_hue+dt*0.12f,1.f);
    d->clearScreen();
    if(!g_osc_ready) return;
    float pk=0.01f;
    for(int i=0;i<OSC_BUF_SIZE;i++){ float a=fabsf(g_osc_buf[i]); if(a>pk) pk=a; }
    int prev_y=SD_H/2;
    for(int x=0;x<SD_W;x++){
        float v=g_osc_buf[x*OSC_BUF_SIZE/SD_W]/pk;
        int y=constrain(SD_H/2-(int)(v*(SD_H/2-1)),0,SD_H-1);
        uint16_t c=hue565(osc_hue+(float)x/SD_W);
        int y0=min(prev_y,y),y1=max(prev_y,y);
        for(int py=y0;py<=y1;py++) d->drawPixel(x,py,c);
        prev_y=y;
    }
}

// ── 5: Echo Wave ─────────────────────────────────────────────────────────────
static void viz_echowave(MatrixPanel_I2S_DMA* d){
    _dt();
    if(!ew_buf) return;
    for(int y=0;y<SD_H;y++)
        for(int x=0;x<SD_W;x++) ew_buf[y*SD_W+x]=dim565(ew_buf[y*SD_W+x],0.78f);
    if(g_osc_ready){
        float pk=0.01f;
        for(int i=0;i<OSC_BUF_SIZE;i++){ float a=fabsf(g_osc_buf[i]); if(a>pk) pk=a; }
        int prev_y=SD_H/2;
        for(int x=0;x<SD_W;x++){
            float v=g_osc_buf[x*OSC_BUF_SIZE/SD_W]/pk;
            int y=constrain(SD_H/2-(int)(v*(SD_H/2-1)),0,SD_H-1);
            int y0=min(prev_y,y),y1=max(prev_y,y);
            for(int py=y0;py<=y1;py++) ew_buf[py*SD_W+x]=0xFFFF;
            prev_y=y;
        }
    }
    d->drawRGBBitmap(0,0,ew_buf,SD_W,SD_H);
}

// ── 6: Fire ──────────────────────────────────────────────────────────────────
static void viz_fire(MatrixPanel_I2S_DMA* d){
    _dt();
    if(!fire_g) return;
    uint8_t heat=(uint8_t)constrain((int)(g_amplitude*230+25),0,255);
    for(int x=0;x<SD_W;x++){
        fire_g[SD_H    *SD_W+x]=(uint8_t)max(0,heat-(int)(fast_rand8()%35));
        fire_g[(SD_H+1)*SD_W+x]=(uint8_t)max(0,heat-(int)(fast_rand8()%15));
    }
    for(int y=SD_H-1;y>=0;y--){
        uint8_t* row = fire_g + y*SD_W;
        uint8_t* rp1 = fire_g + (y+1)*SD_W;
        uint8_t* rp2 = fire_g + (y+2)*SD_W;
        // left edge — wraps to right side
        { int s=((int)rp1[SD_W-1]+rp1[0]+rp1[1]+rp2[0])>>2;
          s-=(fast_rand8()&3); row[0]=(uint8_t)(s<0?0:s); }
        // inner pixels — no modulo needed, hot loop
        for(int x=1;x<SD_W-1;x++){
            int s=((int)rp1[x-1]+rp1[x]+rp1[x+1]+rp2[x])>>2;
            s-=(fast_rand8()&3); row[x]=(uint8_t)(s<0?0:s);
        }
        // right edge — wraps to left side
        { int s=((int)rp1[SD_W-2]+rp1[SD_W-1]+rp1[0]+rp2[SD_W-1])>>2;
          s-=(fast_rand8()&3); row[SD_W-1]=(uint8_t)(s<0?0:s); }
    }
    for(int y=0;y<SD_H;y++)
        for(int x=0;x<SD_W;x++) d->drawPixel(x,y,fire_pal(fire_g[y*SD_W+x]));
}

// ── 7: VU Meter ──────────────────────────────────────────────────────────────
static void viz_vumeter(MatrixPanel_I2S_DMA* d){
    float dt=_dt();
    static float pk[3]={},phold[3]={};
    float vals[3]={0,0,0};
    for(int b= 0;b<16;b++) vals[0]+=g_spectrum[b]; vals[0]/=16;
    for(int b=16;b<32;b++) vals[1]+=g_spectrum[b]; vals[1]/=16;
    for(int b=32;b<48;b++) vals[2]+=g_spectrum[b]; vals[2]/=16;
    const int   y0s[3]={0,6,12};
    const int   hs [3]={5,5, 4};
    const float lh [3]={0.0f,0.35f,0.55f};
    d->clearScreen();
    for(int s=0;s<3;s++){
        float v=vals[s];
        int fill=(int)(v*SD_W);
        if(fill>=(int)(pk[s]*SD_W)){pk[s]=v;phold[s]=0.7f;}
        else{phold[s]-=dt;if(phold[s]<=0)pk[s]=fmaxf(0,pk[s]-0.015f);}
        for(int x=0;x<fill&&x<SD_W;x++){
            float t=(float)x/SD_W;
            d->drawFastVLine(x,y0s[s],hs[s],dim565(hue565(lh[s]+t*0.15f),0.5f+t*0.5f));
        }
        int px=(int)(pk[s]*SD_W)-1;
        if(px>0&&px<SD_W) d->drawFastVLine(px,y0s[s],hs[s],0xFFFF);
    }
}

// ── 8: Beat Flash ────────────────────────────────────────────────────────────
static void viz_beatflash(MatrixPanel_I2S_DMA* d){
    float dt=_dt();
    bf_avg=bf_avg*0.96f+g_amplitude*0.04f;
    if(g_amplitude>bf_avg*2.3f&&g_amplitude>0.12f&&bf_fade<0.15f){
        static uint8_t hi=0;
        bf_col=hue565(hi/12.f); hi=(hi+1)%12; bf_fade=1.f;
    }
    d->fillScreen(dim565(bf_col,bf_fade*bf_fade));
    for(int b=0;b<SPECTRUM_BINS;b++){
        int bH=constrain((int)(g_spectrum[b]*SD_H+.5f),0,SD_H);
        if(bH>0){
            uint16_t c=dim565(hue565((float)b/SPECTRUM_BINS),0.35f);
            d->drawFastVLine(b*2,  SD_H-bH,bH,c);
            d->drawFastVLine(b*2+1,SD_H-bH,bH,c);
        }
    }
    bf_fade=fmaxf(0.f,bf_fade-dt*2.8f);
}

// ── 9: Plasma ────────────────────────────────────────────────────────────────
// fast_sin replaces 4 sinf() calls × 1536 pixels = 6144 sinf() → 6144 table lookups
static void viz_plasma(MatrixPanel_I2S_DMA* d){
    float dt=_dt();
    static float t=0; t+=dt*(0.9f+g_amplitude*3.f);
    for(int y=0;y<SD_H;y++){
        float cy2=y-SD_H/2.f;
        for(int x=0;x<SD_W;x++){
            float cx2=x-SD_W/2.f;
            float v = fast_sin(x*0.18f+t)
                    + fast_sin(y*0.90f+t*1.4f)
                    + fast_sin((x+y)*0.12f+t*0.8f)
                    + fast_sin(sqrtf(cx2*cx2+cy2*cy2+1)*0.25f+t);
            d->drawPixel(x,y,hue565((v+4.f)/8.f));
        }
    }
}

// ── 10: Starfield ────────────────────────────────────────────────────────────
static void viz_stars(MatrixPanel_I2S_DMA* d){
    float dt=_dt();
    if(!stars_ok){
        stars_ok=true;
        for(auto& s:stars){
            s.x=(fast_rand8()-128)*(SD_W/256.f);
            s.y=(fast_rand8()-128)*(SD_H/256.f);
            s.z=fast_rand8()/25.5f+0.1f;
            s.hue=fast_rand8()/255.f;
        }
    }
    float spd=dt*(1.2f+g_amplitude*10.f);
    d->clearScreen();
    for(auto& s:stars){
        s.z-=spd;
        if(s.z<=0.05f){
            s.x=(fast_rand8()-128)*(SD_W/256.f);
            s.y=(fast_rand8()-128)*(SD_H/256.f);
            s.z=10.f; s.hue=fast_rand8()/255.f;
        }
        int sx=(int)(s.x/s.z*(SD_W/2.f))+SD_W/2;
        int sy=(int)(s.y/s.z*(SD_H/2.f))+SD_H/2;
        if(sx>=0&&sx<SD_W&&sy>=0&&sy<SD_H){
            float br=1.f-s.z/10.f;
            d->drawPixel(sx,sy,dim565(hue565(s.hue+(1.f-s.z/10.f)*0.08f),br));
        }
    }
}

// ── 12: Color Mixer ───────────────────────────────────────────────────────────
static void viz_colormix(MatrixPanel_I2S_DMA* d) {
    d->fillRect(0, 0, SD_W, SD_H, d->color565(g_mix_b, g_mix_g, g_mix_r));
}

// ── 11: Breathing Light (4-7-8 technique) ────────────────────────────────────
static void viz_breathe(MatrixPanel_I2S_DMA* d) {
    const uint32_t INHALE = 4000, HOLD = 7000, EXHALE = 8000, CYCLE = 19000;
    uint32_t t = (millis() - g_breathe_start) % CYCLE;

    float bright;
    const char* phase;
    uint32_t secs;

    if (t < INHALE) {
        float p = (float)t / INHALE;
        bright = p * p;                          // ease-in
        phase = "INHALE";
        secs  = (INHALE - t + 999) / 1000;
    } else if (t < INHALE + HOLD) {
        bright = 1.0f;
        phase = "HOLD";
        secs  = (INHALE + HOLD - t + 999) / 1000;
    } else {
        float p = (float)(t - INHALE - HOLD) / EXHALE;
        float q = 1.0f - p;
        bright = q * q;                          // ease-out
        phase = "EXHALE";
        secs  = (CYCLE - t + 999) / 1000;
    }

    // Hue drifts aqua(190°)→pure blue(236°)→soft violet(270°) over the 19 s cycle.
    // Saturation 0.65 gives pastel spa-softness; value capped at 0.9 prevents harshness.
    // Both cycle ends are near-black so the loop seam is invisible.
    const float HUE0 = 190.0f, HUE1 = 270.0f, SAT = 0.65f;
    float hue = HUE0 + (HUE1 - HUE0) * ((float)t / CYCLE);
    float val = bright * 0.90f;
    float h6  = hue / 60.0f;
    int   hi  = (int)h6 % 6;
    float ff  = h6 - (int)h6;
    float pv  = val * (1.0f - SAT);
    float qv  = val * (1.0f - SAT * ff);
    float tv  = val * (1.0f - SAT * (1.0f - ff));
    float fR, fG, fB;
    switch (hi) {
        case 0: fR=val; fG=tv;  fB=pv;  break;
        case 1: fR=qv;  fG=val; fB=pv;  break;
        case 2: fR=pv;  fG=val; fB=tv;  break;
        case 3: fR=pv;  fG=qv;  fB=val; break;
        case 4: fR=tv;  fG=pv;  fB=val; break;
        default:fR=val; fG=pv;  fB=qv;  break;
    }
    uint8_t r = (uint8_t)(fR * 255);
    uint8_t g = (uint8_t)(fG * 255);
    uint8_t b = (uint8_t)(fB * 255);
    d->fillRect(0, 0, SD_W, SD_H, d->color565(b, g, r));

    // Phase label + countdown — TomThumb 3×5 font keeps text unobtrusive
    char label[12];
    snprintf(label, sizeof(label), "%s %u", phase, (unsigned int)secs);
    d->setFont(&TomThumb);   // 3×5 px glyphs, ~4 px wide per char
    d->setTextSize(1);
    d->setTextWrap(false);
    uint16_t tc = bright > 0.55f
        ? d->color565(10, 5, 40)        // deep indigo ink on bright glow
        : d->color565(160, 180, 255);   // soft periwinkle on dark background
    d->setTextColor(tc);
    int lx = (SD_W - (int)strlen(label) * 4) / 2;
    if (lx < 0) lx = 0;
    d->setCursor(lx, 5);     // TomThumb baseline: y=5 places glyphs at rows 0-4
    d->print(label);
    d->setFont(nullptr);     // restore built-in font for all other modes
}

// ── dispatcher ────────────────────────────────────────────────────────────────
void drawVisualization(MatrixPanel_I2S_DMA* d){
    initVisualizer();  // allocates PSRAM buffers + sin table — no-op after first call
    switch(g_viz_mode){
        case 0:  viz_spectrum(d);  break;
        case 1:  viz_mirror(d);    break;
        case 2:  viz_waterfall(d); break;
        case 3:  viz_colorgan(d);  break;
        case 4:  viz_osc(d);       break;
        case 5:  viz_echowave(d);  break;
        case 6:  viz_fire(d);      break;
        case 7:  viz_vumeter(d);   break;
        case 8:  viz_beatflash(d); break;
        case 9:  viz_plasma(d);    break;
        case 10: viz_stars(d);     break;
        case 11: viz_breathe(d);   break;
        case 12: viz_colormix(d);  break;
        default: viz_spectrum(d);  break;
    }
}
