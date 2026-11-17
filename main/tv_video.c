/* Experimental ESP32-WROOM-32 composite output for the S022C.
 * GPIO25 = DAC1 = LCD D5. screen_suspend_for_composite() MUST run before
 * tv_video_start(): it releases the ESP32 I2S0 LCD bus and parks LCD WR/CS.
 * GPIO26 DAC2 audio is intentionally not touched.
 * Upstream video generator: aquaticus/esp32_composite_video_lib, GPL-3.0.
 */
#include "tv_video.h"
#include "media.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
#include "video.h"
#endif

#define TV_WIDTH 320
#ifdef CONFIG_CLOCK_COMPOSITE_NTSC
#define TV_HEIGHT 240
#else
#define TV_HEIGHT 256
#endif
#define TV_TAG "clock_tv"

typedef struct {char id;uint8_t rows[7];} tv_glyph_t;
static const tv_glyph_t glyphs[]={
 {'A',{14,17,17,31,17,17,17}},{'B',{30,17,17,30,17,17,30}},
 {'C',{14,17,16,16,16,17,14}},{'D',{30,17,17,17,17,17,30}},
 {'E',{31,16,16,30,16,16,31}},{'F',{31,16,16,30,16,16,16}},
 {'G',{14,17,16,23,17,17,14}},{'H',{17,17,17,31,17,17,17}},
 {'I',{31,4,4,4,4,4,31}},{'J',{7,2,2,2,18,18,12}},
 {'K',{17,18,20,24,20,18,17}},{'L',{16,16,16,16,16,16,31}},
 {'M',{17,27,21,21,17,17,17}},{'N',{17,25,21,19,17,17,17}},
 {'O',{14,17,17,17,17,17,14}},{'P',{30,17,17,30,16,16,16}},
 {'Q',{14,17,17,17,21,18,13}},{'R',{30,17,17,30,20,18,17}},
 {'S',{15,16,16,14,1,1,30}},{'T',{31,4,4,4,4,4,4}},
 {'U',{17,17,17,17,17,17,14}},{'V',{17,17,17,17,17,10,4}},
 {'W',{17,17,17,21,21,21,10}},{'X',{17,17,10,4,10,17,17}},
 {'Y',{17,17,10,4,4,4,4}},{'Z',{31,1,2,4,8,16,31}},
 {'0',{14,17,19,21,25,17,14}},{'1',{4,12,4,4,4,4,14}},
 {'2',{14,17,1,2,4,8,31}},{'3',{30,1,1,14,1,1,30}},
 {'4',{2,6,10,18,31,2,2}},{'5',{31,16,16,30,1,1,30}},
 {'6',{14,16,16,30,17,17,14}},{'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}},{'9',{14,17,17,15,1,1,14}},
 {' ',{0,0,0,0,0,0,0}},{'.',{0,0,0,0,0,12,12}},
 {'-',{0,0,0,31,0,0,0}},{'/',{1,1,2,4,8,16,16}},
 {':',{0,4,4,0,4,4,0}},{'?',{14,17,1,2,4,0,4}},
 {'%',{25,26,2,4,8,11,19}},{'_',{0,0,0,0,0,0,31}}
};
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
static bool live, bitmap_mode;
static uint32_t *pixels;
static uint32_t frame_counter;
static int last_anim_tick;
static int64_t next_video_diagnostic_us;
static uint32_t previous_video_irq_count;
static bool video_stall_reported;
static inline void dot(int x,int y,bool white)
{
 if(x<0||x>=TV_WIDTH||y<0||y>=TV_HEIGHT)return;
 uint32_t *p=&pixels[(unsigned)y*(TV_WIDTH/32)+(unsigned)x/32];
 uint32_t bit=1u<<(31u-((unsigned)x&31u));
 if(white)*p|=bit;else *p &=~bit;
}
static void fill_rect(int x,int y,int w,int h,bool white)
{
 if(w<=0||h<=0)return;
 for(int yy=y;yy<y+h;++yy)
  for(int xx=x;xx<x+w;++xx)dot(xx,yy,white);
}
static void label(int x,int y,const char *str,int scale)
{
 for(int i=0;str && str[i] && i<46;++i){
  char c=str[i];if(c>='a'&&c<='z')c=c-'a'+'A';
  const uint8_t *r=NULL;
  for(unsigned j=0;j<sizeof(glyphs)/sizeof(glyphs[0]);++j)
   if(c==glyphs[j].id){r=glyphs[j].rows;break;}
  if(!r){c='?';for(unsigned j=0;j<sizeof(glyphs)/sizeof(glyphs[0]);++j)
   if(c==glyphs[j].id){r=glyphs[j].rows;break;}}
  if(r)for(int row=0;row<7;++row)for(int col=0;col<5;++col)
   if(r[row]&(1u<<(4-col)))
    fill_rect(x+col*scale,y+row*scale,scale,scale,true);
  x+=6*scale;
  if(x>=TV_WIDTH-6)break;
 }
}
static void tv_frame(const char *filename)
{
 memset(pixels,0,(size_t)TV_WIDTH*TV_HEIGHT/8);
 fill_rect(8,9,TV_WIDTH-16,2,true);
 fill_rect(8,TV_HEIGHT-10,TV_WIDTH-16,2,true);
 fill_rect(8,9,2,TV_HEIGHT-18,true);
 fill_rect(TV_WIDTH-10,9,2,TV_HEIGHT-18,true);
 label(20,22,"100 PERCENT CLOCK",2);
 label(20,52,bitmap_mode?"TF BMP TV PREVIEW":"TF AUDIO PLAYER",2);
 /* Trim a long UTF-8 filename to a readable ASCII diagnostic. */
 char display[41];int j=0;
 for(const unsigned char *p=(const unsigned char*)filename;*p&&j<39;++p)
  display[j++]=*p>=32&&*p<127?(char)*p:'?';
 display[j]=0;
 label(20,87,display,1);
 if(bitmap_mode){
  /* RGB565 BGR bitmap decoder holds a 128x165 thumb. Convert to
   * ordered-dithered monochrome without allocating another image. */
  const uint16_t *image=media_bitmap();
  if(image){
   static const uint8_t bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
   for(int y=0;y<MEDIA_BMP_HEIGHT;++y)for(int x=0;x<MEDIA_BMP_WIDTH;++x){
    uint16_t rgb=image[y*MEDIA_BMP_WIDTH+x];
    unsigned g=(rgb>>5)&63,b=rgb>>11,r=rgb&31;
    unsigned gray=(r*2+g+b*2)/4; // approximate BGR brightness
    if(gray>bayer[(x&3)+((y&3)<<2)]*4)
       dot(94+x,90+y-0,true);
   }
  }
 }else{
  label(20,112,"GPIO26 AUDIO ACTIVE",1);
  label(20,130,"GPIO25 MONO COMPOSITE",1);
  label(20,206,"TAP STOP ON LCD TO EXIT",1);
 }
}
#endif
bool tv_video_active(void)
{
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
 return live;
#else
 return false;
#endif
}
bool tv_video_start(const char *filename,bool bmp)
{
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
 if(live)return true;
 bitmap_mode=bmp;
 /* LCD must be released before this call. Both use I2S0. */
 video_graphics(
#ifdef CONFIG_CLOCK_COMPOSITE_NTSC
     NTSC_320x240,
#else
     PAL_320x256,
#endif
     FB_FORMAT_GREY_1BPP);
 pixels=(uint32_t*)video_get_frame_buffer_address();
 if(!pixels){ESP_LOGE(TV_TAG,"No composite framebuffer");video_stop();return false;}
 live=true;frame_counter=0;last_anim_tick=-1;
 next_video_diagnostic_us=esp_timer_get_time()+1000000LL;
 previous_video_irq_count=0;video_stall_reported=false;
 tv_frame(filename?filename:"");
 ESP_LOGW(TV_TAG,"Composite initialized on GPIO25; checking I2S DMA activity next. LCD frozen.");
 ESP_LOGW(TV_TAG,"GPIO26 DAC audio may be overridden by global digital-DAC selection");
 return true;
#else
 (void)filename;(void)bmp;return false;
#endif
}
void tv_video_tick(int64_t now_us)
{
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
 if(!live)return;
 /* Run the 5s preparation + 3x15s DAC voltage test from the normal
  * touchscreen loop, so BACK/STOP continues to work immediately. */
 video_dac_selftest_tick(now_us);
 if(now_us>=next_video_diagnostic_us){
  uint32_t irq_count=video_dma_eof_count();
  uint32_t delta=irq_count-previous_video_irq_count;
  if(delta==0){
   ESP_LOGE(TV_TAG,"COMPOSITE DMA STALLED: 0 I2S0 EOF interrupts during last second; no video scanlines");
   if(!video_stall_reported){video_dma_log_stall_state();video_stall_reported=true;}
  }else{
   video_stall_reported=false;
   ESP_LOGI(TV_TAG,"Composite DMA active: %lu scanlines/s (I2S EOF), total=%lu; TV sync/voltage still unverified",
            (unsigned long)delta,(unsigned long)irq_count);
  }
  previous_video_irq_count=irq_count;
  next_video_diagnostic_us=now_us+1000000LL;
 }
 if(bitmap_mode)return;
 int tick=(int)(now_us/160000LL);
 if(tick==last_anim_tick)return;
 last_anim_tick=tick;
 /* Test pattern, not a real FFT; intentionally low CPU overhead. */
 uint32_t rng=frame_counter++*1664525u+1013904223u;
 for(int col=0;col<18;++col){
  rng=1664525u*rng+1013904223u;
  int x=20+col*15,h=14+(int)((rng>>23)%46);
  fill_rect(x,145,10,58,false);
  fill_rect(x,203-h,10,h,true);
 }
#else
 (void)now_us;
#endif
}
void tv_video_stop(void)
{
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
 if(!live)return;
 video_stop();pixels=NULL;live=false;
 ESP_LOGI(TV_TAG,"Composite stopped; GPIO25 returned for LCD");
#endif
}
