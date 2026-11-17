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
#include "audio.h"
#include "tv_video.h"
#include "contact_defaults.h"
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
#define NTP_INTERVAL_US (300LL*1000000LL)
#define NTP_WINDOW_US (35000000LL)
#define WIFI_RETRY_US (5000000LL)
#define NTP_FIRST_RETRY_US (20000000LL)
#define ALERT_DURATION_US (86400000LL) /* 0.10% of one day = 86.4 seconds */
static const char *TAG = "one_hundred";
static ui_state_t ui;
static char tz[85]="CET-1CEST,M3.5.0,M10.5.0/3";
static volatile bool wifi_connected;
static bool sntp_running;
static bool wifi_started;
static httpd_handle_t web_server;
static int64_t portal_deadline;
static int64_t next_ntp_sync_us;
static uint8_t initial_sync_failures;
static int64_t wifi_session_started_us;
static volatile int64_t wifi_retry_after_us;
static volatile bool ntp_updated;
static volatile bool wifi_scan_done;
static bool wifi_scan_active;
static int64_t timer_deadline;
static int64_t ring_deadline;
static int64_t ring_started_us;
static int last_alarm_date=-1;
static bool alarm_adjust_dirty;
static int64_t last_alarm_adjust_us;
static int64_t last_touch_us;
static void enable_portal(void);
static void disable_portal(void);

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
    nvs_set_str(h,"alarm_snd",ui.alarm_sound);
    nvs_set_str(h,"timer_snd",ui.timer_sound);
    nvs_set_u8(h,"contact_t",ui.contact_tune);
    nvs_set_str(h,"sms",ui.sms);
    nvs_set_u32(h,"ct_hash",CLOCK_CONTACT_HASH);
    nvs_set_u16(h,"abp",ui.alarm_bp);
    nvs_set_u16(h,"tbp",ui.timer_bp);
    nvs_set_u32(h,"timer_ms",ui.timer_duration_ms);
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
    ui.timer_bp=35; /* 5:00 displays as 0.35% of a day */
    ui.timer_duration_ms=300000; /* exactly 5 minutes */
    ui.alarm_bp=2917; /* ~07:00 */
    ui.use_ntp=true;
    snprintf(ui.alarm_sound,sizeof ui.alarm_sound,"builtin:0");
    snprintf(ui.timer_sound,sizeof ui.timer_sound,"builtin:0");
    ui.contact_tune=1;
    /* The built-in defaults exist even if NVS has never been initialized.
     * The private override is selected at LOCAL build time, not at runtime. */
    snprintf(ui.xmpp,sizeof ui.xmpp,"%s",CLOCK_CONTACT_XMPP);
    snprintf(ui.email,sizeof ui.email,"%s",CLOCK_CONTACT_EMAIL);
    snprintf(ui.sms,sizeof ui.sms,"%s",CLOCK_CONTACT_SMS);
    nvs_handle_t h;
    if(nvs_open("clock",NVS_READONLY,&h)!=ESP_OK)return;
    size_t n=sizeof(ui.ssid);nvs_get_str(h,"ssid",ui.ssid,&n);
    n=sizeof(ui.password);nvs_get_str(h,"pass",ui.password,&n);
    n=sizeof(ui.portal_password);nvs_get_str(h,"apkey",ui.portal_password,&n);
    n=sizeof(tz);nvs_get_str(h,"tz",tz,&n);
    n=sizeof(ui.email);nvs_get_str(h,"email",ui.email,&n);
    n=sizeof(ui.xmpp);nvs_get_str(h,"xmpp",ui.xmpp,&n);
    n=sizeof(ui.sms);nvs_get_str(h,"sms",ui.sms,&n);
    n=sizeof(ui.alarm_sound);nvs_get_str(h,"alarm_snd",ui.alarm_sound,&n);
    n=sizeof(ui.timer_sound);nvs_get_str(h,"timer_snd",ui.timer_sound,&n);
    nvs_get_u8(h,"contact_t",&ui.contact_tune);
    uint32_t previous_contact_hash=0;
    nvs_get_u32(h,"ct_hash",&previous_contact_hash);
    if(nvs_get_u16(h,"abp",&ui.alarm_bp)!=ESP_OK){
        uint8_t h_old=7,m_old=0;
        nvs_get_u8(h,"ah",&h_old);
        nvs_get_u8(h,"am",&m_old);
        ui.alarm_bp=(uint16_t)(((uint32_t)(h_old%24*3600+m_old%60*60)*10000)/86400);
    }
    nvs_get_u16(h,"tbp",&ui.timer_bp);
    if(nvs_get_u32(h,"timer_ms",&ui.timer_duration_ms)!=ESP_OK){
        /* Upgrade older presets, but preserve a five-minute first use. */
        ui.timer_duration_ms=ui.timer_bp==35?300000:(uint32_t)ui.timer_bp*8640u;
    }
    uint8_t ntp=1;
    nvs_get_u8(h,"ntp",&ntp);
    ui.use_ntp=ntp!=0;
    uint8_t v=0;
    if(nvs_get_u8(h,"ae",&v)==ESP_OK)ui.alarm_enabled=v!=0;
    nvs_get_u8(h,"brightness",&ui.brightness);
    nvs_get_u8(h,"theme",&ui.theme);
    nvs_close(h);
    /* On a changed firmware contact file, apply its defaults exactly once.
     * Later edits made in the Wi-Fi portal remain valid until the contact
     * source file changes again in another build. */
    if(previous_contact_hash!=CLOCK_CONTACT_HASH){
        snprintf(ui.xmpp,sizeof ui.xmpp,"%s",CLOCK_CONTACT_XMPP);
        snprintf(ui.email,sizeof ui.email,"%s",CLOCK_CONTACT_EMAIL);
        snprintf(ui.sms,sizeof ui.sms,"%s",CLOCK_CONTACT_SMS);
        nvs_handle_t contacts;
        if(nvs_open("clock",NVS_READWRITE,&contacts)==ESP_OK){
            nvs_set_str(contacts,"xmpp",ui.xmpp);
            nvs_set_str(contacts,"email",ui.email);
            nvs_set_str(contacts,"sms",ui.sms);
            nvs_set_u32(contacts,"ct_hash",CLOCK_CONTACT_HASH);
            nvs_commit(contacts);
            nvs_close(contacts);
        }
    }
    if(ui.contact_tune>=AUDIO_BUILTIN_COUNT)ui.contact_tune=1;
    if(strncmp(ui.alarm_sound,"builtin:",8) && strncmp(ui.alarm_sound,"sd:",3))
        snprintf(ui.alarm_sound,sizeof ui.alarm_sound,"builtin:0");
    if(strncmp(ui.timer_sound,"builtin:",8) && strncmp(ui.timer_sound,"sd:",3))
        snprintf(ui.timer_sound,sizeof ui.timer_sound,"builtin:0");
    if(ui.brightness<3||ui.brightness>100)ui.brightness=45;
    ui.theme%=4;
    if(ui.alarm_bp>9999)ui.alarm_bp=2917;
    if(ui.timer_duration_ms<8640u || ui.timer_duration_ms>8631360u)
        ui.timer_duration_ms=300000;
    ui.timer_bp=(uint16_t)((ui.timer_duration_ms+4320u)/8640u);
}
static void set_tz(void)
{
    setenv("TZ",tz,1);
    tzset();
}
static void i2c_init(void)
{
    const i2c_config_t conf={.mode=I2C_MODE_MASTER,.sda_io_num=21,.scl_io_num=22,
            .sda_pullup_en=GPIO_PULLUP_ENABLE,.scl_pullup_en=GPIO_PULLUP_ENABLE,
            .master.clk_speed=100000};
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0,&conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0,I2C_MODE_MASTER,0,0,0));
}
static void ntp_time_updated(struct timeval *tv)
{
    (void)tv;
    /* lwIP task callback: let the main loop stop the radio and update RTC. */
    ntp_updated=true;
}
static void stop_sntp(void)
{
    if(sntp_running){
        esp_sntp_stop();
        sntp_running=false;
    }
}
static void snpt_start_once(void)
{
    if(sntp_running || !ui.use_ntp)return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(ntp_time_updated);
    esp_sntp_setservername(0,"pool.ntp.org");
    esp_sntp_init();
    sntp_running=true;
    ESP_LOGI(TAG,"NTP started (short Wi-Fi sync window)");
}
static void stop_wifi_radio(void)
{
    stop_sntp();
    /* Mark inactive before esp_wifi_stop emits disconnect events. */
    wifi_session_started_us=0;
    wifi_retry_after_us=0;
    ui.wifi_connecting=false;
    ntp_updated=false;
    if(wifi_started){
        wifi_started=false;
        ESP_ERROR_CHECK(esp_wifi_stop());
    }
    wifi_connected=false;
}
static void start_ntp_radio(void)
{
    if(!ui.use_ntp || !ui.ssid[0] || ui.portal_enabled || wifi_scan_active ||
       wifi_started)return;
    ui.wifi_disconnect_reason=0;
    ui.wifi_connecting=true;
    wifi_retry_after_us=0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_started=true; /* Event callback can run as soon as start succeeds. */
    esp_err_t start_result=esp_wifi_start();
    if(start_result!=ESP_OK){
        wifi_started=false;
        ESP_ERROR_CHECK(start_result);
    }
    wifi_session_started_us=esp_timer_get_time();
    ESP_LOGI(TAG,"Wi-Fi session: saved SSID length=%u, previous first-sync failures=%u",
             (unsigned)strlen(ui.ssid),(unsigned)initial_sync_failures);
}
static void request_ntp_sync_now(void)
{
    /* NTP enabled with saved credentials must reconnect immediately,
     * even if the previous attempt timed out or Wi-Fi was stopped.
     * A held portal/scan suspends the attempt until it closes. */
    ui.ntp_synced=false;
    ui.wifi_disconnect_reason=0;
    initial_sync_failures=0;
    ntp_updated=false;
    next_ntp_sync_us=0;
    if(!ui.use_ntp || !ui.ssid[0])return;
    if(ui.portal_enabled || wifi_scan_active){
        ESP_LOGI(TAG,"NTP requested; waiting for portal/scan to finish");
        return;
    }
    if(wifi_started)stop_wifi_radio();
    ESP_LOGI(TAG,"NTP requested immediately using saved SSID (length=%u)",
             (unsigned)strlen(ui.ssid));
    start_ntp_radio();
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data)
{
    (void)arg;
    if(base==WIFI_EVENT && id==WIFI_EVENT_SCAN_DONE){
        if(wifi_scan_active)wifi_scan_done=true;
        return;
    }
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START && ui.ssid[0] &&
       ui.use_ntp && !ui.portal_enabled && !wifi_scan_active){
        esp_wifi_connect();
    }
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected=false;
        if(wifi_started && ui.use_ntp && !ui.portal_enabled && !wifi_scan_active){
            const wifi_event_sta_disconnected_t *ev=data;
            ui.wifi_disconnect_reason=ev?ev->reason:0;
            ui.wifi_connecting=false;
            wifi_retry_after_us=esp_timer_get_time()+WIFI_RETRY_US;
            ESP_LOGW(TAG,"Wi-Fi disconnected (reason=%u), retrying",(unsigned)ui.wifi_disconnect_reason);
        }
    }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP && !wifi_scan_active){
        wifi_connected=true;
        ui.wifi_connecting=false;
        ui.wifi_disconnect_reason=0;
        wifi_retry_after_us=0;
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
        ESP_LOGI(TAG,"Boot Wi-Fi: SSID present, NTP %s",
                 ui.use_ntp?"enabled (sync now)":"disabled");
        wifi_config_t sta={0};
        strncpy((char *)sta.sta.ssid,ui.ssid,sizeof sta.sta.ssid-1);
        /* A 64-character WPA2 key may fill the entire buffer (raw PSK). */
        size_t passlen=strlen(ui.password);
        if(passlen>sizeof sta.sta.password)passlen=sizeof sta.sta.password;
        memcpy(sta.sta.password,ui.password,passlen);
        sta.sta.threshold.authmode=WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&sta));
        if(ui.use_ntp)start_ntp_radio();
    }
    /* First boot: the phone setup AP is available for ten minutes. */
    if(!ui.ssid[0])enable_portal();
}
static void apply_wifi_credentials(void)
{
    /* No whole-device reboot for keyboard/scan edits: keep the Wi-Fi page
     * visible so a bad SSID or password is distinguishable from booting. */
    if(ui.portal_enabled)disable_portal();
    stop_wifi_radio();
    wifi_config_t sta={0};
    strncpy((char *)sta.sta.ssid,ui.ssid,sizeof sta.sta.ssid-1);
    size_t passlen=strlen(ui.password);
    if(passlen>sizeof sta.sta.password)passlen=sizeof sta.sta.password;
    memcpy(sta.sta.password,ui.password,passlen);
    sta.sta.threshold.authmode=WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&sta));
    ui.wifi_disconnect_reason=0;
    ui.ntp_synced=false;
    ntp_updated=false;
    next_ntp_sync_us=0;
    initial_sync_failures=0;
    if(ui.use_ntp && ui.ssid[0])request_ntp_sync_now();
}
static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}
/* WPA2 passphrases are 8..63 characters; exactly 64 requires HEX PSK.
   An empty password is permitted for an open access point. */
static bool valid_wifi_password(const char *pass)
{
    size_t n=strlen(pass);
    if(n==0)return true;
    if(n>=8 && n<=63)return true;
    if(n!=64)return false;
    for(size_t i=0;i<n;++i){
        if(!isxdigit((unsigned char)pass[i]))return false;
    }
    return true;
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
       "<label>Wi-Fi password (visible)<input name=pass type=text maxlength=64 autocapitalize=off autocomplete=off></label>"
       "<label>Time zone (POSIX)<input name=tz value='CET-1CEST,M3.5.0,M10.5.0/3' maxlength=79></label>"
       "<label>XMPP JID<input name=xmpp maxlength=64 placeholder='user@example.org'></label>"
       "<label>Email address<input name=email type=email maxlength=64></label>"
       "<label>SMS number<input name=sms maxlength=64></label>"
       "<p>CONTACT photo is built into the firmware from face.bmp.</p>"
       "<button>Save and restart</button></form>"
       "<p>Clock runs offline too: MENU &gt; SET TIME. This setup AP stays available for 10 minutes.</p>";
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
        else if(strcmp(part,"sms")==0)decode(ui.sms,sizeof ui.sms,eq);
    }
    if(!valid_wifi_password(ui.password)){
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,
            "Wi-Fi password must be 8-63 characters or exactly 64 hexadecimal digits.");
        return ESP_FAIL;
    }
    store();
    httpd_resp_sendstr(req,"<h2>Saved. Restarting clock.</h2>");
    xTaskCreate(reboot_task,"reboot",2048,NULL,2,NULL);
    return ESP_OK;
}
/* WPA2 requires at least 8 characters. Reuse a random device-specific
 * lowercase key after portal restarts so phones can reconnect normally. */
static void ensure_portal_password(void)
{
    static const char letters[]="abcdefghjkmnpqrstuvwxyz";
    if(strlen(ui.portal_password)==8 &&
       strspn(ui.portal_password,letters)==8)return;
    for(size_t i=0;i<8;++i)
        ui.portal_password[i]=letters[esp_random()%(sizeof letters-1)];
    ui.portal_password[8]=0;
    nvs_handle_t h;
    if(nvs_open("clock",NVS_READWRITE,&h)==ESP_OK){
        if(nvs_set_str(h,"apkey",ui.portal_password)==ESP_OK)
            nvs_commit(h);
        nvs_close(h);
    }
}
static void enable_portal(void)
{
    /* Restart even when UI says ACTIVE; AP beacons may have disappeared.
     * Keep the exact same password across restarts and reboots. */
    ensure_portal_password();
    wifi_config_t ap={0};
    uint8_t mac[6]={0};
    esp_read_mac(mac,ESP_MAC_WIFI_STA);
    /* New SSID suffix avoids old 12-character credentials cached by phones. */
    snprintf((char *)ap.ap.ssid,sizeof ap.ap.ssid,
             "100PERCENT-%02X%02X-SETUP",mac[4],mac[5]);
    snprintf(ui.portal_ssid,sizeof ui.portal_ssid,"%s",(char *)ap.ap.ssid);
    snprintf((char *)ap.ap.password,sizeof ap.ap.password,"%s",ui.portal_password);
    ap.ap.ssid_len=strlen((char *)ap.ap.ssid);
    ap.ap.channel=6;
    ap.ap.max_connection=2;
    ap.ap.authmode=WIFI_AUTH_WPA2_PSK;

    /* APSTA inherits the home router's channel and can become hard to find.
     * Setup needs no internet, so use AP-only on channel 6 until it closes. */
    ui.portal_enabled=true;
    ntp_updated=false;
    stop_wifi_radio();
    if(web_server){httpd_stop(web_server);web_server=NULL;}
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started=true;

    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers=4;
    if(httpd_start(&web_server,&cfg)==ESP_OK){
        httpd_uri_t get={.uri="/",.method=HTTP_GET,.handler=web_home,.user_ctx=NULL};
        httpd_uri_t post={.uri="/save",.method=HTTP_POST,.handler=web_save,.user_ctx=NULL};
        httpd_register_uri_handler(web_server,&get);
        httpd_register_uri_handler(web_server,&post);
    } else ESP_LOGE(TAG,"Setup web server could not start");

    portal_deadline=esp_timer_get_time()+PORTAL_SECONDS*1000000LL;
    ui.portal_seconds_left=PORTAL_SECONDS;
    ESP_LOGI(TAG,"Provision AP %s (channel 6), URL http://192.168.4.1/",
             ui.portal_ssid);
}
static void disable_portal(void)
{
    if(!ui.portal_enabled)return;
    if(web_server){httpd_stop(web_server);web_server=NULL;}
    stop_wifi_radio();
    ui.portal_enabled=false;
    ui.portal_seconds_left=0;
    if(ui.ssid[0] && ui.use_ntp)start_ntp_radio();
    else ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    /* Password is retained in NVS for the next setup session. */
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
        if(col==0){
            ui.year+=delta;
            if(ui.year<2024)ui.year=2099;
            if(ui.year>2099)ui.year=2024;
        }
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
    ui.alert_flash=false;
    ring_started_us=0;
    audio_stop();
}
static void start_keyboard(bool password)
{
    ui.editing_password=password;
    ui.keyboard_lower=false;
    ui.keyboard_error=false;
    snprintf(ui.keyboard_value,sizeof ui.keyboard_value,"%s",password?ui.password:ui.ssid);
    ui.page=UI_KEYBOARD;
}
static void end_wifi_scan(void)
{
    if(wifi_scan_active && wifi_started){
        esp_wifi_scan_stop(); /* harmless if scan already completed */
        esp_wifi_clear_ap_list();
    }
    wifi_scan_active=false;
    wifi_scan_done=false;
    ui.wifi_scanning=false;
    stop_wifi_radio();
    next_ntp_sync_us=ui.use_ntp?0:esp_timer_get_time()+NTP_INTERVAL_US;
}
static void begin_wifi_scan(void)
{
    /* Scan is opt-in; routine NTP sync does not continuously scan networks. */
    if(ui.portal_enabled)disable_portal();
    if(wifi_scan_active)end_wifi_scan();
    stop_wifi_radio();
    ui.page=UI_WIFI_SCAN;
    ui.wifi_scan_page=0;
    ui.wifi_scan_count=0;
    ui.wifi_scanning=true;
    wifi_scan_done=false;
    wifi_scan_active=true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started=true;
    esp_err_t err=esp_wifi_scan_start(NULL,false);
    if(err!=ESP_OK){
        ESP_LOGW(TAG,"Wi-Fi scan start failed: %s",esp_err_to_name(err));
        end_wifi_scan();
    }
}
static void receive_wifi_scan(void)
{
    wifi_scan_done=false;
    uint16_t total=0;
    if(esp_wifi_scan_get_ap_num(&total)==ESP_OK && total){
        uint16_t take=total>32?32:total;
        wifi_ap_record_t *records=calloc(take,sizeof(*records));
        if(records){
            if(esp_wifi_scan_get_ap_records(&take,records)==ESP_OK){
                for(uint16_t i=0;i<take && ui.wifi_scan_count<16;++i){
                    char ssid[33]={0};
                    memcpy(ssid,records[i].ssid,32);
                    if(!ssid[0])continue;
                    bool duplicate=false;
                    for(int j=0;j<ui.wifi_scan_count;++j)
                        if(strcmp(ssid,ui.wifi_scan_ssids[j])==0){duplicate=true;break;}
                    if(!duplicate)
                        snprintf(ui.wifi_scan_ssids[ui.wifi_scan_count++],33,"%s",ssid);
                }
            }else esp_wifi_clear_ap_list();
            free(records);
        }else esp_wifi_clear_ap_list();
    }else esp_wifi_clear_ap_list();
    /* Preserve displayed scan results while powering the radio back down. */
    end_wifi_scan();
}
static void open_files(void)
{
    ui.sd_ready=media_select(0,ui.file_name,sizeof ui.file_name);
    ui.file_index=0;
    ui.page=UI_FILES;
}
static void save_audio_selection(char *dst,size_t capacity,const char *key)
{
    /* Sound keys and TF filenames are byte strings; preserve complete UTF-8
     * whenever it fits rather than use printf's unsafe implicit truncation. */
    if(!capacity)return;
    size_t n=strnlen(key,capacity-1);
    memcpy(dst,key,n);
    dst[n]=0;
}
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
static bool tv_pending, tv_bmp;
static int64_t tv_started_us;
static char tv_filename[128];
static void tv_request(const char *name,bool bmp)
{
    snprintf(tv_filename,sizeof tv_filename,"%s",name?name:"");
    tv_bmp=bmp;
    tv_pending=true;
    tv_started_us=esp_timer_get_time();
    ESP_LOGW(TAG,"Composite requested for TF file: %s, BMP=%d",
             tv_filename,(int)tv_bmp);
}
#endif
static void touch(int x,int y)
{
#ifdef CONFIG_CLOCK_TOUCH_DEBUG
    ESP_LOGI(TAG,"touch x=%d y=%d page=%d",x,y,(int)ui.page);
#endif
    last_touch_us=esp_timer_get_time();
    if(ui.alarm_ringing||ui.timer_ringing){silence();return;}
    if(ui.page==UI_HOME){ui.page=UI_MENU;return;}
    /* SD media is removable: let the user re-mount without rebooting. */
    if(ui.page==UI_FILES && y<44 && x>=139 && x<246){
        audio_stop();
        media_refresh();
        open_files();
        return;
    }
    if(y<44 && x>=247 && ui.page!=UI_KEYBOARD){
        if(ui.page==UI_MENU)ui.page=UI_HOME;
        else if(ui.page==UI_WIFI_SCAN){end_wifi_scan();ui.page=UI_WIFI;}
        else if(ui.page==UI_KEYBOARD)ui.page=UI_WIFI;
        else if(ui.page==UI_FILE_VIEW){
            audio_stop();
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
            tv_pending=false;
#endif
            ui.page=UI_FILES;
        }
        else if(ui.page==UI_FILES && ui.sound_target)ui.page=UI_SOUND_PICK;
        else if(ui.page==UI_SOUND_PICK){
            audio_stop();
            ui.page=ui.sound_target==2?UI_TIMER:UI_ALARM;
            ui.sound_target=0;
        }
        else {if(ui.page==UI_CONTACT)audio_stop();ui.page=UI_MENU;}
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
        }else if(y>=188&&y<239){ui.sound_target=0;open_files();}
    }else if(ui.page==UI_CONTACT){
        if(y>=215 && x<160){
            if(x<80){
                if(audio_playing())audio_stop();
                else {
                    char key[20];audio_builtin_key(ui.contact_tune,key,sizeof key);
                    audio_play_key(key,true);
                }
            }else{
                audio_stop();
                ui.contact_tune=(ui.contact_tune+1)%AUDIO_BUILTIN_COUNT;
                store();
                char key[20];audio_builtin_key(ui.contact_tune,key,sizeof key);
                audio_play_key(key,true);
            }
        }
    }else if(ui.page==UI_ALARM){
        if(y<44 && x>=139 && x<248){
            ui.sound_target=1;ui.page=UI_SOUND_PICK;
            return;
        }
        if(y>=180&&y<207){
            int col=x<80?0:x<160?1:x<240?2:3;
            int step=col==0?-100:col==1?-1:col==2?1:100;
            int v=ui.alarm_bp+step;
            if(v<0)v+=10000;
            if(v>=10000)v-=10000;
            ui.alarm_bp=(uint16_t)v;
            /* A long hold can repeat at 65 ms. Defer flash/NVS writing
             * until release rather than writing hundreds of times. */
            alarm_adjust_dirty=true;
            last_alarm_adjust_us=esp_timer_get_time();
        }else if(y>=208){
            ui.alarm_enabled=!ui.alarm_enabled;
            store();
            alarm_adjust_dirty=false;
        }
    }else if(ui.page==UI_TIMER){
        if(y<44 && x>=139 && x<248){
            ui.sound_target=2;ui.page=UI_SOUND_PICK;
            return;
        }
        if(y>=180&&y<207){
            int col=x<80?0:x<160?1:x<240?2:3;
            int step=col==0?-10:col==1?-1:col==2?1:10;
            int v=ui.timer_bp+step;
            if(v<1)v=1;
            if(v>999)v=999;
            ui.timer_bp=(uint16_t)v;
            ui.timer_duration_ms=(uint32_t)v*8640u;
            store();
        }else if(y>=208){
            if(x<116){
                ui.timer_running=false;
                timer_deadline=0;
                ui.timer_bp=35;
                ui.timer_duration_ms=300000;
                ui.timer_seconds_left=300;
                store();
            }else if(ui.timer_running){
                ui.timer_running=false;
                timer_deadline=0;
            }else{
                timer_deadline=esp_timer_get_time()+
                        (int64_t)ui.timer_duration_ms*1000LL;
                ui.timer_seconds_left=(int)((ui.timer_duration_ms+999u)/1000u);
                ui.timer_running=true;
            }
        }
    }else if(ui.page==UI_SET_TIME){
        if(y>=50&&y<83 && x<=220){
            ui.use_ntp=!ui.use_ntp;
            if(ui.use_ntp)request_ntp_sync_now();
            else if(!ui.portal_enabled){
                stop_wifi_radio();
                ui.ntp_synced=false;
            }
            store();
            return;
        }
        if(ui.use_ntp){
            if(y>=183 && y<226 && x>=83 && x<237)
                request_ntp_sync_now();
            return;
        }
        /* Match the rendered bottom-left save and precisely spaced +/- keys. */
        if(y>=201&&y<238 && x>=20&&x<208){
            set_manual_time();
            return;
        }
        int row=-1;
        if(y>=106&&y<134)row=0;
        if(y>=164&&y<192)row=1;
        if(row>=0 && x>=20 && x<302){
            int col=(x-20)/94,localx=(x-20)%94;
            if(col>=0&&col<3){
                if(localx>=41&&localx<63)adjust_time_field(col,row,-1);
                else if(localx>=67&&localx<89)adjust_time_field(col,row,1);
            }
        }
    }else if(ui.page==UI_WIFI){
        if(y>=106&&y<146){
            if(x<107)begin_wifi_scan();
            else start_keyboard(x>=208);
        }else if(y>=156&&y<195)enable_portal();
    }else if(ui.page==UI_WIFI_SCAN){
        if(y>=51&&y<204 && !ui.wifi_scanning){
            int row=(y-51)/39;
            int index=ui.wifi_scan_page*4+row;
            if(row>=0&&row<4 && index<ui.wifi_scan_count){
                /* The scan returns the EXACT case-sensitive router SSID. */
                snprintf(ui.ssid,sizeof ui.ssid,"%s",ui.wifi_scan_ssids[index]);
                store();
                end_wifi_scan();
                ui.page=UI_WIFI;
                apply_wifi_credentials();
            }
        }else if(y>=210){
            if(x<112)begin_wifi_scan();
            else if(x<208){
                if(ui.wifi_scan_page>0)--ui.wifi_scan_page;
            }else if((ui.wifi_scan_page+1)*4<ui.wifi_scan_count)
                ++ui.wifi_scan_page;
        }
    }else if(ui.page==UI_KEYBOARD){
        if(y>=51 && y<213){
            int row=(y-51)/27;
            int col=(x-21)/40;
            int ix=row*7+col;
            if(x>=21 && x<301 && col>=0 && col<7 && ix>=0 && ix<42){
                size_t n=strlen(ui.keyboard_value);
                size_t max=ui.editing_password?64:32;
                if(n<max){
                    ui.keyboard_error=false;
                    char c="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-@!?"[ix];
                    if(ui.keyboard_lower&&isalpha((unsigned char)c))
                        c=(char)tolower((unsigned char)c);
                    ui.keyboard_value[n]=c;
                    ui.keyboard_value[n+1]=0;
                }
            }
        }else if(y>=214){
            if(x<112){
                size_t n=strlen(ui.keyboard_value);
                if(n)ui.keyboard_value[n-1]=0;
                ui.keyboard_error=false;
            }else if(x<209){
                ui.keyboard_lower=!ui.keyboard_lower;
            }else{
                if(ui.editing_password){
                    if(!valid_wifi_password(ui.keyboard_value)){
                        ui.keyboard_error=true;
                        return; /* Preserve the entire partially typed key. */
                    }
                    snprintf(ui.password,sizeof ui.password,"%s",ui.keyboard_value);
                }else
                    snprintf(ui.ssid,sizeof ui.ssid,"%.32s",ui.keyboard_value);
                store();
                ui.page=UI_WIFI;
                apply_wifi_credentials();
            }
        }
    }else if(ui.page==UI_LOOK){
        if(y>=89&&y<136){
            if(x>=163)ui.theme=3;
            else ui.theme=(ui.theme+1)%4;
            store();
        }
        if(y>=181&&y<225){
            int v=ui.brightness+(x<160?-10:10);
            if(v<3)v=3;
            if(v>100)v=100;
            ui.brightness=(uint8_t)v;store();
        }
    }else if(ui.page==UI_SOUND_PICK){
        if(y>=47 && y<197){
            int id=(y-47)/30;
            if(id>=0 && id<AUDIO_BUILTIN_COUNT && (y-47)%30<28){
                char key[20];audio_builtin_key(id,key,sizeof key);
                if(ui.sound_target==2)
                    save_audio_selection(ui.timer_sound,sizeof ui.timer_sound,key);
                else save_audio_selection(ui.alarm_sound,sizeof ui.alarm_sound,key);
                store();
                audio_play_key(key,false); /* preview selected melody */
            }
        }else if(y>=205){
            if(x<160){audio_stop();open_files();}
            else {
                audio_stop();
                ui.page=ui.sound_target==2?UI_TIMER:UI_ALARM;
                ui.sound_target=0;
            }
        }
    }else if(ui.page==UI_FILE_VIEW){
        if(audio_file_supported(ui.file_name) && y>=170 && y<215){
            char key[AUDIO_KEY_MAX];audio_sd_key(ui.file_name,key,sizeof key);
            if(x<108){
                if(audio_playing()){
                    audio_stop();
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
                    tv_pending=false;
#endif
                }else{
                    ESP_LOGI(TAG,"TF audio PLAY: %s",ui.file_name);
                    audio_play_key(key,false);
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
                    tv_request(ui.file_name,false);
#else
                    ESP_LOGW(TAG,"No composite: CONFIG_CLOCK_COMPOSITE_TF_TEST is disabled");
#endif
                }
            }else if(x<212){
                audio_stop();
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
                tv_pending=false;
#endif
                save_audio_selection(ui.alarm_sound,sizeof ui.alarm_sound,key);
                store();ui.sound_target=0;ui.page=UI_ALARM;
            }else{
                audio_stop();
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
                tv_pending=false;
#endif
                save_audio_selection(ui.timer_sound,sizeof ui.timer_sound,key);
                store();ui.sound_target=0;ui.page=UI_TIMER;
            }
        }
    }else if(ui.page==UI_FILES){
        if(y>=166&&y<213){
            if(x<106)media_select(-1,ui.file_name,sizeof ui.file_name);
            else if(x<212)media_select(1,ui.file_name,sizeof ui.file_name);
            else if(ui.file_name[0]){
                media_open(ui.file_preview,sizeof ui.file_preview);
                ui.page=UI_FILE_VIEW;
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
                if(media_has_bitmap())tv_request(ui.file_name,true);
#endif
            }
        }else if(!ui.sd_ready && y>=70){
            audio_stop();
            media_refresh();
            open_files();
        }
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
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
    ESP_LOGW(TAG,"COMPOSITE ENABLED: GPIO25 video starts at TF FILES > MP3/WAV/RTTTL > PLAY");
#else
    ESP_LOGW(TAG,"COMPOSITE DISABLED in this build: TF audio works, GPIO25 TV output will NOT start");
#endif
    i2c_init();
    rtc_load();
    ESP_ERROR_CHECK(screen_init());
    audio_init();
    start_wifi();
    last_touch_us=esp_timer_get_time();
    int last_day_bp=-1,previous_bp=-1,previous_day=-1;
    int last_timer_sec=-1;
    int last_brightness=-1;
    bool last_audio_state=false;
    int64_t touch_feedback_deadline=0;
    bool last_connected=false,last_portal=false,last_ntp_synced=false;
    bool last_connecting=false,last_scanning=false;
    uint16_t last_wifi_error=0;
    ESP_LOGI(TAG,"Boot reset reason=%d",(int)esp_reset_reason());
    /* First boot should immediately show the AP credentials. */
    ui.page=ui.portal_enabled?UI_WIFI:UI_HOME;
    screen_draw(&ui);
    while(true){
        int64_t now_us=esp_timer_get_time();
        int x=0,y=0;bool draw=false;
        if(screen_poll_touch(&x,&y,ui.page)){
            ui.touch_feedback=true;
            ui.touch_x=(int16_t)x;
            ui.touch_y=(int16_t)y;
            touch_feedback_deadline=now_us+500000LL;
            touch(x,y);
            draw=true;
        }
        if(ui.touch_feedback && now_us>=touch_feedback_deadline){
            ui.touch_feedback=false;
            draw=true;
        }
        if(alarm_adjust_dirty && now_us-last_alarm_adjust_us>=450000LL){
            store();
            alarm_adjust_dirty=false;
        }
        if(ui.portal_enabled && now_us>portal_deadline){
            disable_portal();draw=true;
        }else if(ui.portal_enabled){
            uint16_t left=(uint16_t)((portal_deadline-now_us+999999LL)/1000000LL);
            if(left!=ui.portal_seconds_left){
                ui.portal_seconds_left=left;
                if(ui.page==UI_WIFI)draw=true;
            }
        }
        if(wifi_scan_active && wifi_scan_done){
            receive_wifi_scan();
            draw=true;
        }
        /* Keep Wi-Fi fully stopped between NTP polls, not just in modem sleep.
         * The running ESP32 clock continues to tick with no network. */
        if(!ui.portal_enabled && !wifi_scan_active){
            if(!ui.use_ntp || !ui.ssid[0]){
                if(wifi_started)stop_wifi_radio();
            }else if(ntp_updated){
                ntp_updated=false;
                ui.ntp_synced=true;
                initial_sync_failures=0;
                time_t synced_epoch=time(NULL);
                rtc_save(synced_epoch);
                stop_wifi_radio();
                next_ntp_sync_us=now_us+NTP_INTERVAL_US;
                draw=true;
            }else if(wifi_session_started_us &&
                     now_us-wifi_session_started_us>=NTP_WINDOW_US){
                ESP_LOGW(TAG,"NTP Wi-Fi window timed out after 35s (Wi-Fi reason=%u)",
                         (unsigned)ui.wifi_disconnect_reason);
                stop_wifi_radio();
                /* A saved network may need a second try after boot. Retry
                 * twice promptly, then back off to the five-minute interval
                 * instead of wasting battery indefinitely. */
                if(initial_sync_failures<3)++initial_sync_failures;
                next_ntp_sync_us=now_us+
                    (!ui.ntp_synced && initial_sync_failures<3 ?
                     NTP_FIRST_RETRY_US:NTP_INTERVAL_US);
                draw=true;
            }else if(!wifi_started && now_us>=next_ntp_sync_us){
                start_ntp_radio();
                draw=true;
            }else if(wifi_started && !wifi_connected && wifi_retry_after_us &&
                     now_us>=wifi_retry_after_us){
                wifi_retry_after_us=0;
                esp_err_t e=esp_wifi_connect();
                if(e!=ESP_OK)wifi_retry_after_us=now_us+WIFI_RETRY_US;
                draw=true;
            }
        }
        time_t epoch=time(NULL);
        ui.time_valid=epoch>1700000000;
        if(ui.time_valid)localtime_r(&epoch,&ui.local);
        if(ui.timer_running){
            int64_t diff=timer_deadline-now_us;
            ui.timer_seconds_left=diff>0?(int)((diff+999999)/1000000):0;
            if(diff<=0){ui.timer_running=false;ui.timer_ringing=true;
                ring_started_us=now_us;
                ring_deadline=now_us+ALERT_DURATION_US;
                audio_play_key(ui.timer_sound,true);draw=true;}
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
                    ring_started_us=now_us;
                    ring_deadline=now_us+ALERT_DURATION_US;
                    audio_play_key(ui.alarm_sound,true);
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
        if((ui.alarm_ringing||ui.timer_ringing)&&now_us>=ring_deadline){
            silence();draw=true;
        }
        /* Brief software color inversion about once per second. Always return
         * to the normal palette between pulses, and after a tap or timeout. */
        bool alert_active=ui.alarm_ringing||ui.timer_ringing;
        bool pulse=alert_active && ring_started_us>0 &&
                   ((now_us-ring_started_us)%1100000LL)<300000LL;
        if(ui.alert_flash!=pulse){
            ui.alert_flash=pulse;
            draw=true;
        }
        bool audio_state=audio_playing();
        ui.audio_playing=audio_state;
        if(audio_state!=last_audio_state){
            last_audio_state=audio_state;
            if(ui.page==UI_CONTACT || ui.page==UI_FILE_VIEW)draw=true;
        }
        if(last_connected!=wifi_connected || last_portal!=ui.portal_enabled ||
           last_ntp_synced!=ui.ntp_synced ||
           last_connecting!=ui.wifi_connecting ||
           last_scanning!=ui.wifi_scanning ||
           last_wifi_error!=ui.wifi_disconnect_reason){
            last_connected=wifi_connected;
            last_portal=ui.portal_enabled;
            last_ntp_synced=ui.ntp_synced;
            last_connecting=ui.wifi_connecting;
            last_scanning=ui.wifi_scanning;
            last_wifi_error=ui.wifi_disconnect_reason;
            draw=true;
        }
        ui.wifi_connected=wifi_connected;
        int target_brightness=ui.brightness;
        /* Keep the portal credentials legible throughout setup. */
        /* An alarm must be visible even if idle dimming had engaged. */
        if(!ui.portal_enabled && !alert_active &&
           now_us-last_touch_us>40000000LL){
            target_brightness=(target_brightness*(ui.theme==3?12:25))/100;
            if(target_brightness<3)target_brightness=3;
        }
        if(last_brightness!=target_brightness){
            screen_set_brightness((uint8_t)target_brightness);
            last_brightness=target_brightness;
        }
#if CONFIG_CLOCK_COMPOSITE_TF_TEST
        /* Fully tear down I80 before the external video driver takes I2S0.
         * Never render LCD stripes while GPIO25 is the composite DAC. */
        if(tv_pending && !tv_video_active()){
            /* Draw a visible STOP target before freezing the LCD, even if
             * the decoder worker has not acknowledged PLAY yet. */
            bool was_playing=ui.audio_playing;
            if(!tv_bmp)ui.audio_playing=true;
            screen_draw(&ui);
            ui.audio_playing=was_playing;
            ESP_LOGW(TAG,"Releasing LCD and starting GPIO25 composite now");
            screen_suspend_for_composite();
            if(!tv_video_start(tv_filename,tv_bmp)){
                tv_pending=false;
                screen_resume_from_composite();
                screen_draw(&ui);
            }
            draw=false;
        }
        if(tv_video_active() && !tv_bmp && !audio_state &&
           now_us-tv_started_us>3000000LL)tv_pending=false;
        if(!tv_pending && tv_video_active()){
            tv_video_stop(); /* relinquish I2S0 + GPIO25 BEFORE LCD restart */
            screen_resume_from_composite();
            draw=true;
        }
        if(tv_video_active())tv_video_tick(now_us);
#endif
        if(draw)screen_draw(&ui);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
