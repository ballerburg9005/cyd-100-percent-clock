/* 100% Clock for ESP32-2432S022C. Local-first and no cloud account. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "screen.h"
#include "media.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RTC_ADDR 0x68
#define PORTAL_SECONDS 600
static const char *TAG = "one_hundred";
static ui_state_t ui;
static char tz[85]="CET-1CEST,M3.5.0,M10.5.0/3";
static volatile bool wifi_connected;
static bool sntp_running;
static bool wifi_started;
static httpd_handle_t web_server;
static int64_t portal_deadline;
static int64_t timer_deadline;
static int64_t ring_deadline;
static int last_alarm_date=-1;
static int64_t last_touch_us;
static int sound_hz;
static void enable_portal(void);

static uint8_t bcd(uint8_t x) { return (x>>4)*10+(x&15); }
static uint8_t binbcd(int x) { return (uint8_t)(((x/10)<<4)|(x%10)); }

/* Portable UTC conversion: ESP-IDF's newlib does not declare timegm().
 * Civil date -> Unix days, valid for the DS3231-supported years 2000..2099.
 * Never change the process TZ: the clock concurrently displays local time. */
static time_t utc_from_tm(const struct tm *t)
{
    int y = t->tm_year + 1900;
    const int m = t->tm_mon + 1;
    const int d = t->tm_mday;
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (time_t)(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}

static void rtc_load(void)
{
    uint8_t reg=0, d[7];
    esp_err_t e=i2c_master_write_read_device(I2C_NUM_0,RTC_ADDR,&reg,1,d,7,pdMS_TO_TICKS(100));
    if(e!=ESP_OK){ui.rtc_present=false;ESP_LOGI(TAG,"No DS3231 on touch I2C bus");return;}
    ui.rtc_present=true;
    struct tm t={0};
    t.tm_sec=bcd(d[0]&0x7f);t.tm_min=bcd(d[1]&0x7f);
    if(d[2]&0x40) {
        t.tm_hour=bcd(d[2]&0x1f)%12+((d[2]&0x20)?12:0);
    } else t.tm_hour=bcd(d[2]&0x3f);
    t.tm_mday=bcd(d[4]&0x3f);
    t.tm_mon=bcd(d[5]&0x1f)-1;
    t.tm_year=bcd(d[6])+100+((d[5]&0x80)?100:0);
    t.tm_isdst=0;
    if(t.tm_year>=124 && t.tm_year<=199 && t.tm_mon>=0 && t.tm_mon<12 &&
       t.tm_mday>=1 && t.tm_mday<=31 && t.tm_hour<24 && t.tm_min<60 && t.tm_sec<60) {
        /* DS3231 is stored as UTC, independent of display timezone. */
        time_t epoch=utc_from_tm(&t);
        if(epoch>1700000000){struct timeval tv={.tv_sec=epoch};settimeofday(&tv,NULL);}
        ESP_LOGI(TAG,"DS3231 loaded (UTC)");
    } else ESP_LOGW(TAG,"DS3231 date invalid; NTP/manual time needed");
}
static void rtc_save(time_t epoch)
{
    if(!ui.rtc_present || epoch<1700000000)return;
    struct tm t;
    gmtime_r(&epoch,&t);
    if(t.tm_year+1900>2099)return;
    uint8_t buf[8]={0,binbcd(t.tm_sec),binbcd(t.tm_min),binbcd(t.tm_hour),
                     binbcd(t.tm_wday+1),binbcd(t.tm_mday),
                     binbcd(t.tm_mon+1),binbcd((t.tm_year+1900)%100)};
    esp_err_t e=i2c_master_write_to_device(I2C_NUM_0,RTC_ADDR,buf,sizeof buf,pdMS_TO_TICKS(100));
    if(e!=ESP_OK)ESP_LOGW(TAG,"Could not update RTC: %s",esp_err_to_name(e));
}
static void store(void)
{
    nvs_handle_t h;
    if(nvs_open("clock",NVS_READWRITE,&h)!=ESP_OK)return;
    nvs_set_str(h,"ssid",ui.ssid);
    nvs_set_str(h,"pass",ui.password);
    nvs_set_str(h,"tz",tz);
    nvs_set_str(h,"email",ui.email);
    nvs_set_str(h,"xmpp",ui.xmpp);
    nvs_set_u16(h,"abp",ui.alarm_bp);
    nvs_set_u16(h,"tbp",ui.timer_bp);
    nvs_set_u8(h,"ntp",ui.use_ntp);
    nvs_set_u8(h,"ae",ui.alarm_enabled);
    nvs_set_u8(h,"brightness",ui.brightness);
    nvs_set_u8(h,"theme",ui.theme);
    nvs_commit(h);
    nvs_close(h);
}
static void load(void)
{
    memset(&ui,0,sizeof ui);
    ui.page=UI_HOME;
    ui.brightness=45;
    ui.timer_bp=35; /* ~5:02 in 24h percent */
    ui.alarm_bp=2917; /* ~07:00 */
    ui.use_ntp=true;
    nvs_handle_t h;
    if(nvs_open("clock",NVS_READONLY,&h)!=ESP_OK)return;
    size_t n=sizeof(ui.ssid);nvs_get_str(h,"ssid",ui.ssid,&n);
    n=sizeof(ui.password);nvs_get_str(h,"pass",ui.password,&n);
    n=sizeof(tz);nvs_get_str(h,"tz",tz,&n);
    n=sizeof(ui.email);nvs_get_str(h,"email",ui.email,&n);
    n=sizeof(ui.xmpp);nvs_get_str(h,"xmpp",ui.xmpp,&n);
    if(nvs_get_u16(h,"abp",&ui.alarm_bp)!=ESP_OK){
        uint8_t h_old=7,m_old=0;
        nvs_get_u8(h,"ah",&h_old);
        nvs_get_u8(h,"am",&m_old);
        ui.alarm_bp=(uint16_t)(((uint32_t)(h_old%24*3600+m_old%60*60)*10000)/86400);
    }
    nvs_get_u16(h,"tbp",&ui.timer_bp);
    uint8_t ntp=1;
    nvs_get_u8(h,"ntp",&ntp);
    ui.use_ntp=ntp!=0;
    uint8_t v=0;
    if(nvs_get_u8(h,"ae",&v)==ESP_OK)ui.alarm_enabled=v!=0;
    nvs_get_u8(h,"brightness",&ui.brightness);
    nvs_get_u8(h,"theme",&ui.theme);
    nvs_close(h);
    if(ui.brightness<3||ui.brightness>100)ui.brightness=45;
    ui.theme%=4;
    if(ui.alarm_bp>9999)ui.alarm_bp=2917;
    if(ui.timer_bp<1||ui.timer_bp>999)ui.timer_bp=35;
}
static void set_tz(void)
{
    setenv("TZ",tz,1);
    tzset();
}
static void sound(int hz)
{
    if(hz==sound_hz)return;
    sound_hz=hz;
    if(hz>0)ledc_set_freq(LEDC_LOW_SPEED_MODE,LEDC_TIMER_1,hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1,hz>0?130:0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
}
static void speaker_init(void)
{
    const ledc_timer_config_t t={.speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_1,
          .duty_resolution=LEDC_TIMER_10_BIT,.freq_hz=2200,.clk_cfg=LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    const ledc_channel_config_t ch={.gpio_num=26,.speed_mode=LEDC_LOW_SPEED_MODE,
          .channel=LEDC_CHANNEL_1,.intr_type=LEDC_INTR_DISABLE,.timer_sel=LEDC_TIMER_1,.duty=0,.hpoint=0};
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}
static void i2c_init(void)
{
    const i2c_config_t conf={.mode=I2C_MODE_MASTER,.sda_io_num=21,.scl_io_num=22,
            .sda_pullup_en=GPIO_PULLUP_ENABLE,.scl_pullup_en=GPIO_PULLUP_ENABLE,
            .master.clk_speed=100000};
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0,&conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0,I2C_MODE_MASTER,0,0,0));
}
static void snpt_start_once(void)
{
    if(sntp_running || !ui.use_ntp)return;
    sntp_running=true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,"pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG,"SNTP enabled");
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;(void)data;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START && ui.ssid[0])
        esp_wifi_connect();
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected=false;
        if(ui.ssid[0])esp_wifi_connect();
    }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        wifi_connected=true;
        snpt_start_once();
    }
}
static void start_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL,NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    if(ui.ssid[0]){
        wifi_config_t sta={0};
        strncpy((char *)sta.sta.ssid,ui.ssid,sizeof sta.sta.ssid-1);
        strncpy((char *)sta.sta.password,ui.password,sizeof sta.sta.password-1);
        sta.sta.threshold.authmode=WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&sta));
        ESP_ERROR_CHECK(esp_wifi_start());wifi_started=true;
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    /* First boot: allow phone provisioning for 10 minutes automatically,
       then shut the AP off to conserve battery. */
    if(!ui.ssid[0])enable_portal();
}
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}
static int fromhex(int c)
{
    if(c>='0'&&c<='9')return c-'0';
    if(c>='A'&&c<='F')return c-'A'+10;
    if(c>='a'&&c<='f')return c-'a'+10;
    return -1;
}
static void decode(char *dst,size_t size,const char *src)
{
    size_t j=0;
    for(size_t i=0;src[i] && j+1<size;++i){
        if(src[i]=='+'){dst[j++]=' ';continue;}
        if(src[i]=='%' && src[i+1] && src[i+2]){
            int a=fromhex((unsigned char)src[i+1]),b=fromhex((unsigned char)src[i+2]);
            if(a>=0&&b>=0){dst[j++]=(char)((a<<4)|b);i+=2;continue;}
        }
        dst[j++]=src[i];
    }
    dst[j]=0;
}
static esp_err_t web_home(httpd_req_t *req)
{
    static const char *html=
       "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
       "<title>100% Clock setup</title><style>"
       "body{background:#101724;color:#ffebe0;font:17px sans-serif;max-width:480px;"
       "margin:2em auto;padding:1em}input,textarea,button{display:block;padding:12px;"
       "box-sizing:border-box;width:100%;margin:10px 0;border-radius:8px;border:0}"
       "button{background:#efadc1;color:#101724;font-weight:bold}"
       "</style><h2>100% Clock</h2><p>Wi-Fi, time zone and contact information.</p>"
       "<form method=post action=/save>"
       "<label>Wi-Fi SSID<input name=ssid maxlength=32></label>"
       "<label>Wi-Fi password<input name=pass type=password maxlength=63></label>"
       "<label>Time zone (POSIX)<input name=tz value='CET-1CEST,M3.5.0,M10.5.0/3' maxlength=79></label>"
       "<label>XMPP JID<input name=xmpp maxlength=64 placeholder='user@example.org'></label>"
       "<label>Email address<input name=email type=email maxlength=64></label>"
       "<p>Place face.bmp (24-bit BMP) at the root of a microSD for the contact photo.</p>"
       "<button>Save and restart</button></form>"
       "<p>Clock runs offline too: MENU &gt; SET TIME. This setup AP closes after 10 minutes.</p>";
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    return httpd_resp_sendstr(req,html);
}
static esp_err_t web_save(httpd_req_t *req)
{
    if(req->content_len>950){
        httpd_resp_send_err(req,HTTPD_413_CONTENT_TOO_LARGE,"Form too large");return ESP_FAIL;
    }
    char buf[952]={0};
    int got=0;
    while(got<req->content_len){
        int n=httpd_req_recv(req,buf+got,req->content_len-got);
        if(n<=0){httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Read failed");return ESP_FAIL;}
        got+=n;
    }
    char *saveptr;
    for(char *part=strtok_r(buf,"&",&saveptr);part;part=strtok_r(NULL,"&",&saveptr)){
        char *eq=strchr(part,'=');
        if(!eq)continue;
        *eq++='\0';
        if(strcmp(part,"ssid")==0)decode(ui.ssid,sizeof ui.ssid,eq);
        else if(strcmp(part,"pass")==0)decode(ui.password,sizeof ui.password,eq);
        else if(strcmp(part,"tz")==0)decode(tz,sizeof tz,eq);
        else if(strcmp(part,"xmpp")==0)decode(ui.xmpp,sizeof ui.xmpp,eq);
        else if(strcmp(part,"email")==0)decode(ui.email,sizeof ui.email,eq);
    }
    store();
    httpd_resp_sendstr(req,"<h2>Saved. Restarting clock.</h2>");
    xTaskCreate(reboot_task,"reboot",2048,NULL,2,NULL);
    return ESP_OK;
}
static void enable_portal(void)
{
    if(ui.portal_enabled){
        portal_deadline=esp_timer_get_time()+PORTAL_SECONDS*1000000LL;
        return;
    }
    wifi_config_t ap={0};
    uint8_t mac[6]={0};
    esp_read_mac(mac,ESP_MAC_WIFI_STA);
    snprintf((char *)ap.ap.ssid,sizeof ap.ap.ssid,
             "100PERCENT-%02X%02X",mac[4],mac[5]);
    snprintf(ui.portal_ssid,sizeof ui.portal_ssid,"%s",(char *)ap.ap.ssid);
    /* New random WPA2 key on each boot, visible on the clock itself. */
    static const char abc[]="ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for(unsigned i=0;i<12;++i)
        ui.portal_password[i]=abc[esp_random()%(sizeof abc-1)];
    ui.portal_password[12]=0;
    snprintf((char *)ap.ap.password,sizeof ap.ap.password,"%s",ui.portal_password);
    ap.ap.ssid_len=strlen((char *)ap.ap.ssid);
    ap.ap.channel=6;
    ap.ap.max_connection=2;
    ap.ap.authmode=WIFI_AUTH_WPA2_PSK;
    if(wifi_started)ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(ui.ssid[0]?WIFI_MODE_APSTA:WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    ESP_ERROR_CHECK(esp_wifi_start());wifi_started=true;
    if(ui.ssid[0])esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if(!web_server){
        httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
        cfg.max_uri_handlers=4;
        if(httpd_start(&web_server,&cfg)==ESP_OK){
            httpd_uri_t get={.uri="/",.method=HTTP_GET,.handler=web_home,.user_ctx=NULL};
            httpd_uri_t post={.uri="/save",.method=HTTP_POST,.handler=web_save,.user_ctx=NULL};
            httpd_register_uri_handler(web_server,&get);
            httpd_register_uri_handler(web_server,&post);
        }
    }
    ui.portal_enabled=true;
    portal_deadline=esp_timer_get_time()+PORTAL_SECONDS*1000000LL;
    ESP_LOGI(TAG,"Provision AP 100PERCENT-%02X%02X, URL http://192.168.4.1/",mac[4],mac[5]);
}
static void disable_portal(void)
{
    if(!ui.portal_enabled)return;
    if(web_server){httpd_stop(web_server);web_server=NULL;}
    if(wifi_started)esp_wifi_stop();
    if(ui.ssid[0]){
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();wifi_started=true;
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {esp_wifi_set_mode(WIFI_MODE_NULL);wifi_started=false;}
    ui.portal_enabled=false;
    ui.portal_password[0]=0;
    ui.portal_ssid[0]=0;
}
static void init_time_editor(void)
{
    time_t now=time(NULL);
    struct tm t={0};
    if(now>1700000000)localtime_r(&now,&t);
    else {t.tm_year=126;t.tm_mon=0;t.tm_mday=1;}
    ui.year=t.tm_year+1900;ui.month=t.tm_mon+1;ui.day=t.tm_mday;
    ui.hour=t.tm_hour;ui.minute=t.tm_min;ui.second=t.tm_sec;
}
static void adjust_time_field(int col,int row,int delta)
{
    if(row==0){
        if(col==0){ui.year+=delta;if(ui.year<2024)ui.year=2099;if(ui.year>2099)ui.year=2024;}
        if(col==1){int v=ui.month+delta;ui.month=v<1?12:v>12?1:v;}
        if(col==2){int v=ui.day+delta;ui.day=v<1?31:v>31?1:v;}
    }else{
        if(col==0){int v=ui.hour+delta;ui.hour=v<0?23:v>23?0:v;}
        if(col==1){int v=ui.minute+delta;ui.minute=v<0?59:v>59?0:v;}
        if(col==2){int v=ui.second+delta;ui.second=v<0?59:v>59?0:v;}
    }
}
static void set_manual_time(void)
{
    struct tm t={.tm_year=ui.year-1900,.tm_mon=ui.month-1,.tm_mday=ui.day,
        .tm_hour=ui.hour,.tm_min=ui.minute,.tm_sec=ui.second,.tm_isdst=-1};
    time_t unix_time=mktime(&t);
    if(unix_time>1700000000){
        struct timeval tv={.tv_sec=unix_time};
        settimeofday(&tv,NULL);
        rtc_save(unix_time);
        ESP_LOGI(TAG,"Manual local time set");
    }
    ui.page=UI_HOME;
}
static void silence(void)
{
    ui.alarm_ringing=false;
    ui.timer_ringing=false;
    sound(0);
}
static void start_keyboard(bool password)
{
    ui.editing_password=password;ui.keyboard_lower=false;
    snprintf(ui.keyboard_value,sizeof ui.keyboard_value,"%s",password?ui.password:ui.ssid);
    ui.page=UI_KEYBOARD;
}
static void open_files(void)
{
    ui.sd_ready=media_select(0,ui.file_name,sizeof ui.file_name);
    ui.file_index=0;
    ui.page=UI_FILES;
}
static void touch(int x,int y)
{
    last_touch_us=esp_timer_get_time();
    if(ui.alarm_ringing||ui.timer_ringing){silence();return;}
    if(ui.page==UI_HOME){ui.page=UI_MENU;return;}
    if(y<44 && x>=247){
        if(ui.page==UI_MENU)ui.page=UI_HOME;
        else if(ui.page==UI_KEYBOARD)ui.page=UI_WIFI;
        else if(ui.page==UI_FILE_VIEW)ui.page=UI_FILES;
        else ui.page=UI_MENU;
        return;
    }
    if(ui.page==UI_MENU){
        int col=x<108?0:x<211?1:2;
        if(y>=54&&y<111){
            if(col==0)ui.page=UI_ALARM;
            else if(col==1)ui.page=UI_TIMER;
            else {ui.page=UI_SET_TIME;init_time_editor();}
        }else if(y>=119&&y<176){
            if(col==0)ui.page=UI_WIFI;
            else if(col==1){ui.page=UI_CONTACT;media_photo();}
            else ui.page=UI_LOOK;
        }else if(y>=188&&y<239)open_files();
    }else if(ui.page==UI_ALARM){
        if(y>=167&&y<204){
            int col=x<80?0:x<160?1:x<240?2:3;
            int step=col==0?-100:col==1?-1:col==2?1:100;
            int v=ui.alarm_bp+step;
            if(v<0)v+=10000;
            if(v>=10000)v-=10000;
            ui.alarm_bp=(uint16_t)v;
            store();
        }else if(y>=208){
            ui.alarm_enabled=!ui.alarm_enabled;store();
        }
    }else if(ui.page==UI_TIMER){
        if(y>=167&&y<204){
            int col=x<80?0:x<160?1:x<240?2:3;
            int step=col==0?-10:col==1?-1:col==2?1:10;
            int v=ui.timer_bp+step;
            if(v<1)v=1;if(v>999)v=999;
            ui.timer_bp=(uint16_t)v;
            /* Timer adjustments apply to the next run, not an active countdown. */
            store();
        }else if(y>=208){
            if(ui.timer_running){ui.timer_running=false;timer_deadline=0;}
            else{
                int64_t duration_us=(int64_t)ui.timer_bp*8640000LL;
                timer_deadline=esp_timer_get_time()+duration_us;
                ui.timer_seconds_left=(int)(duration_us/1000000);
                ui.timer_running=true;
            }
        }
    }else if(ui.page==UI_SET_TIME){
        if(y>=50&&y<83 && x<=220){
            ui.use_ntp=!ui.use_ntp;
            if(ui.use_ntp){
                if(wifi_connected)snpt_start_once();
            }else if(sntp_running){
                esp_sntp_stop();sntp_running=false;
            }
            store();
            return;
        }
        if(ui.use_ntp)return;
        if(y>=210){set_manual_time();return;}
        int row=-1;
        if(y>=99&&y<124)row=0;
        if(y>=160&&y<185)row=1;
        if(row>=0){
            int col=x/102,localx=x%102;
            if(col>=0&&col<3){
                if(localx>=66&&localx<89)adjust_time_field(col,row,-1);
                else if(localx>=89)adjust_time_field(col,row,1);
            }
        }
    }else if(ui.page==UI_WIFI){
        if(y>=106&&y<146)start_keyboard(x>=160);
        else if(y>=156&&y<195)enable_portal();
    }else if(ui.page==UI_KEYBOARD){
        if(y>=82&&y<214){
            int row=(y-82)/22,col=(x-5)/45;
            int ix=row*7+col;
            if(x>=5&&col>=0&&col<7&&ix>=0&&ix<42){
                size_t n=strlen(ui.keyboard_value),max=ui.editing_password?63:32;
                if(n<max){
                    char c="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-@!?"[ix];
                    if(ui.keyboard_lower&&isalpha((unsigned char)c))
                        c=(char)tolower((unsigned char)c);
                    ui.keyboard_value[n]=c;
                    ui.keyboard_value[n+1]=0;
                }
            }
        }else if(y>=214){
            if(x<102)ui.keyboard_lower=!ui.keyboard_lower;
            else if(x<208){size_t n=strlen(ui.keyboard_value);if(n)ui.keyboard_value[n-1]=0;}
            else{
                if(ui.editing_password){
                    snprintf(ui.password,sizeof ui.password,"%s",ui.keyboard_value);
                }else snprintf(ui.ssid,sizeof ui.ssid,"%.32s",ui.keyboard_value);
                store();
                ui.page=UI_WIFI;
                /* Wi-Fi config is loaded at start; reconfigure on reboot. */
                xTaskCreate(reboot_task,"reboot",2048,NULL,2,NULL);
            }
        }
    }else if(ui.page==UI_LOOK){
        if(y>=91&&y<137){ui.theme=(ui.theme+1)%4;store();}
        if(y>=181&&y<225){
            int v=ui.brightness+(x<160?-10:10);
            if(v<3)v=3;
            if(v>100)v=100;
            ui.brightness=(uint8_t)v;store();
        }
    }else if(ui.page==UI_FILES){
        if(y>=166&&y<213){
            if(x<106)media_select(-1,ui.file_name,sizeof ui.file_name);
            else if(x<212)media_select(1,ui.file_name,sizeof ui.file_name);
            else if(ui.file_name[0]){
                media_open(ui.file_preview,sizeof ui.file_preview);
                ui.page=UI_FILE_VIEW;
            }
        }else if(!ui.sd_ready && y>=70)open_files();
    }
}
void app_main(void)
{
    esp_err_t e=nvs_flash_init();
    if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());e=nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    load();set_tz();
    i2c_init();
    rtc_load();
    ESP_ERROR_CHECK(screen_init());
    speaker_init();
    start_wifi();
    last_touch_us=esp_timer_get_time();
    int last_day_bp=-1,previous_bp=-1,previous_day=-1;
    int last_timer_sec=-1;
    int64_t last_rtc_poll=0;
    int last_brightness=-1;
    bool last_connected=false,last_portal=false;
    ui.page=UI_HOME;
    screen_draw(&ui);
    while(true){
        int64_t now_us=esp_timer_get_time();
        int x=0,y=0;bool draw=false;
        if(screen_poll_touch(&x,&y)){touch(x,y);draw=true;}
        if(ui.portal_enabled && now_us>portal_deadline){
            disable_portal();draw=true;
        }
        time_t epoch=time(NULL);
        ui.time_valid=epoch>1700000000;
        if(ui.time_valid)localtime_r(&epoch,&ui.local);
        if(ui.timer_running){
            int64_t diff=timer_deadline-now_us;
            ui.timer_seconds_left=diff>0?(int)((diff+999999)/1000000):0;
            if(diff<=0){ui.timer_running=false;ui.timer_ringing=true;
                ring_deadline=now_us+60000000LL;draw=true;}
        }
        if(ui.time_valid){
            int today=ui.local.tm_year*400+ui.local.tm_yday;
            int sec=ui.local.tm_hour*3600+ui.local.tm_min*60+ui.local.tm_sec;
            int bp=(int)((int64_t)sec*10000/86400);
            if(ui.alarm_enabled && today!=last_alarm_date){
                bool crossing=(previous_day==today && previous_bp<ui.alarm_bp &&
                               bp>=ui.alarm_bp);
                bool midnight=(previous_day!=-1 && previous_day!=today &&
                               ui.alarm_bp==0);
                if(crossing||midnight){
                    ui.alarm_ringing=true;
                    last_alarm_date=today;
                    ring_deadline=now_us+60000000LL;
                    draw=true;
                }
            }
            previous_day=today;
            previous_bp=bp;
            if(bp!=last_day_bp){last_day_bp=bp;draw=true;}
        }
        if(ui.timer_running && ui.timer_seconds_left!=last_timer_sec &&
           ui.page==UI_TIMER){draw=true;}
        last_timer_sec=ui.timer_seconds_left;
        if((ui.alarm_ringing||ui.timer_ringing)&&now_us>ring_deadline){
            silence();draw=true;
        }
        /* Three-note, relatively quiet alert instead of a 2.2kHz radio beep. */
        int hz=0;
        if(ui.alarm_ringing||ui.timer_ringing){
            int note=(int)((now_us/185000)%16);
            if(note==0)hz=523;
            else if(note==1)hz=659;
            else if(note==2||note==3)hz=784;
        }
        sound(hz);
        if(wifi_connected && ui.use_ntp && ui.time_valid && ui.rtc_present &&
           now_us-last_rtc_poll>3600LL*1000000){
            rtc_save(epoch);
            last_rtc_poll=now_us;
        }
        if(last_connected!=wifi_connected||last_portal!=ui.portal_enabled){
            last_connected=wifi_connected;last_portal=ui.portal_enabled;draw=true;
        }
        ui.wifi_connected=wifi_connected;
        int target_brightness=ui.brightness;
        if(now_us-last_touch_us>40000000LL){
            target_brightness=(target_brightness*(ui.theme==3?12:25))/100;
            if(target_brightness<3)target_brightness=3;
        }
        if(last_brightness!=target_brightness){
            screen_set_brightness((uint8_t)target_brightness);
            last_brightness=target_brightness;
        }
        if(draw)screen_draw(&ui);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
