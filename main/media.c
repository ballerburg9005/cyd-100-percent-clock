/* Local microSD root browser and contact photo. Decode uncompressed
 * 24-bit BMP, and common 32-bit GIMP exports. No JPEG decoder yet. */
#include "media.h"
#include "embedded_face.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"

#define MAX_FILES 64
static const char *TAG="clock_media";
static bool mounted, bus_ready, image_ready;
static int64_t retry_after_us;
static sdmmc_card_t *card;
static char file_names[MAX_FILES][64];
static int file_count,selected;
static char sd_status[48]="NOT CHECKED", photo_status[48]="NOT LOADED";
static uint16_t bitmap[MEDIA_BMP_WIDTH*MEDIA_BMP_HEIGHT];
static uint16_t u16(const uint8_t *p) {return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static bool ends(const char *name,const char *ext)
{
    size_t n=strlen(name),e=strlen(ext);
    if(n<e)return false;
    for(size_t i=0;i<e;i++)
        if(tolower((unsigned char)name[n-e+i])!=tolower((unsigned char)ext[i]))return false;
    return true;
}
static void scan(void)
{
    file_count=0;selected=0;
    DIR *dir=opendir("/sd");
    if(!dir){snprintf(sd_status,sizeof sd_status,"ROOT DIRECTORY ERROR");return;}
    struct dirent *ent;
    while((ent=readdir(dir)) && file_count<MAX_FILES){
        if(ent->d_name[0]=='.')continue;
        if(!ends(ent->d_name,".txt")&&!ends(ent->d_name,".bmp")&&!ends(ent->d_name,".mp3"))continue;
        snprintf(file_names[file_count++],64,"%.63s",ent->d_name);
    }
    closedir(dir);
    snprintf(sd_status,sizeof sd_status,"MOUNT OK / %d FILES",file_count);
}
bool media_mount(void)
{
    if(mounted)return true;
    /* A missing TF card is normal. Limit automatic retries to 3 seconds.
     * CONTACT retries automatically while shown, so hot insertion works. */
    const int64_t now=esp_timer_get_time();
    if(now<retry_after_us)return false;
    retry_after_us=now+3000000LL;
    if(!bus_ready){
        spi_bus_config_t bc={
            .mosi_io_num=23,.miso_io_num=19,.sclk_io_num=18,
            .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096
        };
        esp_err_t e=spi_bus_initialize(SPI2_HOST,&bc,SDSPI_DEFAULT_DMA);
        if(e!=ESP_OK && e!=ESP_ERR_INVALID_STATE){
            snprintf(sd_status,sizeof sd_status,"SPI: %s",esp_err_to_name(e));
            ESP_LOGW(TAG,"SD SPI bus: %s",esp_err_to_name(e));return false;
        }
        bus_ready=true;
    }
    sdmmc_host_t host=SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs=GPIO_NUM_5;
    slot.host_id=host.slot;
    esp_vfs_fat_sdmmc_mount_config_t cfg={
        .format_if_mount_failed=false,.max_files=3,.allocation_unit_size=16*1024
    };
    esp_err_t e=esp_vfs_fat_sdspi_mount("/sd",&host,&slot,&cfg,&card);
    if(e!=ESP_OK){
        snprintf(sd_status,sizeof sd_status,"MOUNT: %s",esp_err_to_name(e));
        ESP_LOGW(TAG,"No usable TF card: %s",esp_err_to_name(e));
        return false;
    }
    mounted=true;
    retry_after_us=0;
    scan();
    ESP_LOGI(TAG,"Mounted TF card, %d supported files",file_count);
    return true;
}
void media_refresh(void)
{
    /* Explicit recheck bypasses normal retry delay and re-reads FAT after
     * replacing an already mounted card. */
    image_ready=false;
    snprintf(photo_status,sizeof photo_status,"NOT LOADED");
    if(mounted){
        esp_err_t e=esp_vfs_fat_sdcard_unmount("/sd",card);
        if(e!=ESP_OK){
            snprintf(sd_status,sizeof sd_status,"UNMOUNT: %s",esp_err_to_name(e));
            ESP_LOGW(TAG,"SD refresh: %s",sd_status);
            return;
        }
        mounted=false;
        card=NULL;
    }
    file_count=0;
    selected=0;
    retry_after_us=0;
    media_mount();
}
bool media_is_mounted(void){return mounted;}
const char *media_sd_status(void){return sd_status;}
const char *media_photo_status(void){return photo_status;}
bool media_select(int direction,char *name,size_t len)
{
    if(!media_mount()){if(len)name[0]=0;return false;}
    if(file_count==0){if(len)name[0]=0;return true;}
    if(direction){
        selected+=direction;
        if(selected<0)selected=file_count-1;
        if(selected>=file_count)selected=0;
    }
    if(len)snprintf(name,len,"%s",file_names[selected]);
    return true;
}
static bool bmp_read(const char *path)
{
    image_ready=false;
    FILE *f=fopen(path,"rb");
    if(!f){
        snprintf(photo_status,sizeof photo_status,"FACE.BMP NOT FOUND");
        return false;
    }
    uint8_t h[54];
    size_t header_bytes=fread(h,1,sizeof h,f);
    if(header_bytes!=sizeof h){
        /* A changed file on a still-mounted card may have stale FAT data,
         * while an incomplete export may genuinely be shorter than 54 B.
         * Report the failing condition rather than blaming the dimensions. */
        if(ferror(f))
            snprintf(photo_status,sizeof photo_status,"BMP HEADER READ ERROR");
        else
            snprintf(photo_status,sizeof photo_status,"BMP TOO SHORT: %u/54",
                     (unsigned)header_bytes);
        ESP_LOGW(TAG,"%s: header read %u/54 bytes (ferror=%d)",
                 path,(unsigned)header_bytes,ferror(f)!=0);
        fclose(f);return false;
    }
    if(h[0]!='B'||h[1]!='M'){
        snprintf(photo_status,sizeof photo_status,"NOT BMP: %02X %02X",
                 (unsigned)h[0],(unsigned)h[1]);
        ESP_LOGW(TAG,"%s: not a Windows BMP (signature %02X %02X)",
                 path,(unsigned)h[0],(unsigned)h[1]);
        fclose(f);return false;
    }
    int32_t w=(int32_t)u32(h+18),height=(int32_t)u32(h+22);
    uint16_t bpp=u16(h+28);
    uint32_t compression=u32(h+30),offset=u32(h+10),dib=u32(h+14);
    int32_t abs_h=height<0?-height:height;
    if(w<=0||w>2048||abs_h<=0||abs_h>2048||dib<40||dib>124||
       u16(h+26)!=1 || offset<54 ||
       !((bpp==24 && compression==0) ||
         (bpp==32 && (compression==0 || compression==3 || compression==6)))){
        snprintf(photo_status,sizeof photo_status,"BMP FORMAT UNSUPPORTED");
        ESP_LOGW(TAG,"Unsupported BMP %s: %ldx%ld %u-bit compression=%lu DIB=%lu",
                 path,(long)w,(long)height,(unsigned)bpp,
                 (unsigned long)compression,(unsigned long)dib);
        fclose(f);return false;
    }
    if(bpp==32 && compression!=0){
        /* Standard Windows BITFIELDS/V4/V5 BGRA masks only. */
        uint8_t m[12];
        if(offset<66 || fseek(f,54,SEEK_SET)!=0 ||
           fread(m,1,sizeof m,f)!=sizeof m ||
           u32(m)!=0x00ff0000u || u32(m+4)!=0x0000ff00u ||
           u32(m+8)!=0x000000ffu){
            snprintf(photo_status,sizeof photo_status,"32-BIT MASK UNSUPPORTED");
            fclose(f);return false;
        }
    }
    uint32_t bytes=bpp/8;
    uint32_t stride=(((uint32_t)w*bytes)+3u)&~3u;
    uint8_t *row=malloc(stride);
    if(!row){
        snprintf(photo_status,sizeof photo_status,"OUT OF MEMORY");
        fclose(f);return false;
    }
    /* Preserve aspect ratio: center-crop to the thumbnail's portrait
     * 128:165 aspect. CONTACT's built-in image uses this same format. */
    int crop_w=w,crop_h=abs_h;
    if((int64_t)w*MEDIA_BMP_HEIGHT>(int64_t)abs_h*MEDIA_BMP_WIDTH){
        crop_w=(int)((int64_t)abs_h*MEDIA_BMP_WIDTH/MEDIA_BMP_HEIGHT);
        if(crop_w<1)crop_w=1;
    }else{
        crop_h=(int)((int64_t)w*MEDIA_BMP_HEIGHT/MEDIA_BMP_WIDTH);
        if(crop_h<1)crop_h=1;
    }
    int crop_x=(w-crop_w)/2,crop_y=(abs_h-crop_h)/2;
    bool ok=true;
    for(int y=0;y<MEDIA_BMP_HEIGHT && ok;++y){
        int sy=crop_y+(int)((int64_t)y*crop_h/MEDIA_BMP_HEIGHT);
        int disk_y=height>0?abs_h-1-sy:sy;
        if(fseek(f,(long)(offset+(uint32_t)disk_y*stride),SEEK_SET)!=0 ||
           fread(row,1,stride,f)!=stride){ok=false;break;}
        for(int x=0;x<MEDIA_BMP_WIDTH;++x){
            int sx=crop_x+(int)((int64_t)x*crop_w/MEDIA_BMP_WIDTH);
            uint8_t b=row[sx*bytes],g=row[sx*bytes+1],r=row[sx*bytes+2];
            /* The ST7789 is configured BGR. Swap R/B on this source
             * bitmap to make reds display red, rather than blue. */
            bitmap[y*MEDIA_BMP_WIDTH+x]=((b&0xf8)<<8)|((g&0xfc)<<3)|(r>>3);
        }
    }
    free(row);fclose(f);
    image_ready=ok;
    snprintf(photo_status,sizeof photo_status,ok?"BMP %u-BIT OK":"BMP READ ERROR",
             (unsigned)bpp);
    if(ok)ESP_LOGI(TAG,"Loaded %s: %ld x %ld, %u-bit",path,
                   (long)w,(long)abs_h,(unsigned)bpp);
    return ok;
}
bool media_open(char *preview,size_t len)
{
    if(len)preview[0]=0;
    image_ready=false;
    if(!mounted||file_count==0)return false;
    char path[140];snprintf(path,sizeof path,"/sd/%s",file_names[selected]);
    if(ends(path,".bmp")){
        bool ok=bmp_read(path);
        if(len)snprintf(preview,len,ok?"BMP IMAGE: %dx%d THUMBNAIL":"UNSUPPORTED BMP",
                        MEDIA_BMP_WIDTH,MEDIA_BMP_HEIGHT);
        return ok;
    }
    if(ends(path,".mp3")){
        if(len)snprintf(preview,len,"MP3 FILE\nPLAYBACK NOT AVAILABLE IN THIS BUILD");
        return true;
    }
    FILE *f=fopen(path,"rb");if(!f)return false;
    size_t n=0;int col=0,ch;
    while((ch=fgetc(f))!=EOF && n+2<len){
        if(ch=='\r')continue;
        if(ch=='\n'||col>=48){preview[n++]='\n';col=0;if(ch=='\n')continue;}
        preview[n++]=(ch=='\t')?' ':(ch>=32&&ch<127?(char)ch:'?');
        col++;
    }
    preview[n]=0;fclose(f);return true;
}
bool media_photo(void)
{
    /* Compiled into the firmware from root face.bmp. CONTACT does not
     * attempt to access a removable SD card. TF FILES still works as usual. */
    memcpy(bitmap,clock_builtin_face,sizeof bitmap);
    image_ready=true;
    snprintf(photo_status,sizeof photo_status,"BUILT-IN PHOTO");
    return true;
}
const uint16_t *media_bitmap(void) {return bitmap;}
bool media_has_bitmap(void) {return image_ready;}
int media_index(void) {return selected;}
int media_count(void) {return file_count;}
