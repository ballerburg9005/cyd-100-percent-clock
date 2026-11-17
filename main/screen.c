/* Native graphics: no LVGL, framebuffer or PSRAM required. */
#include "screen.h"
#include "big_font.h"
#include "media.h"
#include "audio.h"
#include "esp_timer.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define W 320
#define H 240
#define STRIP 16
#define I2C_PORT I2C_NUM_0
#define TOUCH_ADDR 0x15

/* Kconfig defaults of 'n' are omitted from sdkconfig.h. */
#ifndef CONFIG_CLOCK_LCD_ROTATE_CW
#define CONFIG_CLOCK_LCD_ROTATE_CW 0
#endif
#ifndef CONFIG_CLOCK_DISPLAY_FLIP_180
#define CONFIG_CLOCK_DISPLAY_FLIP_180 0
#endif
#ifndef CONFIG_CLOCK_LCD_MIRROR_Y
#define CONFIG_CLOCK_LCD_MIRROR_Y 0
#endif
#ifndef CONFIG_CLOCK_TOUCH_SWAP_XY
#define CONFIG_CLOCK_TOUCH_SWAP_XY 0
#endif
#ifndef CONFIG_CLOCK_TOUCH_MIRROR_X
#define CONFIG_CLOCK_TOUCH_MIRROR_X 0
#endif
#ifndef CONFIG_CLOCK_TOUCH_MIRROR_Y
#define CONFIG_CLOCK_TOUCH_MIRROR_Y 0
#endif

typedef struct { char ch; uint8_t row[7]; } font_glyph_t;
static const font_glyph_t FONT[] = {
 {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
 {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
 {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
 {'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
 {'I',{31,4,4,4,4,4,31}}, {'J',{7,2,2,2,18,18,12}},
 {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
 {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
 {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
 {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
 {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
 {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
 {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
 {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
 {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
 {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
 {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
 {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
 {' ',{0,0,0,0,0,0,0}}, {':',{0,4,4,0,4,4,0}},
 {'.',{0,0,0,0,0,12,12}}, {'-',{0,0,0,31,0,0,0}},
 {'/',{1,1,2,4,8,16,16}}, {'%',{25,26,2,4,8,11,19}},
 {'+',{0,4,4,31,4,4,0}}, {'!',{4,4,4,4,4,0,4}},
 {'?',{14,17,1,2,4,0,4}}, {'@',{14,17,23,21,23,16,14}},
 {'_',{0,0,0,0,0,0,31}}, {'*',{0,21,14,31,14,21,0}},
 {'=',{0,0,31,0,31,0,0}}, {',',{0,0,0,0,4,4,8}},
 {'[',{14,8,8,8,8,8,14}}, {']',{14,2,2,2,2,2,14}},
 {'a',{0,0,14,1,15,17,15}},
 {'b',{16,16,30,17,17,17,30}},
 {'c',{0,0,14,17,16,17,14}},
 {'d',{1,1,15,17,17,17,15}},
 {'e',{0,0,14,17,31,16,14}},
 {'f',{6,9,8,28,8,8,8}},
 {'g',{0,15,17,17,15,1,14}},
 {'h',{16,16,30,17,17,17,17}},
 {'i',{4,0,12,4,4,4,14}},
 {'j',{2,0,6,2,2,18,12}},
 {'k',{16,16,18,20,24,20,18}},
 {'l',{12,4,4,4,4,4,14}},
 {'m',{0,0,26,21,21,21,21}},
 {'n',{0,0,30,17,17,17,17}},
 {'o',{0,0,14,17,17,17,14}},
 {'p',{0,30,17,17,30,16,16}},
 {'q',{0,15,17,17,15,1,1}},
 {'r',{0,0,22,25,16,16,16}},
 {'s',{0,0,15,16,14,1,30}},
 {'t',{8,8,28,8,8,9,6}},
 {'u',{0,0,17,17,17,19,13}},
 {'v',{0,0,17,17,17,10,4}},
 {'w',{0,0,17,17,21,21,10}},
 {'x',{0,0,17,10,4,10,17}},
 {'y',{0,0,17,17,15,1,14}},
 {'z',{0,0,31,2,4,8,31}},
};
static uint8_t *strip;
static SemaphoreHandle_t finished;
static esp_lcd_i80_bus_handle_t bus;
static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
static int top;
static uint16_t bg, card, ink, accent, muted, edge;
static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-@!?";

static const uint8_t *glyph(char c)
{
    for (unsigned i = 0; i < sizeof(FONT)/sizeof(FONT[0]); ++i)
        if (FONT[i].ch == c) return FONT[i].row;
    static const uint8_t question[7]={14,17,1,2,4,0,4};
    return question;
}
static bool transfer_done(esp_lcd_panel_io_handle_t h, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)h; (void)e;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}
static void rect(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    int xa = x < 0 ? 0 : x, xb = x+w > W ? W : x+w;
    int ya = y < top ? top : y, yb = y+h > top+STRIP ? top+STRIP : y+h;
    if (xa >= xb || ya >= yb) return;
    for (int yy=ya; yy<yb; ++yy) {
        uint8_t *p = strip + ((size_t)(yy-top)*W + xa)*2u;
        for (int xx=xa; xx<xb; ++xx) { *p++ = (uint8_t)(c >> 8); *p++=(uint8_t)c; }
    }
}
static void rounded(int x,int y,int w,int h,int radius,uint16_t c)
{
    if (radius <= 0) { rect(x,y,w,h,c); return; }
    for (int dy=0;dy<h;++dy) {
        int cut=0;
        if (dy<radius) {
            int n=radius-1-dy;
            while (cut<radius && (radius-cut-1)*(radius-cut-1)+n*n>=radius*radius) cut++;
        } else if (dy>=h-radius) {
            int n=dy-(h-radius);
            while (cut<radius && (radius-cut-1)*(radius-cut-1)+n*n>=radius*radius) cut++;
        }
        rect(x+cut,y+dy,w-2*cut,1,c);
    }
}
static void text(int x,int y,const char *s,int scale,uint16_t c)
{
    for (int index=0;s[index];index++) {
        const uint8_t *g=glyph(s[index]);
        int gx=x+index*6*scale;
        for (int row=0;row<7;++row)
            for (int col=0;col<5;++col)
                if (g[row] & (1u<<(4-col))) rect(gx+col*scale,y+row*scale,scale,scale,c);
    }
}
static int text_w(const char *s,int scale) { return (int)strlen(s)*6*scale; }
static void center(int y,const char *s,int scale,uint16_t c)
{ text((W-text_w(s,scale))/2,y,s,scale,c); }
static void button(int x,int y,int w,int h,const char *label,bool selected)
{
    rounded(x,y,w,h,8,selected?accent:card);
    int scale = text_w(label,2) <= w-6 ? 2:1;
    text(x+(w-text_w(label,scale))/2,y+(h-7*scale)/2,label,scale,selected?bg:ink);
}
static void image_preview(int x,int y,int w,int h)
{
    if(!media_has_bitmap())return;
    const uint16_t *pixels=media_bitmap();
    int from=top>y?top-y:0;
    int to=top+STRIP-y;
    if(to>h)to=h;
    for(int dy=from;dy<to;dy++){
        int sy=dy*MEDIA_BMP_HEIGHT/h;
        for(int dx=0;dx<w;dx++){
            int sx=dx*MEDIA_BMP_WIDTH/w;
            rect(x+dx,y+dy,1,1,pixels[sy*MEDIA_BMP_WIDTH+sx]);
        }
    }
}
static void palette(int theme)
{
    /* RGB565; disable ST7789 colour inversion at panel initialization.
     * SLATE, MOSS, PAPER and genuine BLACK offer calm high-contrast ink. */
    switch(theme){
      case 1: bg=0x10E3;card=0x21C6;ink=0xEF9D;accent=0xB695;muted=0xA5B4;edge=0x4B2A;break;
      case 2: bg=0xF79E;card=0xDF1C;ink=0x1925;accent=0x1AAD;muted=0x42CC;edge=0xB618;break;
      case 3: bg=0x0000;card=0x1925;ink=0xF79D;accent=0xC6BC;muted=0xADD8;edge=0x3A29;break;
      default:bg=0x10C4;card=0x21A8;ink=0xEF9E;accent=0x9E59;muted=0xA5B7;edge=0x430D;break;
    }
}
static void header(const char *label)
{
    text(12,12,label,2,ink);
    button(258,3,57,36,"BACK",false);
    rect(9,43,302,1,edge);
}
/* Tall typographic digits, with four-pixel visual separation.
 * Two heights keep the alarm's small LOCAL row clear of its large digits. */
#define LARGE_GAP 3
#define LARGE_HOME_H 114
#define LARGE_ALARM_H 94
static const uint8_t digit_min_x[10]={5,8,5,6,4,6,5,6,5,5};
static const uint8_t digit_max_x[10]={42,40,40,41,43,41,42,41,42,42};
static int large_width(char c,int digit_advance)
{
    if(c=='.')return 15;
    if(c=='%')return 26;
    return big_index(c)>=0?digit_advance:8;
}
static void huge(int x,int y,const char *value,uint16_t color,
                 int height,int digit_advance)
{
    int cursor=x;
    for(const char *p=value;*p;p++){
        if(*p=='.'){
            rounded(cursor+4,y+height-14,7,7,3,color);
        }else if(*p=='%'){
            text(cursor+4,y+height-31,"%",3,color);
        }else{
            int index=big_index(*p);
            if(index>=0){
                int w=digit_advance-1;
                int from=top-y;
                if(from<0)from=0;
                int to=top+STRIP-y;
                if(to>height)to=height;
                for(int dy=from;dy<to;++dy){
                    int source_y=dy*BIG_FONT_H/height;
                    for(int dx=0;dx<w;++dx){
                        int source_x=digit_min_x[index]+
                            dx*(digit_max_x[index]-digit_min_x[index])/w;
                        if(big_pixel(index,source_x,source_y))
                            rect(cursor+dx,y+dy,1,1,color);
                    }
                }
            }
        }
        cursor+=large_width(*p,digit_advance);
        if(p[1])cursor+=LARGE_GAP;
    }
}
static void huge_center(int y,const char *value,uint16_t color,
                        int height,int digit_advance)
{
    int width=0;
    for(const char *p=value;*p;p++){
        width+=large_width(*p,digit_advance);
        if(p[1])width+=LARGE_GAP;
    }
    huge((W-width)/2,y,value,color,height,digit_advance);
}
/* A small bell in the otherwise unused top strip, drawn without requiring
 * Unicode glyphs that the 5x7 bitmap font does not support. */
static void bell(int x,int y,uint16_t color)
{
    rect(x+6,y,2,2,color);
    rounded(x+2,y+3,10,11,5,color);
    rect(x,y+13,14,2,color);
    rect(x+5,y+16,4,2,color);
}
static int day_bp(const struct tm *t)
{
    int secs=t->tm_hour*3600+t->tm_min*60+t->tm_sec;
    return (int)((int64_t)secs*10000/86400);
}
static void fmt_day(char *out,size_t n,int bp)
{
    if(bp<0)bp=0;
    if(bp>9999)bp=9999;
    snprintf(out,n,"%02d.%02d%%",bp/100,bp%100);
}
static void fmt_duration(char *out,size_t n,int bp)
{
    if(bp<0)bp=0;
    if(bp>999)bp=999;
    snprintf(out,n,"%d.%02d%%",bp/100,bp%100);
}
static void home(const ui_state_t *s)
{
    text(12,14,s->wifi_connected?"NTP / SYNCING":
                    s->ntp_synced?"NTP / SYNCED":
                    s->rtc_present?"RTC / OFFLINE":"LOCAL / OFFLINE",1,muted);
    button(255,5,60,34,"MENU",false);
    if(s->alarm_enabled){
        char alarm_label[12];
        fmt_day(alarm_label,sizeof alarm_label,s->alarm_bp);
        bell(115,11,accent);
        text(137,13,alarm_label,2,ink);
    }
    char label[48];
    if(s->time_valid){
        fmt_day(label,sizeof label,day_bp(&s->local));
        huge_center(53,label,accent,LARGE_HOME_H,61);
        strftime(label,sizeof label,"%Y-%m-%d %H:%M %A",&s->local);
        center(195,label,2,ink);
        rounded(10,225,300,11,4,card);
        int filled=day_bp(&s->local)*300/10000;
        if(filled>0)rounded(10,225,filled,11,4,accent);
    } else {
        center(86,"SET TIME / CONNECT WIFI",2,accent);
        center(149,"--:--",2,ink);
    }
    if(s->alarm_ringing||s->timer_ringing){
        rounded(6,7,240,32,6,accent);
        text(15,18,"TAP TO SILENCE",2,bg);
    }
}
static void menu(void)
{
    header("MENU");
    button(10,54,95,55,"ALARM",false);
    button(112,54,95,55,"TIMER",false);
    button(214,54,95,55,"SET",false);
    button(10,119,95,55,"WIFI",false);
    button(112,119,95,55,"CONTACT",false);
    button(214,119,95,55,"LOOK",false);
    button(96,188,128,46,"FILES",false);
}
static void alarm_page(const ui_state_t *s)
{
    header("ALARM");
    button(142,5,103,34,"SOUND",false);
    char value[20],note[40];
    fmt_day(value,sizeof value,s->alarm_bp);
    huge_center(60,value,accent,LARGE_ALARM_H,58);
    int sec=(s->alarm_bp*86400+5000)/10000;
    snprintf(note,sizeof note,"%02d:%02d LOCAL",sec/3600%24,sec/60%60);
    center(164,note,2,muted);
    button(5,181,73,24,"-1.00",false);
    button(84,181,73,24,"-0.01",false);
    button(163,181,73,24,"+0.01",false);
    button(242,181,73,24,"+1.00",false);
    button(18,208,284,29,s->alarm_enabled?"ALARM ON":"ALARM OFF",s->alarm_enabled);
}
static void timer_page(const ui_state_t *s)
{
    header("TIMER");
    button(142,5,103,34,"SOUND",false);
    int sec=s->timer_running?s->timer_seconds_left:
            (int)((s->timer_duration_ms+500u)/1000u);
    if(sec<0)sec=0;
    int bp=s->timer_running?(int)(((int64_t)sec*10000+43200)/86400):s->timer_bp;
    char value[20];
    fmt_duration(value,sizeof value,bp);
    huge_center(58,value,accent,110,60);
    button(5,181,73,24,"-0.10",false);
    button(84,181,73,24,"-0.01",false);
    button(163,181,73,24,"+0.01",false);
    button(242,181,73,24,"+0.10",false);
    button(17,208,93,29,"RESET",false);
    button(119,208,184,29,s->timer_running?"STOP TIMER":"START TIMER",s->timer_running);
}
static void edit_time(const ui_state_t *s)
{
    header("SET TIME");
    button(16,50,185,30,s->use_ntp?"[X] USE NTP":"[ ] USE NTP",s->use_ntp);
    if(s->use_ntp){
        center(103,"TIME FROM INTERNET",2,accent);
        const char *status=s->wifi_connected?"CONNECTED - WAITING FOR NTP":
                           s->wifi_connecting?"CONNECTING TO WIFI":
                           s->ntp_synced?"NTP SYNCED / RADIO OFF":
                           !s->ssid[0]?"SET WIFI SSID FIRST":
                           s->wifi_disconnect_reason==WIFI_REASON_NO_AP_FOUND?
                             "WIFI SSID NOT FOUND":
                           s->wifi_disconnect_reason==WIFI_REASON_AUTH_FAIL ||
                           s->wifi_disconnect_reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT?
                             "CHECK WIFI PASSWORD":"SYNC PENDING / OFFLINE";
        center(141,status,1,s->ntp_synced?accent:muted);
        button(83,183,154,43,"SYNC NOW",false);
        center(231,"TOGGLE NTP FOR MANUAL SET",1,muted);
        return;
    }
    static const char *labels[]={"YEAR","MONTH","DAY","HOUR","MIN","SEC"};
    int vals[]={s->year%100,s->month,s->day,s->hour,s->minute,s->second};
    char b[8];
    for(int i=0;i<6;++i){
        /* Three 94px cells inset 20px from each edge, like the keyboard.
         * Even the far-right + button leaves a 23px bezel margin. */
        int row=i/3,col=i%3,x=20+col*94,y=89+row*58;
        text(x+5,y,labels[i],1,muted);
        snprintf(b,sizeof b,"%02d",vals[i]);
        text(x+5,y+17,b,2,ink);
        button(x+41,y+17,22,28,"-",false);
        button(x+67,y+17,22,28,"+",false);
    }
    button(20,201,188,36,"SAVE TIME",true);
}
static void wifi_page(const ui_state_t *s)
{
    header("WIFI");
    const char *status=s->portal_enabled?"SETUP AP ACTIVE":
                       s->wifi_connected?"CONNECTED":
                       s->wifi_scanning?"SCANNING":
                       s->wifi_disconnect_reason==WIFI_REASON_NO_AP_FOUND?
                         "SSID NOT FOUND - SCAN":
                       s->wifi_disconnect_reason==WIFI_REASON_AUTH_FAIL ||
                       s->wifi_disconnect_reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT?
                         "CHECK WIFI PASSWORD":
                       s->wifi_connecting?"CONNECTING":
                       s->ntp_synced?"TIME SYNCED / WIFI OFF":"WIFI OFF";
    center(51,status,2,(s->wifi_connected||s->ntp_synced||s->portal_enabled)?
           accent:muted);
    char text_buf[55];
    snprintf(text_buf,sizeof text_buf,"SSID: %.32s",s->ssid[0]?s->ssid:"(NOT SET)");
    center(78,text_buf,1,ink);
    button(18,106,84,38,"SCAN",false);
    button(110,106,94,38,"EDIT SSID",false);
    button(212,106,90,38,"EDIT PASS",false);
    button(49,156,222,37,s->portal_enabled?"RESTART WIFI PORTAL":"START WIFI PORTAL",s->portal_enabled);
    if(s->portal_enabled){
        center(197,s->portal_ssid,1,accent);
        snprintf(text_buf,sizeof text_buf,"PASSWORD: %s",s->portal_password);
        center(209,text_buf,1,ink);
        snprintf(text_buf,sizeof text_buf,"192.168.4.1   %u:%02u LEFT",
                 (unsigned)(s->portal_seconds_left/60),
                 (unsigned)(s->portal_seconds_left%60));
        center(224,text_buf,1,ink);
    }
}
static void wifi_scan_page(const ui_state_t *s)
{
    header("SCAN WIFI");
    if(s->wifi_scanning){
        center(99,"SCANNING 2.4 GHZ...",2,accent);
        center(142,"PLEASE WAIT",2,muted);
    }else if(!s->wifi_scan_count){
        center(99,"NO NETWORK FOUND",2,muted);
    }else{
        for(int i=0;i<4;++i){
            int index=s->wifi_scan_page*4+i;
            if(index>=s->wifi_scan_count)break;
            /* Exact SSID casing matters. */
            button(18,51+i*39,284,35,s->wifi_scan_ssids[index],false);
        }
    }
    button(17,210,92,27,"RESCAN",false);
    button(115,210,89,27,"PREV",false);
    button(211,210,91,27,"NEXT",false);
}
static void keyboard_page(const ui_state_t *s)
{
    /* More square keys with bezel margins and visible input/count. */
    text(11,4,s->editing_password?"PASSWORD":"WIFI SSID",1,muted);
    char counter[24];
    size_t len=strlen(s->keyboard_value);
    snprintf(counter,sizeof counter,"%u/%u",(unsigned)len,
                 s->editing_password?64u:32u);
    text(259,4,counter,1,accent);
    if(s->keyboard_error)text(111,4,"INVALID WIFI KEY",1,accent);
    char preview[48]={0};
    snprintf(preview,sizeof preview,"%.46s",
             s->keyboard_value+(len>46?len-46:0));
    rounded(14,19,292,30,4,card);
    text(19,29,preview,1,ink);
    for(int i=0;i<42;i++){
        int col=i%7,row=i/7,x=21+col*40,y=51+row*27;
        char letter[2]={keys[i],0};
        if(s->keyboard_lower && isalpha((unsigned char)letter[0]))
            letter[0]=(char)tolower((unsigned char)letter[0]);
        button(x,y,38,25,letter,false);
    }
    button(22,214,84,25,"DELETE",false);
    button(117,214,86,25,s->keyboard_lower?"LOWER":"UPPER",false);
    button(214,214,84,25,"SAVE",true);
}
/* The photo starts at x=168. Contact text may continue over it instead
 * of truncating an email address at 24 characters. Add a thin black outline
 * to photo-overlapping text when at least four glyphs extend over the photo. */
static void contact_value(const char *value,int y)
{
    const char *v=value[0]?value:"NOT CONFIGURED";
    size_t n=strlen(v);
    char chunk[52];
    size_t left=n<26?n:26; /* 26 glyphs × 6px = 156px from x=12 */
    memcpy(chunk,v,left);
    chunk[left]=0;
    text(12,y,chunk,1,ink);
    if(n>26){
        size_t over=n-26;
        size_t take=over<25?over:25; /* Remaining width to right edge */
        memcpy(chunk,v+26,take);
        chunk[take]=0;
        if(over>3){
            /* Black 1px border around the glyphs, retaining visible photo. */
            for(int dy=-1;dy<=1;++dy)
                for(int dx=-1;dx<=1;++dx)
                    if(dx||dy)text(168+dx,y+dy,chunk,1,0x0000);
            text(168,y,chunk,1,0xFFFF);
        }else text(168,y,chunk,1,ink);
        /* Maximum address field is 64 chars; wrap any remainder into
         * the unused 11px line gap rather than clipping at right bezel. */
        if(n>51)text(12,y+11,v+51,1,ink);
    }
}
static void contact_page(const ui_state_t *s)
{
    header("CONTACT");
    /* Right margin 12px, bottom margin 10px. Draw image FIRST, so longer
     * contact fields remain visible when they pass across its left edge. */
    if(media_has_bitmap())image_preview(168,50,140,180);
    text(12,54,"100% CLOCK MADE BY",1,muted);
    text(12,80,"XMPP",2,accent);
    contact_value(s->xmpp,103);
    text(12,128,"EMAIL",2,accent);
    contact_value(s->email,151);
    text(12,176,"SMS",2,accent);
    contact_value(s->sms,199);
    text(12,208,audio_builtin_name(s->contact_tune),1,muted);
    button(12,218,66,21,s->audio_playing?"STOP":"PLAY",false);
    button(84,218,68,21,"NEXT",false);
}
static void sound_page(const ui_state_t *s)
{
    header(s->sound_target==2?"TIMER SOUND":"ALARM SOUND");
    const char *selected=s->sound_target==2?s->timer_sound:s->alarm_sound;
    char key[16];
    /* Five tracks fit without colliding with TF AUDIO/BACK at the bottom. */
    for(int i=0;i<AUDIO_BUILTIN_COUNT;++i){
        audio_builtin_key(i,key,sizeof key);
        button(12,47+i*30,296,28,audio_builtin_name(i),
               !strcmp(selected,key));
    }
    button(12,204,142,32,"TF AUDIO",false);
    button(167,204,141,32,"BACK",false);
}
static void look_page(const ui_state_t *s)
{
    header("LOOK");
    const char *names[]={"SLATE","MOSS","PAPER","BLACK"};
    center(59,names[s->theme%4],2,muted);
    button(18,89,138,45,"NEXT COLOR",false);
    button(167,89,135,45,"DARK",s->theme==3);
    char label[36];snprintf(label,sizeof label,"BRIGHTNESS: %u%%",s->brightness);
    center(152,label,2,ink);
    button(43,181,94,41,"DIM",false);
    button(183,181,94,41,"BRIGHT",false);
    text(12,228,"DIM AFTER 40 SECONDS IDLE",1,muted);
}
static void files_page(const ui_state_t *s)
{
    header("TF FILES");
    button(141,4,104,35,"RESCAN",false);
    if(!s->sd_ready){
        center(84,"NO TF CARD MOUNTED",2,muted);
        center(116,media_sd_status(),1,ink);
        center(147,"INSERT CARD / PRESS RESCAN",1,muted);
        button(76,184,168,43,"RETRY SD",false);
        return;
    }
    char msg[60];
    snprintf(msg,sizeof msg,"FILE %d / %d",media_count()?media_index()+1:0,media_count());
    center(55,msg,2,muted);
    center(78,media_sd_status(),1,muted);
    center(108,s->file_name[0]?s->file_name:"(NO SUPPORTED FILES)",1,ink);
    button(13,166,90,44,"PREV",false);
    button(114,166,90,44,"NEXT",false);
    button(215,166,91,44,"OPEN",true);
    center(216,"TXT / BMP / WAV / MP3 / RTTTL",1,muted);
}
static void file_view(const ui_state_t *s)
{
    header("FILE VIEW");
    text(9,59,s->file_name,1,accent);
    if(audio_file_supported(s->file_name)){
        center(101,"TF AUDIO",2,ink);
        center(131,"WAV / MP3 / RTTTL",1,muted);
        button(11,174,96,39,s->audio_playing?"STOP":"PLAY",false);
        button(112,174,96,39,"ALARM",false);
        button(213,174,96,39,"TIMER",false);
        center(222,"SELECT AS ALARM OR TIMER SOUND",1,muted);
        return;
    }
    if(media_has_bitmap()){
        image_preview(112,82,96,96);
        center(202,"BMP IMAGE",1,muted);
        return;
    }
    const char *p=s->file_preview;
    int row=0;
    while(*p && row<8){
        char line[51]={0};int i=0;
        while(*p && *p!='\n' && i<50)line[i++]=*p++;
        if(*p=='\n')p++;
        text(9,83+row*17,line,1,ink);
        row++;
    }
}
void screen_draw(const ui_state_t *s)
{
    if(!s||!strip)return;
    palette(s->theme);
    if(s->alert_flash){
        /* Invert RGB565 software colors, not the ST7789 inversion mode.
         * Every theme, including actual black, returns to its exact
         * original palette at the end of the brief alert pulse. */
        bg=(uint16_t)~bg;
        card=(uint16_t)~card;
        ink=(uint16_t)~ink;
        accent=(uint16_t)~accent;
        muted=(uint16_t)~muted;
        edge=(uint16_t)~edge;
    }
    for(top=0;top<H;top+=STRIP){
        rect(0,top,W,STRIP,bg);
        switch(s->page){
          case UI_HOME:home(s);break;
          case UI_MENU:menu();break;
          case UI_ALARM:alarm_page(s);break;
          case UI_TIMER:timer_page(s);break;
          case UI_SET_TIME:edit_time(s);break;
          case UI_WIFI:wifi_page(s);break;
          case UI_WIFI_SCAN:wifi_scan_page(s);break;
          case UI_CONTACT:contact_page(s);break;
          case UI_LOOK:look_page(s);break;
          case UI_KEYBOARD:keyboard_page(s);break;
          case UI_FILES:files_page(s);break;
          case UI_FILE_VIEW:file_view(s);break;
          case UI_SOUND_PICK:sound_page(s);break;
        }
        /* A short, unmistakable tap marker shows the coordinates accepted
           by the touch driver, even when no button action was matched.
           This is particularly useful for diagnosing mirrored axes. */
        if(s->touch_feedback){
            int x=s->touch_x,y=s->touch_y;
            rounded(x-9,y-9,19,19,9,ink);
            rounded(x-6,y-6,13,13,6,bg);
            rounded(x-3,y-3,7,7,3,accent);
        }
        esp_err_t e=esp_lcd_panel_draw_bitmap(panel,0,top,W,top+STRIP,strip);
        if(e!=ESP_OK||xSemaphoreTake(finished,pdMS_TO_TICKS(700))!=pdTRUE){
           ESP_LOGE("clock_screen","LCD stripe failed: %s",esp_err_to_name(e));
           break;
        }
    }
}
void screen_set_brightness(uint8_t brightness)
{
    if(brightness<3) brightness=3;
    if(brightness>100) brightness=100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,brightness*1023/100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
}
esp_err_t screen_init(void)
{
    gpio_config_t rd={.pin_bit_mask=1ULL<<2,.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rd));
    gpio_set_level(GPIO_NUM_2,1);
    const ledc_timer_config_t timer={
        .speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_0,
        .duty_resolution=LEDC_TIMER_10_BIT,.freq_hz=4000,.clk_cfg=LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t channel={
        .gpio_num=0,.speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_0,.intr_type=LEDC_INTR_DISABLE,
        .timer_sel=LEDC_TIMER_0,.duty=0,.hpoint=0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    finished=xSemaphoreCreateBinary();
    if(!finished)return ESP_ERR_NO_MEM;
    const esp_lcd_i80_bus_config_t bc={
        .clk_src=LCD_CLK_SRC_DEFAULT,.dc_gpio_num=GPIO_NUM_16,
        .wr_gpio_num=GPIO_NUM_4,
        .data_gpio_nums={15,13,12,14,27,25,33,32},.bus_width=8,
        .max_transfer_bytes=W*STRIP*2
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bc,&bus));
    const esp_lcd_panel_io_i80_config_t ic={
        .cs_gpio_num=GPIO_NUM_17,.pclk_hz=10000000,
        .trans_queue_depth=2,.lcd_cmd_bits=8,.lcd_param_bits=8,
        .dc_levels={.dc_idle_level=0,.dc_cmd_level=0,.dc_dummy_level=0,.dc_data_level=1}
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus,&ic,&io));
    esp_lcd_panel_io_callbacks_t callbacks={.on_color_trans_done=transfer_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io,&callbacks,finished));
    const esp_lcd_panel_dev_config_t dc={
        .reset_gpio_num=-1,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR,.bits_per_pixel=16
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io,&dc,&panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel,true));
    /* Button/TF-up installation: rotate the already landscape LCD 180°.
     * This inverts BOTH mirror flags. Touch axes are flipped below too. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel,
                CONFIG_CLOCK_LCD_ROTATE_CW ^ CONFIG_CLOCK_DISPLAY_FLIP_180,
                CONFIG_CLOCK_LCD_MIRROR_Y ^ CONFIG_CLOCK_DISPLAY_FLIP_180));
    /* Inversion made the BLACK theme physically white on this panel. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
    strip=heap_caps_malloc(W*STRIP*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!strip)return ESP_ERR_NO_MEM;
    big_font_init();
    screen_set_brightness(30);
    ESP_LOGI("clock_screen","320x240 landscape ST7789 i80 initialized");
    return ESP_OK;
}
bool screen_poll_touch(int *x,int *y,ui_page_t page)
{
    /* CST816S reports its native 240x320 sensor coordinates. Portrait data
       becomes landscape x=raw_y, y=239-raw_x with this board's LCD orientation.
       Unlike the original edge-only implementation, a missed release no
       longer disables all subsequent touches. The 350ms limit also allows
       deliberate press-and-hold on +/- without runaway repeats. */
    static int last_x=-1,last_y=-1;
    static int last_alarm_key=-1;
    static int64_t held_since_us=0;
    static int64_t last_trigger_us=0;
    uint8_t reg=0x02,d[5];
    esp_err_t err=i2c_master_write_read_device(I2C_PORT,TOUCH_ADDR,
                       &reg,1,d,5,pdMS_TO_TICKS(22));
    if(err!=ESP_OK || (d[0]&0x0F)==0) {
        last_x=-1;last_y=-1;
        last_alarm_key=-1;
        held_since_us=0;
        return false;
    }
    int raw_x=((d[1]&0x0f)<<8)|d[2];
    int raw_y=((d[3]&0x0f)<<8)|d[4];
    /* The board's CST816S axes run opposite the displayed landscape
       orientation after swapping ST7789 XY. Correct both axes here,
       independently of the LCD mirror configuration. */
    int xx=CONFIG_CLOCK_LCD_ROTATE_CW?raw_y:319-raw_y;
    int yy=CONFIG_CLOCK_LCD_ROTATE_CW?239-raw_x:raw_x;
    if(CONFIG_CLOCK_TOUCH_SWAP_XY){int tmp=xx;xx=yy;yy=tmp;}
    if(CONFIG_CLOCK_TOUCH_MIRROR_X)xx=W-1-xx;
    if(CONFIG_CLOCK_TOUCH_MIRROR_Y)yy=H-1-yy;
    /* Match the physical 180-degree LCD rotation before edge calibration. */
    if(CONFIG_CLOCK_DISPLAY_FLIP_180){
        xx=W-1-xx;
        yy=H-1-yy;
    }
    /* Compensate for capacitive active area falling short of LCD edges.
       Exaggerate offsets from center and saturate at screen boundaries;
       this retains central precision and keeps existing orientation. */
    xx=W/2+(xx-W/2)*CONFIG_CLOCK_TOUCH_EDGE_GAIN/100;
    yy=H/2+(yy-H/2)*CONFIG_CLOCK_TOUCH_EDGE_GAIN/100;
    if(xx<0)xx=0;
    if(xx>=W)xx=W-1;
    if(yy<0)yy=0;
    if(yy>=H)yy=H-1;
    const int64_t now=esp_timer_get_time();
    int dx=xx-last_x,dy=yy-last_y;
    if(page==UI_ALARM && yy>=180 && yy<207){
        int key=xx<80?0:xx<160?1:xx<240?2:3;
        if(key!=last_alarm_key || !held_since_us){
            last_alarm_key=key;
            held_since_us=now;
            last_trigger_us=0;
        }
        int64_t held=now-held_since_us;
        /* Quick on hold; normal discrete taps still change one increment. */
        int64_t gap=held>=2300000?65000:held>=700000?110000:350000;
        if(last_trigger_us && now-last_trigger_us<gap)return false;
    }else{
        last_alarm_key=-1;
        held_since_us=0;
        if(last_x>=0 && dx*dx+dy*dy<1600 &&
           now-last_trigger_us<350000)return false;
    }
    last_x=xx;last_y=yy;last_trigger_us=now;
    *x=xx;*y=yy;
    return true;
}
