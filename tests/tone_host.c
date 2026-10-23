/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include <assert.h>
#include <stdio.h>
static unsigned level = 127;
unsigned fm1_engine_output_level(void) { return level; }
int fm1_usb_audio_read(int32_t *p, unsigned n, uint16_t g[2])
{ for(unsigned i=0;i<n*2;i++) p[i]=8388607; g[0]=g[1]=128; return n; }
int fm1_bluetooth_audio_read(int16_t *p, unsigned n)
{ for(unsigned i=0;i<n*2;i++) p[i]=32767; return n; }
int fm1_usb_audio_write(const int32_t *p, unsigned n) { (void)p; return n; }
int main(void)
{
    int32_t out[FM1_BLOCK_WORDS];
    unsigned crossings=0;
    int32_t previous=0, peak=0;
    fm1_output_reset();
    for(unsigned start=0;start<4*44118;start+=128) {
        fm1_output_mix(out);
        for(unsigned i=0;i<128 && start+i<4*44118;i++) {
            unsigned at=start+i;
            assert(out[2*i]==out[2*i+1]);
            int32_t s=out[2*i];
            if(at%88236>=44118) assert(s==0);
            if(at>1000 && at<44000 && previous<=0 && s>0) crossings++;
            if(s>peak) peak=s;
            previous=s;
        }
    }
    assert(crossings>=973 && crossings<=975);
    assert(peak<=1040353 && peak>1000000);
    level=0; fm1_output_reset(); fm1_output_mix(out);
    for(unsigned i=0;i<FM1_BLOCK_WORDS;i++) assert(out[i]==0);
    puts("PASS: frequency, silent intervals, stereo, amplitude, muted master, input isolation");
}
