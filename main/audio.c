/* Audio for the ESP32-2432S022C.
 * LCD i80 uses I2S0, as would DAC DMA: never use DAC continuous mode.
 * Instead a 10 kHz task-dispatched esp_timer updates GPIO26 DAC channel 1,
 * while a FreeRTOS task decodes SD files / generates ORIGINAL melodies.
 * Avoid GPTimer's high-frequency hardware ISR: the previous version
 * panicked in gptimer_default_isr when TF audio playback was started.
 * MP3 decoder: minimp3 (CC0), see minimp3-LICENSE.txt.
 */
#include "audio.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "driver/dac_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define RATE 10000u
#define RING 4096u
#define MP3BUF 8192u
#define WAVBUF 2048u
#define NOTES 256u
#define N(h,m) {h,m}
typedef struct {uint16_t hz,ms;} tone_t;
static const tone_t classic[]={N(523,170),N(659,170),N(784,370),N(0,1100)};
static const tone_t sunrise[]={
 N(392,150),N(523,150),N(659,150),N(784,300),N(659,150),N(523,150),N(440,300),N(0,130),
 N(440,150),N(587,150),N(698,150),N(880,300),N(784,150),N(659,150),N(523,300),N(0,210),
 N(659,150),N(784,150),N(988,150),N(1047,300),N(880,150),N(784,150),N(659,300),N(0,190)
};
static const tone_t rush[]={
 N(220,100),N(440,100),N(330,100),N(659,100),N(440,100),N(880,100),N(330,100),N(659,200),N(0,75),
 N(247,100),N(494,100),N(370,100),N(740,100),N(494,100),N(988,100),N(370,100),N(740,200),N(0,75),
 N(262,100),N(523,100),N(392,100),N(784,100),N(523,100),N(1047,100),N(392,100),N(784,330),N(0,150)
};
/* An original late-1990s bedside/radio alarm: closely spaced electronic
 * double/triple beeps in alternating pitches, not a movie soundtrack sample. */
static const tone_t clock1999[]={
 N(1047,165),N(0,85),N(1047,165),N(0,85),N(1047,210),N(0,430),
 N(988,165),N(0,85),N(988,165),N(0,85),N(988,210),N(0,450),
 N(1175,175),N(0,95),N(1047,175),N(0,95),N(880,260),N(0,700)
};
static const tone_t night[]={
 N(196,250),N(0,85),N(294,125),N(349,125),N(392,250),N(294,125),N(262,125),N(196,250),N(0,85),
 N(220,250),N(0,85),N(330,125),N(392,125),N(440,250),N(330,125),N(294,125),N(220,250),N(0,100),
 N(175,220),N(262,220),N(349,220),N(523,280),N(392,220),N(294,220),N(196,400),N(0,230)
};
typedef struct {const char *name;const tone_t *notes;size_t count;} tune_t;
static const tune_t tunes[AUDIO_BUILTIN_COUNT]={
 {"CLASSIC CHIME",classic,sizeof classic/sizeof *classic},
 {"PIXEL SUNRISE",sunrise,sizeof sunrise/sizeof *sunrise},
 {"8-BIT RUSH",rush,sizeof rush/sizeof *rush},
 {"NIGHT CIRCUIT",night,sizeof night/sizeof *night},
 {"1999 RADIO CLOCK",clock1999,sizeof clock1999/sizeof *clock1999}
};
typedef enum {NONE,TUNES,WAV,MP3} format_t;
typedef struct {
 format_t format;
 bool loop;
 FILE *file;
 long begin;
 tone_t custom[NOTES];
 const tone_t *notes;
 size_t note_count,note_index;
 uint32_t samples_left,phase;
 uint32_t rate,src_phase;
 int channels,bits;
 int16_t held;
 uint8_t wav[WAVBUF];
 size_t wav_len,wav_pos;
 uint32_t wav_left,wav_total;
 uint8_t mp3[MP3BUF];
 size_t mp3_len;
 bool eof;
 mp3dec_t decoder;
 mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
 int pcm_len,pcm_pos;
 uint32_t decoded_frames;
} source_t;
static source_t src;
static SemaphoreHandle_t lock;
static esp_timer_handle_t timer;
static dac_oneshot_handle_t output;
static char request[AUDIO_KEY_MAX];
static bool request_loop;
static uint32_t request_version;
static volatile bool playback_active;
static uint8_t fifo[RING];
static volatile uint32_t producer,consumer;
static const char *TAG="clock_audio";

const char *audio_builtin_name(int i)
{return i>=0&&i<AUDIO_BUILTIN_COUNT?tunes[i].name:tunes[0].name;}
void audio_builtin_key(int i,char *out,size_t len)
{if(len)snprintf(out,len,"builtin:%d",i>=0&&i<AUDIO_BUILTIN_COUNT?i:0);}
void audio_sd_key(const char *name,char *out,size_t len)
{if(len)snprintf(out,len,"sd:%s",name?name:"");}
static bool has_ext(const char *name,const char *suffix)
{size_t a=strlen(name),b=strlen(suffix);return a>=b&&!strcasecmp(name+a-b,suffix);}
bool audio_file_supported(const char *name)
{return name&&(has_ext(name,".wav")||has_ext(name,".mp3")||has_ext(name,".rtttl"));}
void audio_key_label(const char *key,char *out,size_t len)
{
 if(!len)return;
 if(key&&!strncmp(key,"sd:",3))snprintf(out,len,"%.35s",key+3);
 else snprintf(out,len,"%s",audio_builtin_name(key&&!strncmp(key,"builtin:",8)?atoi(key+8):0));
}
void audio_play_key(const char *key,bool loop)
{
 if(!lock)return;
 if(xSemaphoreTake(lock,pdMS_TO_TICKS(100))==pdTRUE){
  snprintf(request,sizeof request,"%s",key?key:"");
  request_loop=loop;
  ++request_version;
  xSemaphoreGive(lock);
 }
}
bool audio_playing(void){return playback_active;}
void audio_stop(void)
{
 audio_play_key("",false);
 /* Wait for stream close before an SD rescan can unmount it.
  * Large MP3 files remain streamed; never load the whole file into RAM. */
 for(int i=0;i<200 && playback_active;++i)vTaskDelay(pdMS_TO_TICKS(10));
}
static uint16_t rd16(const uint8_t *b){return (uint16_t)b[0]|((uint16_t)b[1]<<8);}
static uint32_t rd32(const uint8_t *b){return (uint32_t)rd16(b)|((uint32_t)rd16(b+2)<<16);}
static uint16_t note_hz(char key,bool sharp,int octave)
{
 static const uint16_t hz[]={262,294,330,349,392,440,494,0};
 const char *keys="cdefgabp";
 const char *pos=strchr(keys,tolower((unsigned char)key));
 if(!pos||octave<3||octave>7)return 0;
 uint32_t freq=hz[pos-keys];
 if(sharp&&freq)freq=freq*10595u/10000u;
 while(octave>4){freq*=2;octave--;}
 while(octave<4){freq=(freq+1)/2;octave++;}
 return freq>4000?4000:(uint16_t)freq;
}
static bool load_rtttl(source_t *s)
{
 char line[2500];
 if(!fgets(line,sizeof line,s->file))return false;
 char *a=strchr(line,':');if(!a)return false;
 char *b=strchr(a+1,':');if(!b)return false;
 *b++=0;
 int duration=4,octave=5,bpm=125;
 char *save=NULL;
 for(char *v=strtok_r(a+1,",",&save);v;v=strtok_r(NULL,",",&save)){
  while(*v==' ')++v;
  if(!strncmp(v,"d=",2))duration=atoi(v+2);
  if(!strncmp(v,"o=",2))octave=atoi(v+2);
  if(!strncmp(v,"b=",2))bpm=atoi(v+2);
 }
 if(bpm<30||bpm>400||duration<1||duration>64)return false;
 for(char *v=strtok_r(b,", \r\n",&save);v && s->note_count<NOTES;
     v=strtok_r(NULL,", \r\n",&save)){
  int d=0;while(isdigit((unsigned char)*v)){d=d*10+(*v++-'0');if(d>64)return false;}
  if(!d)d=duration;
  if(!*v)return false;
  char key=*v++;
  bool sharp=false,dotted=false;
  if(*v=='#'){sharp=true;++v;}
  if(*v=='.'){dotted=true;++v;}
  int o=octave;
  if(isdigit((unsigned char)*v))o=*v++-'0';
  if(*v=='.')dotted=true;
  int ms=(int)(240000LL/bpm/d);
  if(dotted)ms=ms*3/2;
  if(ms<20)ms=20;
  if(ms>12000)ms=12000;
  s->custom[s->note_count++]=(tone_t){note_hz(key,sharp,o),(uint16_t)ms};
 }
 s->notes=s->custom;s->format=TUNES;
 return s->note_count>0;
}
static bool load_wav(source_t *s)
{
 uint8_t h[12];
 if(fread(h,1,12,s->file)!=12||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4))return false;
 bool have_format=false;
 for(int i=0;i<40;++i){
  uint8_t tag[8];if(fread(tag,1,8,s->file)!=8)return false;
  uint32_t n=rd32(tag+4);
  if(!memcmp(tag,"fmt ",4)){
   uint8_t fmt[16];
   if(n<16||fread(fmt,1,16,s->file)!=16||rd16(fmt)!=1)return false;
   s->channels=rd16(fmt+2);s->rate=rd32(fmt+4);s->bits=rd16(fmt+14);
   if((s->channels!=1&&s->channels!=2)||(s->bits!=8&&s->bits!=16)||
      s->rate<8000||s->rate>48000)return false;
   if(n>16&&fseek(s->file,(long)(n-16),SEEK_CUR))return false;
   have_format=true;
  }else if(!memcmp(tag,"data",4)){
   if(!have_format||!n)return false;
   s->begin=ftell(s->file);s->wav_left=s->wav_total=n;
   s->format=WAV;return true;
  }else if(n>64u*1024u*1024u||fseek(s->file,(long)n,SEEK_CUR))return false;
  if((n&1)&&fseek(s->file,1,SEEK_CUR))return false;
 }
 return false;
}
static bool get_wav_byte(source_t *s,uint8_t *out)
{
 if(!s->wav_left)return false;
 if(s->wav_pos==s->wav_len){
  size_t want=s->wav_left<WAVBUF?s->wav_left:WAVBUF;
  s->wav_len=fread(s->wav,1,want,s->file);s->wav_pos=0;
  if(!s->wav_len)return false;
 }
 *out=s->wav[s->wav_pos++];--s->wav_left;return true;
}
static bool get_wav_sample(source_t *s,int16_t *out)
{
 int val=0;
 for(int c=0;c<s->channels;++c){
  uint8_t a,b;
  if(!get_wav_byte(s,&a))return false;
  if(s->bits==8)val+=((int)a-128)*256;
  else {if(!get_wav_byte(s,&b))return false;val+=(int16_t)((uint16_t)a|((uint16_t)b<<8));}
 }
 *out=(int16_t)(val/s->channels);return true;
}
static void skip_id3(FILE *f)
{
 uint8_t h[10];
 if(fread(h,1,10,f)==10&&!memcmp(h,"ID3",3)&&
    !(h[6]&128)&&!(h[7]&128)&&!(h[8]&128)&&!(h[9]&128)){
  uint32_t len=((uint32_t)h[6]<<21)|((uint32_t)h[7]<<14)|((uint32_t)h[8]<<7)|h[9];
  fseek(f,(long)(10+len+((h[5]&0x10)?10:0)),SEEK_SET);
 }else fseek(f,0,SEEK_SET);
}
static bool get_mp3_sample(source_t *s,int16_t *out)
{
 while(s->pcm_pos>=s->pcm_len){
  if(s->mp3_len<4096 && !s->eof){
   size_t n=fread(s->mp3+s->mp3_len,1,MP3BUF-s->mp3_len,s->file);
   s->mp3_len+=n;if(!n)s->eof=true;
  }
  if(!s->mp3_len)return false;
  mp3dec_frame_info_t info={0};
  int frames=mp3dec_decode_frame(&s->decoder,s->mp3,(int)s->mp3_len,
                                  s->pcm,&info);
  size_t consumed=info.frame_bytes>0?(size_t)info.frame_bytes:1;
  if(consumed>s->mp3_len)consumed=s->mp3_len;
  memmove(s->mp3,s->mp3+consumed,s->mp3_len-consumed);
  s->mp3_len-=consumed;
  if(frames>0&&(info.channels==1||info.channels==2)&&
     info.hz>=8000&&info.hz<=48000){
   s->rate=info.hz;s->channels=info.channels;
   s->pcm_len=frames;s->pcm_pos=0;
   /* First frame and every 1024 frames: observe actual stack margin.
    * No full MP3 is ever copied into RAM; only a streaming window. */
   if((s->decoded_frames++ & 1023u)==0u)
    ESP_LOGI(TAG,"MP3 %dHz, %d channels; decoder stack available=%u bytes",
             info.hz,info.channels,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
  }
 }
 int ix=s->pcm_pos++*s->channels;
 *out=s->channels==2?(int16_t)(((int)s->pcm[ix]+s->pcm[ix+1])/2):s->pcm[ix];
 return true;
}
static bool get_sample(source_t *s,int16_t *out)
{
 return s->format==WAV?get_wav_sample(s,out):
        s->format==MP3?get_mp3_sample(s,out):false;
}
static bool next_byte(source_t *s,uint8_t *out)
{
 if(s->format==TUNES){
  if(s->note_index>=s->note_count)return false;
  tone_t note=s->notes[s->note_index];
  if(!s->samples_left){
   s->samples_left=(uint32_t)note.ms*RATE/1000u;
   if(!s->samples_left)s->samples_left=1;
   s->phase=0;
  }
  uint8_t v=128;
  if(note.hz){
   s->phase+=note.hz;
   if(s->phase>=RATE)s->phase-=RATE;
   v=s->phase<RATE/2?88:168;
  }
  *out=v;
  if(!--s->samples_left)++s->note_index;
  return true;
 }
 if(s->format==NONE)return false;
 s->src_phase+=s->rate;
 while(s->src_phase>=RATE){
  if(!get_sample(s,&s->held))return false;
  s->src_phase-=RATE;
 }
 *out=(uint8_t)(128+s->held/512); /* conservative output volume */
 return true;
}
static void close_source(source_t *s)
{
 if(s->file)fclose(s->file);
 memset(s,0,sizeof *s);
}
static bool open_source(source_t *s,const char *key,bool loop)
{
 close_source(s);s->loop=loop;
 if(!strncmp(key,"builtin:",8)){
  int id=atoi(key+8);
  if(id<0||id>=AUDIO_BUILTIN_COUNT)id=0;
  s->notes=tunes[id].notes;s->note_count=tunes[id].count;s->format=TUNES;
  return true;
 }
 if(strncmp(key,"sd:",3)||!key[3])return false;
 const char *name=key+3;
 if(strchr(name,'/')||strchr(name,'\\')||strstr(name,".."))return false;
 char path[160];snprintf(path,sizeof path,"/sd/%s",name);
 s->file=fopen(path,"rb");
 if(!s->file)return false;
 if(has_ext(name,".rtttl"))return load_rtttl(s);
 if(has_ext(name,".wav"))return load_wav(s);
 if(has_ext(name,".mp3")){
  skip_id3(s->file);s->begin=ftell(s->file);
  mp3dec_init(&s->decoder);s->format=MP3;s->rate=16000;
  return true;
 }
 return false;
}
static bool rewind_source(source_t *s)
{
 if(s->format==TUNES){
  s->note_index=0;s->samples_left=0;s->phase=0;
  return s->note_count>0;
 }
 if(!s->file||fseek(s->file,s->begin,SEEK_SET))return false;
 s->src_phase=0;s->held=0;
 if(s->format==WAV){
  s->wav_pos=s->wav_len=0;s->wav_left=s->wav_total;
  return true;
 }
 if(s->format==MP3){
  s->mp3_len=0;s->eof=false;s->pcm_len=s->pcm_pos=0;
  mp3dec_init(&s->decoder);
  return true;
 }
 return false;
}
static void audio_tick(void *ctx)
{
 (void)ctx;
 /* In esp_timer TASK context, no custom hardware ISR. Never decode here. */
 uint32_t tail=consumer;
 uint8_t v=128;
 if(tail!=producer){v=fifo[tail&(RING-1)];consumer=tail+1;}
 dac_oneshot_output_voltage(output,v);
}
static void audio_worker(void *arg)
{
 (void)arg;
 uint32_t seen=0;
 bool running=false,finished=false;
 char wanted[AUDIO_KEY_MAX]={0};
 uint8_t temp[256];
 for(;;){
  if(xSemaphoreTake(lock,pdMS_TO_TICKS(5))==pdTRUE){
   if(seen!=request_version){
    seen=request_version;
    snprintf(wanted,sizeof wanted,"%s",request);
    bool loop=request_loop;
    xSemaphoreGive(lock);
    if(running){esp_timer_stop(timer);running=false;}
    consumer=producer;
    close_source(&src);finished=false;playback_active=false;
    if(wanted[0]){
     bool ok=open_source(&src,wanted,loop);
     if(!ok){ESP_LOGW(TAG,"Unavailable TF audio, using built-in chime");
              ok=open_source(&src,"builtin:0",loop);}
     if(ok){esp_timer_start_periodic(timer,1000000u/RATE);running=playback_active=true;}
    }else dac_oneshot_output_voltage(output,128);
    continue;
   }
   xSemaphoreGive(lock);
  }
  if(!running){vTaskDelay(pdMS_TO_TICKS(25));continue;}
  uint32_t fill=producer-consumer;
  if(finished){
   if(!fill){esp_timer_stop(timer);running=playback_active=false;
             close_source(&src);dac_oneshot_output_voltage(output,128);}
   else vTaskDelay(pdMS_TO_TICKS(5));
   continue;
  }
  if(fill>=RING-256){vTaskDelay(pdMS_TO_TICKS(5));continue;}
  size_t count=0;
  for(;count<sizeof temp;++count){
   if(next_byte(&src,&temp[count]))continue;
   if(src.loop&&rewind_source(&src)&&next_byte(&src,&temp[count]))continue;
   finished=true;break;
  }
  if(count){
   uint32_t head=producer;
   for(size_t i=0;i<count;++i)fifo[(head+i)&(RING-1)]=temp[i];
   producer=head+(uint32_t)count;
  }
  if(!count)vTaskDelay(pdMS_TO_TICKS(5));
 }
}
void audio_init(void)
{
 lock=xSemaphoreCreateMutex();if(!lock)return;
 dac_oneshot_config_t dac_cfg={.chan_id=DAC_CHAN_1};
 esp_err_t e=dac_oneshot_new_channel(&dac_cfg,&output);
 if(e!=ESP_OK){ESP_LOGE(TAG,"GPIO26 DAC unavailable: %s",esp_err_to_name(e));return;}
 dac_oneshot_output_voltage(output,128);
 /* ESP timer's task dispatch keeps DAC writes out of GPTimer hardware
  * interrupts; also avoids I2S0 used by the parallel LCD. */
 esp_timer_create_args_t timer_cfg={
  .callback=audio_tick,
  .arg=NULL,
  .dispatch_method=ESP_TIMER_TASK,
  .name="clock_pcm",
  .skip_unhandled_events=true
 };
 e=esp_timer_create(&timer_cfg,&timer);
 if(e!=ESP_OK){ESP_LOGE(TAG,"Audio timer unavailable: %s",esp_err_to_name(e));return;}
 /* minimp3 allocates mp3dec_scratch_t on the calling task's stack:
 * the scratch structure is already >16 KiB! The former 8192-byte task
 * overran its stack when decoding large MP3s, corrupting other tasks
 * (panics appeared in GPTimer and esp_timer code). ESP-IDF uses BYTES
 * for xTaskCreatePinnedToCore stack sizes, not FreeRTOS words. */
 const uint32_t worker_stack=32768;
 ESP_LOGI(TAG,"MP3 scratch=%u bytes; decoder task stack=%u bytes",
          (unsigned)sizeof(mp3dec_scratch_t),(unsigned)worker_stack);
 if(xTaskCreatePinnedToCore(audio_worker,"clock_audio",worker_stack,NULL,4,NULL,1)!=pdPASS)
  ESP_LOGE(TAG,"Cannot start audio worker (heap too fragmented)");
 else ESP_LOGI(TAG,"GPIO26 DAC + 10kHz esp_timer TASK (no GPTimer ISR / LCD I2S0)");
}
