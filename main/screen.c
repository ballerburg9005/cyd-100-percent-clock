/* Native graphics: no LVGL, framebuffer or PSRAM required. */
#include "screen.h"
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

#define W 240
#define H 320
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
static void palette(int theme)
{
    /* Muted rose / glacial teal / honey. Contrast also works in low brightness. */
    if (theme==1) {
        bg=0x084F; card=0x122F; ink=0xEFFF; accent=0x85FF; muted=0x8CB4; edge=0x2B53;
    } else if (theme==2) {
        bg=0x1806; card=0x310B; ink=0xFFF1; accent=0xFE82; muted=0xBD2C; edge=0x5A4B;
    } else {
        bg=0x1022; card=0x2846; ink=0xFF9B; accent=0xFAED; muted=0xA48E; edge=0x408A;
    }
}
static void header(const char *label)
{
    text(13,17,label,2,ink);
    rounded(169,10,59,32,7,card);
    text(176,21,"BACK",1,ink);
    rect(14,52,212,1,edge);
}
static void segment(int x,int y,int w,int h,uint16_t c)
{ rounded(x,y,w,h,3,c); }
static void numeral(int x,int y,int d,uint16_t color)
{
    static const uint8_t masks[10]={63,6,91,79,102,109,125,7,127,111};
    const uint8_t m=masks[d%10];
    const int w=42,h=76,t=7;
    if (m&1) segment(x+t,y,w-2*t,t,color);                        // a
    if (m&2) segment(x+w-t,y+t,t,h/2-t,color);                    // b
    if (m&4) segment(x+w-t,y+h/2,t,h/2-t,color);                  // c
    if (m&8) segment(x+t,y+h-t,w-2*t,t,color);                    // d
    if (m&16) segment(x,y+h/2,t,h/2-t,color);                     // e
    if (m&32) segment(x,y+t,t,h/2-t,color);                       // f
    if (m&64) segment(x+t,y+h/2-t/2,w-2*t,t,color);               // g
}
static void percent_digits(int bp)
{
    /* 4 large digits, a decimal point and the % sign; width 225 px. */
    int x=8,y=98;
    int digits[]={bp/1000,(bp/100)%10,(bp/10)%10,bp%10};
    for (int i=0;i<4;++i) {
        numeral(x,y,digits[i],accent); x+=45;
        if(i==1) { rounded(x+1,y+66,7,7,3,accent); x+=13; }
    }
    rounded(x+2,y+15,7,7,3,accent);
    rounded(x+17,y+53,7,7,3,accent);
    for(int a=0;a<42;++a) rect(x+20-a/3,y+22+a,3,3,accent);
}
static void home(const ui_state_t *s)
{
    center(25,"YOUR DAY, IN PERCENT",1,muted);
    center(51,"ONE HUNDRED",2,ink);
    rect(22,83,196,1,edge);
    if (s->time_valid) {
        int secs=s->local.tm_hour*3600+s->local.tm_min*60+s->local.tm_sec;
        int basis=secs*10000/86400;
        percent_digits(basis);
    } else center(123,"SET THE TIME",2,accent);
    char label[64];
    if (s->time_valid) {
        snprintf(label,sizeof label,"%02d:%02d:%02d",s->local.tm_hour,s->local.tm_min,s->local.tm_sec);
        center(197,label,3,ink);
        strftime(label,sizeof label,"%a %d %b %Y",&s->local);
        center(236,label,2,muted);
    } else center(197,"--:--:--",3,ink);
    rounded(20,267,200,7,3,card);
    if (s->time_valid) {
        int secs=s->local.tm_hour*3600+s->local.tm_min*60+s->local.tm_sec;
        int pixels=secs*200/86400;
        if (pixels>0) rounded(20,267,pixels,7,3,accent);
    }
    if (s->alarm_ringing || s->timer_ringing) center(285,"TAP TO SILENCE",2,accent);
    else button(77,283,86,33,"MENU",false);
    text(10,307,s->rtc_present?"RTC":"SYS",1,muted);
    text(182,307,s->wifi_connected?"WIFI":"OFFLINE",1,muted);
}
static void menu(void)
{
    header("MENU");
    button(12,69,104,53,"ALARM",false);button(124,69,104,53,"TIMER",false);
    button(12,132,104,53,"SET TIME",false);button(124,132,104,53,"WIFI",false);
    button(12,195,104,53,"GIFT",false);button(124,195,104,53,"LOOK",false);
    center(277,"MAKE TODAY COUNT",1,muted);
}
static void alarm_page(const ui_state_t *s)
{
    header("ALARM");
    center(77,"DAILY ALARM",2,muted);
    char b[20];
    snprintf(b,sizeof b,"%02u:%02u",s->alarm_hour,s->alarm_minute);
    center(111,b,4,ink);
    button(18,163,48,42,"H-",false);button(71,163,48,42,"H+",false);
    button(124,163,48,42,"M-",false);button(177,163,48,42,"M+",false);
    button(20,229,200,48,s->alarm_enabled?"ALARM ON":"ALARM OFF",s->alarm_enabled);
    center(288,"TAP TIME TO ADJUST",1,muted);
}
static void timer_page(const ui_state_t *s)
{
    header("TIMER");
    center(76,"KITCHEN TIMER",2,muted);
    int left=s->timer_running ? s->timer_seconds_left : s->timer_minutes*60;
    if (left<0) left=0;
    char b[24];snprintf(b,sizeof b,"%02d:%02d",left/60,left%60);
    center(114,b,4,ink);
    button(16,174,48,40,"-5",false);button(69,174,48,40,"+5",false);
    button(124,174,48,40,"-1",false);button(177,174,48,40,"+1",false);
    button(19,238,202,47,s->timer_running?"STOP":"START",s->timer_running);
}
static void edit_time(const ui_state_t *s)
{
    header("SET TIME");
    static const char *labels[]={"YEAR","MONTH","DAY","HOUR","MIN","SEC"};
    int vals[]={s->year,s->month,s->day,s->hour,s->minute,s->second};
    char b[20];
    for(int i=0;i<6;++i){
        int row=i/3,col=i%3,x=8+col*76,y=67+row*97;
        text(x+4,y,labels[i],1,muted);
        snprintf(b,sizeof b,i==0?"%04d":"%02d",vals[i]);
        text(x+4,y+19,b,2,ink);
        button(x+2,y+46,31,30,"-",false);
        button(x+37,y+46,31,30,"+",false);
    }
    button(20,277,200,36,"SAVE TIME",true);
}
static void wifi_page(const ui_state_t *s)
{
    header("WIFI");
    center(64,s->wifi_connected?"ONLINE":"NOT CONNECTED",2,s->wifi_connected?accent:muted);
    char b[34];
    snprintf(b,sizeof b,"SSID: %.19s",s->ssid[0]?s->ssid:"(NOT SET)");
    center(94,b,1,ink);
    button(15,125,100,44,"SSID",false);button(125,125,100,44,"PASSWORD",false);
    button(15,184,210,43,s->portal_enabled?"PORTAL ON":"START PORTAL",s->portal_enabled);
    if (s->portal_enabled) {
        center(236,"CONNECT TO:",1,muted);
        center(251,s->portal_ssid,1,ink);
        snprintf(b,sizeof b,"PASS: %s",s->portal_password);
        center(267,b,1,ink);
        center(286,"192.168.4.1",1,accent);
    } else center(244,"TOUCH EDIT OR USE WEB SETUP",1,muted);
}
static void keyboard_page(const ui_state_t *s)
{
    header(s->editing_password?"PASSWORD":"WIFI SSID");
    char preview[24];
    size_t len=strlen(s->keyboard_value);
    if(s->editing_password) {
        size_t count=len>19?19:len;
        memset(preview,'*',count);preview[count]=0;
    } else snprintf(preview,sizeof preview,"%.19s",s->keyboard_value+(len>19?len-19:0));
    rounded(9,52,222,31,5,card);
    text(15,62,preview,2,ink);
    for(int i=0;i<42;++i){
        int col=i%6,row=i/6,x=10+col*37,y=88+row*26;
        char letter[2]={keys[i],0};
        button(x,y,34,24,letter,false);
    }
    button(8,275,75,37,s->keyboard_lower?"LOWER":"UPPER",false);
    button(87,275,68,37,"DEL",false);
    button(159,275,73,37,"DONE",true);
}
static void gift_page(const ui_state_t *s)
{
    header("A LITTLE GIFT");
    center(81,"JUST FOR YOU",2,accent);
    rounded(20,126,200,111,10,card);
    center(143,"A WHOLE DAY",2,ink);
    center(166,"IS YOURS",2,ink);
    char b[40];snprintf(b,sizeof b,"%.28s",s->gift[0]?s->gift:"MAKE IT BEAUTIFUL");
    center(207,b,1,muted);
    if(s->contact[0]){
        char contact[36];snprintf(contact,sizeof contact,"%.29s",s->contact);
        center(256,contact,1,accent);
    }else center(256,"ADD CONTACT IN WIFI PORTAL",1,muted);
    center(281,"ONE DAY AT A TIME",1,accent);
}
static void look_page(const ui_state_t *s)
{
    header("LOOK");
    center(85,"CHOOSE YOUR COLOR",2,muted);
    button(14,119,212,44,s->theme==0?"ROSE":s->theme==1?"GLACIER":"HONEY",true);
    char b[30];snprintf(b,sizeof b,"BRIGHTNESS  %u%%",s->brightness);
    center(185,b,2,ink);
    button(28,215,72,43,"-",false);button(139,215,72,43,"+",false);
    center(276,"AUTO DIMS AFTER IDLE",1,muted);
}
void screen_draw(const ui_state_t *s)
{
    if (!s || !strip) return;
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
            case UI_GIFT:gift_page(s);break;
            case UI_LOOK:look_page(s);break;
            case UI_KEYBOARD:keyboard_page(s);break;
        }
        esp_err_t e=esp_lcd_panel_draw_bitmap(panel,0,top,W,top+STRIP,strip);
        if(e!=ESP_OK || xSemaphoreTake(finished,pdMS_TO_TICKS(700))!=pdTRUE){
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
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel,false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel,
                CONFIG_CLOCK_LCD_MIRROR_X,CONFIG_CLOCK_LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
    strip=heap_caps_malloc(W*STRIP*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!strip)return ESP_ERR_NO_MEM;
    screen_set_brightness(30);
    ESP_LOGI("clock_screen","240x320 ST7789 i80 initialized");
    return ESP_OK;
}
bool screen_poll_touch(int *x,int *y)
{
    static bool pressed;
    uint8_t reg=0x02,d[5];
    const esp_err_t err=i2c_master_write_read_device(I2C_PORT,TOUCH_ADDR,&reg,1,d,5,pdMS_TO_TICKS(22));
    if(err!=ESP_OK || (d[0]&0x0F)==0) { pressed=false;return false; }
    if(pressed)return false;
    int xx=((d[1]&0x0f)<<8)|d[2], yy=((d[3]&0x0f)<<8)|d[4];
    if(CONFIG_CLOCK_TOUCH_SWAP_XY){int tmp=xx;xx=yy;yy=tmp;}
    if(CONFIG_CLOCK_TOUCH_MIRROR_X)xx=W-1-xx;
    if(CONFIG_CLOCK_TOUCH_MIRROR_Y)yy=H-1-yy;
    if(xx<0||xx>=W||yy<0||yy>=H)return false;
    pressed=true;*x=xx;*y=yy;
    return true;
}
