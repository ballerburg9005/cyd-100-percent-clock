/* Native graphics: no LVGL, framebuffer or PSRAM required. */
#include "screen.h"
#include "big_font.h"
#include "media.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define W 320
#define H 240
#define STRIP 16
#define I2C_PORT I2C_NUM_0
#define TOUCH_ADDR 0x15

/* Kconfig defaults of 'n' are omitted from sdkconfig.h. */
#ifndef CONFIG_CLOCK_LCD_MIRROR_X
#define CONFIG_CLOCK_LCD_MIRROR_X 0
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
    c = (char)toupper((unsigned char)c);
    for (unsigned i = 0; i < sizeof(FONT)/sizeof(FONT[0]); ++i)
        if (FONT[i].ch == c) return FONT[i].row;
    return FONT[sizeof(FONT)/sizeof(FONT[0]) - 1].row;
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
        int sy=dy*MEDIA_BMP_SIZE/h;
        for(int dx=0;dx<w;dx++){
            int sx=dx*MEDIA_BMP_SIZE/w;
            rect(x+dx,y+dy,1,1,pixels[sy*MEDIA_BMP_SIZE+sx]);
        }
    }
}
static void palette(int theme)
{
    /* Rose / glacial teal / honey / battery-oriented nearly-black. */
    switch(theme){
      case 1: bg=0x084F;card=0x122F;ink=0xEFFF;accent=0x85FF;muted=0x8CB4;edge=0x2B53;break;
      case 2: bg=0x1806;card=0x310B;ink=0xFFF1;accent=0xFE82;muted=0xBD2C;edge=0x5A4B;break;
      case 3: bg=0x0000;card=0x1082;ink=0xDF1B;accent=0xB36D;muted=0x7BEF;edge=0x2104;break;
      default:bg=0x1022;card=0x2846;ink=0xFF9B;accent=0xFAED;muted=0xA48E;edge=0x408A;break;
    }
}
static void header(const char *label)
{
    text(12,12,label,2,ink);
    button(258,3,57,36,"BACK",false);
    rect(9,43,302,1,edge);
}
static void huge(int x,int y,const char *s,uint16_t color)
{
    int cursor=x;
    for(int i=0;s[i];i++){
        int g=big_index(s[i]);
        if(g<0){cursor+=14;continue;}
        int width=big_width[g];
        if(y+BIG_FONT_H>top && y<top+STRIP){
            int from=top-y;
            if(from<0)from=0;
            int to=top+STRIP-y;
            if(to>BIG_FONT_H)to=BIG_FONT_H;
            for(int dy=from;dy<to;dy++)
                for(int dx=0;dx<width;dx++)
                    if(big_pixel(g,dx,dy))rect(cursor+dx,y+dy,1,1,color);
        }
        cursor+=width+1;
    }
}
static void huge_center(int y,const char *s,uint16_t color)
{
    int width=0;
    for(int i=0;s[i];++i){
        int g=big_index(s[i]);
        width+=g<0?14:big_width[g]+1;
    }
    huge((W-width)/2,y,s,color);
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
    text(12,14,s->wifi_connected?"NTP / ONLINE":s->rtc_present?"RTC / OFFLINE":"LOCAL / OFFLINE",1,muted);
    button(255,5,60,34,"MENU",false);
    char label[48];
    if(s->time_valid){
        fmt_day(label,sizeof label,day_bp(&s->local));
        huge_center(65,label,accent);
        snprintf(label,sizeof label,"%02d:%02d",s->local.tm_hour,s->local.tm_min);
        center(150,label,2,ink);
        strftime(label,sizeof label,"%A",&s->local);
        center(180,label,2,muted);
        snprintf(label,sizeof label,"%04d-%02d-%02d",s->local.tm_year+1900,
                 s->local.tm_mon+1,s->local.tm_mday);
        center(200,label,2,muted);
        rounded(16,225,288,5,2,card);
        int filled=day_bp(&s->local)*288/10000;
        if(filled>0)rounded(16,225,filled,5,2,accent);
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
    button(214,54,95,55,"SET TIME",false);
    button(10,119,95,55,"WIFI",false);
    button(112,119,95,55,"CONTACT",false);
    button(214,119,95,55,"LOOK",false);
    button(96,188,128,46,"FILES",false);
}
static void alarm_page(const ui_state_t *s)
{
    header("ALARM");
    text(11,52,"DAILY ALARM IN DECIMAL TIME",1,muted);
    char value[20],note[40];
    fmt_day(value,sizeof value,s->alarm_bp);
    huge_center(68,value,accent);
    int sec=(s->alarm_bp*86400+5000)/10000;
    snprintf(note,sizeof note,"%02d:%02d LOCAL",sec/3600%24,sec/60%60);
    center(144,note,2,muted);
    button(5,167,73,35,"-1.00",false);
    button(84,167,73,35,"-0.01",false);
    button(163,167,73,35,"+0.01",false);
    button(242,167,73,35,"+1.00",false);
    button(18,208,284,29,s->alarm_enabled?"ALARM ON":"ALARM OFF",s->alarm_enabled);
}
static void timer_page(const ui_state_t *s)
{
    header("TIMER");
    text(11,52,"DURATION / PERCENT OF 24H",1,muted);
    int sec=s->timer_running?s->timer_seconds_left:
            ((int64_t)s->timer_bp*86400+5000)/10000;
    if(sec<0)sec=0;
    int bp=s->timer_running?(int)((int64_t)sec*10000/86400):s->timer_bp;
    char value[20],note[40];
    fmt_duration(value,sizeof value,bp);
    huge_center(68,value,accent);
    snprintf(note,sizeof note,"%02d:%02d REMAINING",sec/60,sec%60);
    center(144,note,2,muted);
    button(5,167,73,35,"-0.10",false);
    button(84,167,73,35,"-0.01",false);
    button(163,167,73,35,"+0.01",false);
    button(242,167,73,35,"+0.10",false);
    button(18,208,284,29,s->timer_running?"STOP TIMER":"START TIMER",s->timer_running);
}
static void edit_time(const ui_state_t *s)
{
    header("SET TIME");
    button(16,50,185,30,s->use_ntp?"[X] USE NTP":"[ ] USE NTP",s->use_ntp);
    if(s->use_ntp){
        center(110,"TIME FROM INTERNET",2,accent);
        center(149,s->wifi_connected?"CONNECTED / SYNC ON":"CONNECT WIFI FOR NTP",1,muted);
        center(184,"TAP CHECKBOX FOR MANUAL",1,muted);
        return;
    }
    static const char *labels[]={"YEAR","MONTH","DAY","HOUR","MIN","SEC"};
    int vals[]={s->year,s->month,s->day,s->hour,s->minute,s->second};
    char b[20];
    for(int i=0;i<6;++i){
        int row=i/3,col=i%3,x=12+col*102,y=87+row*61;
        text(x+3,y,labels[i],1,muted);
        snprintf(b,sizeof b,i==0?"%04d":"%02d",vals[i]);
        text(x+5,y+12,b,2,ink);
        button(x+54,y+12,21,20,"-",false);
        button(x+77,y+12,21,20,"+",false);
    }
    button(72,210,176,27,"SAVE LOCAL TIME",true);
}
static void wifi_page(const ui_state_t *s)
{
    header("WIFI");
    center(51,s->wifi_connected?"ONLINE":"NOT CONNECTED",2,s->wifi_connected?accent:muted);
    char text_buf[55];
    snprintf(text_buf,sizeof text_buf,"SSID: %.32s",s->ssid[0]?s->ssid:"(NOT SET)");
    center(78,text_buf,1,ink);
    button(18,106,133,38,"EDIT SSID",false);
    button(168,106,133,38,"EDIT PASSWORD",false);
    button(49,156,222,37,s->portal_enabled?"PORTAL ACTIVE":"START WIFI PORTAL",s->portal_enabled);
    if(s->portal_enabled){
        center(197,s->portal_ssid,1,accent);
        snprintf(text_buf,sizeof text_buf,"PASSWORD: %s",s->portal_password);
        center(209,text_buf,1,ink);
        center(224,"OPEN 192.168.4.1",1,ink);
    }
}
static void keyboard_page(const ui_state_t *s)
{
    header(s->editing_password?"WIFI PASSWORD":"WIFI SSID");
    char preview[43]={0};
    size_t len=strlen(s->keyboard_value);
    snprintf(preview,sizeof preview,"%.40s",s->keyboard_value+(len>40?len-40:0));
    rounded(10,48,300,30,4,card);
    text(15,58,preview,1,ink); /* Visible plaintext, as requested. */
    for(int i=0;i<42;i++){
        int col=i%7,row=i/7,x=5+col*45,y=82+row*22;
        char letter[2]={keys[i],0};
        button(x,y,43,20,letter,false);
    }
    button(5,214,91,25,s->keyboard_lower?"LOWER":"UPPER",false);
    button(107,214,94,25,"DELETE",false);
    button(211,214,104,25,"SAVE",true);
}
static void contact_page(const ui_state_t *s)
{
    header("CONTACT");
    text(12,53,"A MESSAGE FROM THE MAKER",1,muted);
    text(12,78,"XMPP",2,accent);
    char tmp[50];
    snprintf(tmp,sizeof tmp,"%.32s",s->xmpp[0]?s->xmpp:"NOT CONFIGURED");
    text(12,100,tmp,1,ink);
    text(12,135,"EMAIL",2,accent);
    snprintf(tmp,sizeof tmp,"%.32s",s->email[0]?s->email:"NOT CONFIGURED");
    text(12,156,tmp,1,ink);
    if(media_has_bitmap()){
        rounded(232,76,81,81,5,card);
        image_preview(237,81,71,71);
    }else text(12,192,"PHOTO: FACE.BMP ON TF CARD",1,muted);
    text(12,213,"EDIT DETAILS IN WIFI PORTAL",1,muted);
}
static void look_page(const ui_state_t *s)
{
    header("LOOK");
    center(59,"COLOR SCHEME",2,muted);
    const char *names[]={"ROSE","GLACIER","HONEY","DARK"};
    button(35,91,250,42,names[s->theme%4],true);
    char label[36];snprintf(label,sizeof label,"BRIGHTNESS: %u%%",s->brightness);
    center(152,label,2,ink);
    button(43,181,94,41,"DIM",false);
    button(183,181,94,41,"BRIGHT",false);
    text(12,228,"DIM AFTER 40 SECONDS IDLE",1,muted);
}
static void files_page(const ui_state_t *s)
{
    header("TF FILES");
    if(!s->sd_ready){
        center(97,"NO TF CARD MOUNTED",2,muted);
        center(141,"INSERT CARD / REOPEN",1,ink);
        return;
    }
    char msg[60];
    snprintf(msg,sizeof msg,"FILE %d / %d",media_count()?media_index()+1:0,media_count());
    center(54,msg,2,muted);
    center(95,s->file_name[0]?s->file_name:"(EMPTY CARD)",1,ink);
    button(13,166,90,44,"PREV",false);
    button(114,166,90,44,"NEXT",false);
    button(215,166,91,44,"OPEN",true);
    center(216,"TXT / BMP VIEW / MP3 NAME ONLY",1,muted);
}
static void file_view(const ui_state_t *s)
{
    header("FILE VIEW");
    text(9,59,s->file_name,1,accent);
    if(media_has_bitmap()){
        image_preview(112,82,96,96);
        center(202,"24-BIT BMP IMAGE",1,muted);
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
    for(top=0;top<H;top+=STRIP){
        rect(0,top,W,STRIP,bg);
        switch(s->page){
          case UI_HOME:home(s);break;
          case UI_MENU:menu();break;
          case UI_ALARM:alarm_page(s);break;
          case UI_TIMER:timer_page(s);break;
          case UI_SET_TIME:edit_time(s);break;
          case UI_WIFI:wifi_page(s);break;
          case UI_CONTACT:contact_page(s);break;
          case UI_LOOK:look_page(s);break;
          case UI_KEYBOARD:keyboard_page(s);break;
          case UI_FILES:files_page(s);break;
          case UI_FILE_VIEW:file_view(s);break;
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
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel,
                CONFIG_CLOCK_LCD_MIRROR_X,CONFIG_CLOCK_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
    strip=heap_caps_malloc(W*STRIP*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!strip)return ESP_ERR_NO_MEM;
    big_font_init();
    screen_set_brightness(30);
    ESP_LOGI("clock_screen","320x240 landscape ST7789 i80 initialized");
    return ESP_OK;
}
bool screen_poll_touch(int *x,int *y)
{
    /* CST816S reports its native 240x320 sensor coordinates. Portrait data
       becomes landscape x=raw_y, y=239-raw_x after the LCD's XY swap.
       Unlike the original edge-only implementation, a missed release no
       longer disables all subsequent touches. The 350ms limit also allows
       deliberate press-and-hold on +/- without runaway repeats. */
    static int last_x=-1,last_y=-1;
    static int64_t last_trigger_us=0;
    uint8_t reg=0x02,d[5];
    esp_err_t err=i2c_master_write_read_device(I2C_PORT,TOUCH_ADDR,
                       &reg,1,d,5,pdMS_TO_TICKS(22));
    if(err!=ESP_OK || (d[0]&0x0F)==0) {
        last_x=-1;last_y=-1;
        return false;
    }
    int raw_x=((d[1]&0x0f)<<8)|d[2];
    int raw_y=((d[3]&0x0f)<<8)|d[4];
    int xx=raw_y,yy=239-raw_x;
    if(CONFIG_CLOCK_TOUCH_SWAP_XY){int tmp=xx;xx=yy;yy=tmp;}
    if(CONFIG_CLOCK_TOUCH_MIRROR_X)xx=W-1-xx;
    if(CONFIG_CLOCK_TOUCH_MIRROR_Y)yy=H-1-yy;
    if(xx<0||xx>=W||yy<0||yy>=H)return false;
    const int64_t now=esp_timer_get_time();
    int dx=xx-last_x,dy=yy-last_y;
    if(last_x>=0 && dx*dx+dy*dy<1600 && now-last_trigger_us<350000)
        return false;
    last_x=xx;last_y=yy;last_trigger_us=now;
    *x=xx;*y=yy;
    return true;
}
