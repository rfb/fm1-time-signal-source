/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "clock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned level = 127;
unsigned fm1_engine_output_level(void) { return level; }
int fm1_usb_audio_read(int32_t *p, unsigned n, uint16_t g[2])
{ for(unsigned i=0;i<n*2;i++) p[i]=8388607; g[0]=g[1]=128; return n; }
int fm1_bluetooth_audio_read(int16_t *p, unsigned n)
{ for(unsigned i=0;i<n*2;i++) p[i]=32767; return n; }
int fm1_usb_audio_write(const int32_t *p, unsigned n) { (void)p; return n; }

/* Independent model of the clock after clock_set: seconds of 44117 or 44118 frames. */
enum { SECONDS = 195, MAX_FRAMES = SECONDS * 44119 };
static int32_t *out;
static unsigned starts[SECONDS + 1], lengths[SECONDS + 1];
static unsigned crossings(unsigned from, unsigned to)
{
    unsigned n=0;
    for(unsigned i=from+1;i<to;i++) n += out[i-1]<=0 && out[i]>0;
    return n;
}
static int silent(unsigned from, unsigned to)
{
    for(unsigned i=from;i<to;i++) if(out[i]) return 0;
    return 1;
}
static unsigned near(unsigned value, unsigned expected) { return value+2>=expected && value<=expected+2; }
int main(void)
{
    out=malloc(MAX_FRAMES*sizeof *out);
    assert(out);
    int32_t block[FM1_BLOCK_WORDS];
    fm1_output_reset();
    /* Local 10:59:50.000 at UTC-08:48 is 19:47:50 UTC. */
    clock_set((19*3600+47*60+50)*1000U);
    unsigned length=CLOCK_RATE_DEFAULT/1000, carry=0, total=0;
    for(unsigned s=0;s<=SECONDS;s++) {
        starts[s]=total; lengths[s]=length; total+=length;
        carry+=CLOCK_RATE_DEFAULT%1000; length=CLOCK_RATE_DEFAULT/1000;
        if(carry>=1000){carry-=1000;++length;}
    }
    for(unsigned n=0;n+FM1_BLOCK_FRAMES<=MAX_FRAMES && n<starts[SECONDS];n+=FM1_BLOCK_FRAMES) {
        fm1_output_mix(block);
        for(unsigned i=0;i<FM1_BLOCK_FRAMES;i++) { assert(block[2*i]==block[2*i+1]); out[n+i]=block[2*i]; }
    }
    int32_t peak_tick=0, peak_tone=0;
    for(unsigned s=0;s<SECONDS;s++) {
        unsigned local=10*3600+59*60+50+s, minute=local/60%60, second=local%60;
        unsigned a=starts[s], n=lengths[s];
        unsigned tone = (minute==0||minute==3||minute==29||minute==30||minute==59||(minute>=43&&minute<=51)) ? 0
                      : minute==2 ? 440 : minute%2 ? 600 : 500;
        if(second==0) {
            unsigned hz = minute ? 1000 : 1500;
            assert(near(crossings(a,a+35294), hz*4/5));        /* 800 ms marker */
            assert(silent(a+35294,a+n));                        /* then 200 ms silence */
            continue;
        }
        unsigned tick = second!=29 && second!=59, body=a;
        if(tick) {
            assert(near(crossings(a,a+221), 5));               /* five cycles */
            for(unsigned i=a;i<a+221;i++) if(out[i]>peak_tick) peak_tick=out[i];
            assert(silent(a+221,a+1324));                       /* 30 ms after */
            body=a+1324;
        }
        unsigned end = second==28||second==58 ? a+n : a+n-441;  /* 10 ms before */
        if(end<a+n) assert(silent(end,a+n));
        if(tone && second<=44) {
            unsigned expect=(unsigned)((double)tone*(end-body)/44118.0);
            assert(near(crossings(body,end), expect));
            for(unsigned i=body;i<end;i++) if(out[i]>peak_tone) peak_tone=out[i];
        } else
            assert(silent(body,end));
    }
    assert(peak_tick<=2080704 && peak_tick>2000000);           /* about -12 dBFS */
    assert(peak_tone<=1040353 && peak_tone>1000000);           /* about -18 dBFS */
    struct clock_time now;
    clock_read(&now);
    assert(now.set && now.second==(19*3600+47*60+50+SECONDS)%CLOCK_DAY);
    assert(clock_rate_set(44000000)<0 && clock_offset_set(-721)<0 && clock_offset_set(-528)==0);
    level=0; fm1_output_reset(); fm1_output_mix(block);
    for(unsigned i=0;i<FM1_BLOCK_WORDS;i++) assert(block[i]==0);
    puts("PASS: hour/minute markers, ticks, protected zones, 440/500/600 Hz schedule, levels, muted master, input isolation");
    free(out);
}
