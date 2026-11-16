/* Embedded monochrome raster subset 0-9, '.' and '%' of DejaVu Sans.
 * Larger type is typographic, NOT seven-segment numerals. Glyph bitmaps are
 * RLE/base64 compressed to ~2.5 KiB in flash and 4 KiB RAM after decoding.
 * DejaVu typeface derives from Bitstream Vera (permissive font license).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define BIG_FONT_H 58
#define BIG_FONT_PIXELS 32712
static const uint8_t big_width[12]={47,47,47,47,47,47,47,47,47,47,24,70};
static uint8_t big_bitmap[(BIG_FONT_PIXELS+7)/8];
static const char big_rle64[] =
    "EwkjDx4TGxUZFxcZFQoHChMJCwkSCA0IEQgPCBAIDwgPCBEIDggRCA4HEwcNCBMIDAgUBwwHFQcMBxUHDAcVBwsIFQgKCBUI"
    "CggWBwoHFwcKBxcHCgcXBwoHFwcKBxcHCgcXBwoHFwcKBxcHCgcXBwoHFwcKBxcHCgcXBwoIFgcKCBUICggVCAsHFQcMBxUH"
    "DAcVBwwIFAcMCBMIDQcTBw4IEQgOCBEIDwgPCBAIDwgRCA0IEgkLCRMKBwoVGRcXGRUbEx4PIwmEAQkiDR0SGxQbFBsUGwsC"
    "BxsHBgcbAgsHKAcoBygHKAcoBygHKAcoBygHKAcoBygHKAcoBygHKAcoBygHKAcoBygHKAcoBygHKAcoBygHKAcoBygHKAco"
    "BygHKAcoBygHKAcoBygHHB8QHxAfEB8QHxAf0wEKIRIaFxYaFB0SHhELCAwQCA0KEAYRCQ8EFAgPAhcIDgEYCCcIKAcoBygH"
    "KAcoBygHJwgnCCYIJwgmCCcIJggmCCYJJQklCSUJJQklCSYIJggmCCYIJggmCCYIJggmCSUJJQklCSUJJQklCSUJJSMMIwwj"
    "DCMMIwwjowEMHhUXGhUcEx0SHhEJCgwQBRAKEAIVCScIKAcoCCcIKAcoBygHJwgnBygHJwgmCCYIJAoaFBsSHRAfEh0UGxUl"
    "CycJJwknCScIKAcoCCgHKAcoBygHKAcoBycIJwgnBycIDQEYCQ0CFgkOBRELDgkKDQ8fEB4RHBQaGBQgC40BCSUKJAskCyMM"
    "Ig0iDSEGAQcgBwEHIAYCBx8GAwceBwMHHgYEBx0GBQccBwUHHAYGBxsGBwcaBwcHGgYIBxkHCAcZBgkHGAYKBxcHCgcXBgsH"
    "FgYMBxUHDAcVBg0HFAYOBxMHDgcTBg8HEgYQBxEHEAcRBhEHEAcRBxAGEgcQJwgnCCcIJwgnCCcgBygHKAcoBygHKAcoBygH"
    "KAcoBygHKAcoB9ABHRIdEh0SHRIdEh0SBygHKAcoBygHKAcoBygHKAcoBygHKAcoBwIJHRYZGBcZFhsUHBMHCQ0SBA8KEgET"
    "CicJJwgnCCgIJwgoBygHKAgnCCcIJwgnCCcHKAcnCCcIJgkmCCYJDgEVCg8EEQsPCAoNEB4RHRIbFBoXFR8NuQEKIRIbFhgX"
    "FhkVGhQMCAcTCg4EEwgTARIIJggnBycIJwcnCCcHKAcnCCcHKAcICRcHBQ8UBwQSEQgCFRAIARcPIQ4QBwsNDgsKDA0NCQwM"
    "DwkLCxEICwoTBwsKEwgKCRQICgkVBwoJFQcLCBUHCwgVBwsIFQcLCBUHCwgVBwwHFQcMBxUHDAcUCAwIEwgNBxMHDggRCA8I"
    "DwkPCQ0JEQkLCRMKBwsTGxUZGBYaEx4PIwl2IwwjDCMMIwwiDSInBycIJwgnBycIJwgmCCcIJwcnCCcIJwcnCCcHJwgnCCcH"
    "JwgnCCYIJwgnBycIJwgnBycIJwcnCCcIJwcnCCcIJggnCCcHJwgnCCcHJwgnBycIJwgnBycIJwgnBycIJwfpAQohERwVGBkV"
    "GxMdEQwHDBAJDQkPCQ8JDggRCA4IEQgOBxMIDAgTCAwIEwgMCBMIDAgTCAwIEwgNBxMHDgcTBw4IEQgPBxEHEAgPCBEIDQgT"
    "CgcKFRkYFRwRHRMaFxYbEwsHCxEJDQkPCQ8JDggRCA0IEwgMBxUHCwgVCAoIFQgKBxcHCgcXBwoHFwcKBxcHCggVCAoIFQgK"
    "CBUICwgTCAwJEQkNCQ8KDQoNCg8MBwwQHxEdFBkXFxoTIAuCAQkjDx4TGhcXGRUbEwsHChMJCwkRCQ4IEAgQBw8IEQgOBxMH"
    "DQgTCAwIFAcMBxUHDAcVCAsHFQgLBxUICwcVCAsHFQgLBxUICwcVCQoHFQkKCBQJCggTCgsHEwoLCBELCwkQCwwJDgwMCgsO"
    "DQsHEA4ZAQcPFwEIEBUCCBETAwcUDwUHFwkIBygHJwgnBygHJwgnBycIJwcnCCYIEgETCBMEDgoTBwgMFBoVGRYXGBYbEiEK"
    "fQgQCBAIEAgQCBAIEAgQCBAIrQkHHQYZDRkGGQ8YBRkRFgYYExQGGAgFCBMGGAcHBxIGGAcJBxEFGQYLBhAGGQYLBg8GGQYN"
    "Bg4GGQYNBg0GGgYNBg0FGwYNBgwGGwYNBgsGHAYNBgsGHAYNBgoGHQYNBgoFHgYNBgkGHwYLBgkGIAYLBgkGIAcJBwgGIgcH"
    "BwkFIwgFCAgGJBMIBiYRCQYnDwkGKQ0KBQwHGgcMBgoMKQYJDygGCBEmBggTJQUJBwUIIwYIBwgHIQYJBgoGIQYIBgsGIAYJ"
    "BgwGHwUKBgwGHgYKBQ0GHQYKBg0GHQYKBg0GHAYLBg0GHAUMBg0GGwYMBg0GGgYNBg0GGgUPBgwGGQYPBgwGGQUQBgsGGQYR"
    "BgoGGAYSBwgHGAUUBwUIGAYUExgGFhEZBhcPGQYaDBoFHQeZAQ==";
static int big_base64(char c)
{
    if(c>='A'&&c<='Z')return c-'A';
    if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52;
    if(c=='+')return 62;
    if(c=='/')return 63;
    return -1;
}
static void big_font_init(void)
{
    uint32_t accum=0, run=0, pixel=0;
    unsigned accbits=0, shift=0;
    int color=0;
    for(const char *p=big_rle64;*p && pixel<BIG_FONT_PIXELS;p++){
        int v=big_base64(*p);
        if(v<0)continue;
        accum=(accum<<6)|(unsigned)v;
        accbits+=6;
        while(accbits>=8 && pixel<BIG_FONT_PIXELS){
            accbits-=8;
            uint8_t byte=(uint8_t)((accum>>accbits)&255);
            run |= (uint32_t)(byte&127)<<shift;
            if(byte&128){shift+=7;continue;}
            if(run>BIG_FONT_PIXELS-pixel)run=BIG_FONT_PIXELS-pixel;
            if(color)for(uint32_t i=0;i<run;++i){
                uint32_t bit=pixel+i;
                big_bitmap[bit/8]|=1u<<(7u-(bit&7u));
            }
            pixel+=run;
            color=!color;run=0;shift=0;
        }
    }
}
static int big_index(char c)
{
    if(c>='0'&&c<='9')return c-'0';
    if(c=='.')return 10;
    if(c=='%')return 11;
    return -1;
}
static uint32_t big_start(int index)
{
    uint32_t offset=0;
    for(int i=0;i<index;++i)offset+=58u*big_width[i];
    return offset;
}
static int big_pixel(int index,int x,int y)
{
    uint32_t bit=big_start(index)+(uint32_t)y*big_width[index]+x;
    return (big_bitmap[bit/8]>>(7u-(bit&7u)))&1u;
}
