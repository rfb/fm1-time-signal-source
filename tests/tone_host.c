/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "clock.h"
#include <assert.h>
#include <stdio.h>
static unsigned level = 127;
unsigned fm1_engine_output_level(void) { return level; }
int fm1_usb_audio_read(int32_t *p, unsigned n, uint16_t g[2])
{ for(unsigned i=0;i<n*2;i++) p[i]=8388607; g[0]=g[1]=128; return n; }
int fm1_bluetooth_audio_read(int16_t *p, unsigned n)
{ for(unsigned i=0;i<n*2;i++) p[i]=32767; return n; }
int fm1_usb_audio_write(const int32_t *p, unsigned n) { (void)p; return n; }
/* Independent model of the clock: seconds of 44117 or 44118 frames. */
static unsigned model_second, model_frame, model_length=CLOCK_RATE_DEFAULT/1000, model_carry;
static void model_step(void)
{
    if(++model_frame<model_length) return;
    model_frame=0; model_second=(model_second+1)%CLOCK_DAY;
    model_carry+=CLOCK_RATE_DEFAULT%1000; model_length=CLOCK_RATE_DEFAULT/1000;
    if(model_carry>=1000){model_carry-=1000;++model_length;}
}
int main(void)
{
    int32_t out[FM1_BLOCK_WORDS];
    unsigned crossings=0, frames=0;
    int32_t previous=0, peak=0;
    fm1_output_reset();
    for(unsigned start=0;start<6*44118;start+=128) {
        fm1_output_mix(out);
        for(unsigned i=0;i<128;i++,frames++) {
            assert(out[2*i]==out[2*i+1]);
            int32_t s=out[2*i];
            if(model_second&1) assert(s==0);
            if(model_second==0 && model_frame>1000 && model_frame<44000 && previous<=0 && s>0) crossings++;
            if(s>peak) peak=s;
            previous=s;
            model_step();
        }
    }
    assert(crossings>=973 && crossings<=975);
    assert(peak<=1040353 && peak>1000000);
    struct clock_time now;
    clock_read(&now);
    assert(!now.set && now.second==model_second);
    /* Set 12:34:56.500: the next block runs in that second, tone on (even). */
    clock_set((12*3600+34*60+56)*1000U+500);
    fm1_output_mix(out);
    clock_read(&now);
    assert(now.set && now.second==12*3600+34*60+56 && now.ms>=502 && now.ms<=504);
    fm1_output_mix(out);
    assert(fm1_output_tone());
    assert(clock_rate_set(44000000)<0 && clock_offset_set(-721)<0 && clock_offset_set(330)==0);
    level=0; fm1_output_reset(); fm1_output_mix(out);
    for(unsigned i=0;i<FM1_BLOCK_WORDS;i++) assert(out[i]==0);
    puts("PASS: clock-timed tone, silent odd seconds, stereo, amplitude, time set, muted master, input isolation");
}
