/* Optional microSD root-file browser. No multimedia decoders or network
 * dependencies: TXT preview and uncompressed 24-bit BMP only.
 * MP3 files can be listed, NOT played, in this low-memory build.
 */
#include "media.h"
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
static uint16_t bitmap[MEDIA_BMP_SIZE*MEDIA_BMP_SIZE];
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
    if(!dir)return;
    struct dirent *ent;
    while((ent=readdir(dir)) && file_count<MAX_FILES){
        if(ent->d_name[0]=='.')continue;
        if(!ends(ent->d_name,".txt")&&!ends(ent->d_name,".bmp")&&!ends(ent->d_name,".mp3"))continue;
        snprintf(file_names[file_count++],64,"%.63s",ent->d_name);
    }
    closedir(dir);
}
bool media_mount(void)
{
    if(mounted)return true;
    /* A missing TF card is normal: retry at most every 15 seconds.
       Repeated taps must not cause a flood of SD initialization errors. */
    const int64_t now=esp_timer_get_time();
    if(now<retry_after_us)return false;
    retry_after_us=now+15000000LL;
    if(!bus_ready){
        spi_bus_config_t bc={
            .mosi_io_num=23,.miso_io_num=19,.sclk_io_num=18,
            .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096
        };
        esp_err_t e=spi_bus_initialize(SPI2_HOST,&bc,SDSPI_DEFAULT_DMA);
        if(e!=ESP_OK && e!=ESP_ERR_INVALID_STATE){
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
        ESP_LOGW(TAG,"No usable TF card: %s",esp_err_to_name(e));
        return false;
    }
    mounted=true;
    retry_after_us=0;
    scan();
    ESP_LOGI(TAG,"Mounted TF card, %d supported files",file_count);
    return true;
}
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
    FILE *f=fopen(path,"rb");if(!f)return false;
    uint8_t h[54];
    if(fread(h,1,54,f)!=54 || h[0]!='B'||h[1]!='M' || u32(h+30)!=0 ||
       u16(h+28)!=24 || u32(h+18)>2048 || (int32_t)u32(h+18)<=0) {
        fclose(f);return false;
    }
    int w=(int)u32(h+18),height=(int32_t)u32(h+22);
    int abs_h=height<0?-height:height;
    if(abs_h<=0||abs_h>2048){fclose(f);return false;}
    uint32_t offset=u32(h+10),stride=((w*3+3)/4)*4;
    uint8_t *row=malloc(stride);
    if(!row){fclose(f);return false;}
    bool ok=true;
    for(int y=0;y<MEDIA_BMP_SIZE && ok;++y){
        int sy=y*abs_h/MEDIA_BMP_SIZE;
        int disk_y=height>0?abs_h-1-sy:sy;
        if(fseek(f,(long)(offset+disk_y*stride),SEEK_SET)!=0 ||
           fread(row,1,stride,f)!=stride){ok=false;break;}
        for(int x=0;x<MEDIA_BMP_SIZE;++x){
            int sx=x*w/MEDIA_BMP_SIZE;
            uint8_t b=row[sx*3],g=row[sx*3+1],r=row[sx*3+2];
            bitmap[y*MEDIA_BMP_SIZE+x]=((r&0xf8)<<8)|((g&0xfc)<<3)|(b>>3);
        }
    }
    free(row);fclose(f);
    image_ready=ok;
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
                        MEDIA_BMP_SIZE,MEDIA_BMP_SIZE);
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
    if(!media_mount())return false;
    /* The maker's image is explicit and local; never bundle personal photos. */
    return bmp_read("/sd/face.bmp");
}
const uint16_t *media_bitmap(void) {return bitmap;}
bool media_has_bitmap(void) {return image_ready;}
int media_index(void) {return selected;}
int media_count(void) {return file_count;}
