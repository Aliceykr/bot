#include "lcd.h"

static uint8_t FontBuf[130]; // 字库缓冲

void ZK_command(uint8_t dat)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        LCD_SCLK_Clr();
        if (dat & 0x80) LCD_MOSI_Set();
        else            LCD_MOSI_Clr();
        LCD_SCLK_Set();
        dat <<= 1;
    }
}

uint8_t get_data_from_ROM(void)
{
    uint8_t i;
    uint8_t ret_data = 0;
    for (i = 0; i < 8; i++) {
        LCD_SCLK_Clr();
        ret_data <<= 1;
        if (ZK_MISO) ret_data++;
        LCD_SCLK_Set();
    }
    return ret_data;
}

void get_n_bytes_data_from_ROM(uint8_t AddrHigh, uint8_t AddrMid, uint8_t AddrLow, uint8_t *pBuff, uint8_t DataLen)
{
    uint8_t i;
    ZK_CS_Clr();
    ZK_command(0x03);
    ZK_command(AddrHigh);
    ZK_command(AddrMid);
    ZK_command(AddrLow);
    for (i = 0; i < DataLen; i++)
        *(pBuff + i) = get_data_from_ROM();
    ZK_CS_Set();
}

void Display_GB2312(uint16_t x, uint16_t y, uint8_t zk_num, uint16_t fc, uint16_t bc)
{
    uint8_t i, k;
    switch (zk_num) {
    case 1: // 12x12
        LCD_Address_Set(x, y, x + 15, y + 11);
        for (i = 0; i < 24; i++)
            for (k = 0; k < 8; k++)
                LCD_WR_DATA((FontBuf[i] & (0x80 >> k)) ? fc : bc);
        break;
    case 2: // 15x16
        LCD_Address_Set(x, y, x + 15, y + 15);
        for (i = 0; i < 32; i++)
            for (k = 0; k < 8; k++)
                LCD_WR_DATA((FontBuf[i] & (0x80 >> k)) ? fc : bc);
        break;
    case 3: // 24x24
        LCD_Address_Set(x, y, x + 23, y + 23);
        for (i = 0; i < 72; i++)
            for (k = 0; k < 8; k++)
                LCD_WR_DATA((FontBuf[i] & (0x80 >> k)) ? fc : bc);
        break;
    case 4: // 32x32
        LCD_Address_Set(x, y, x + 31, y + 31);
        for (i = 0; i < 128; i++)
            for (k = 0; k < 8; k++)
                LCD_WR_DATA((FontBuf[i] & (0x80 >> k)) ? fc : bc);
        break;
    }
}

void Display_GB2312_String(uint16_t x, uint16_t y, uint8_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc)
{
    uint8_t i = 0;
    uint8_t AddrHigh, AddrMid, AddrLow;
    uint32_t FontAddr, BaseAdd;
    uint8_t n, d;
    switch (zk_num) {
    case 1: BaseAdd = 0x00;     n = 24; d = 12; break;
    case 2: BaseAdd = 0x2C9D0;  n = 32; d = 16; break;
    case 3: BaseAdd = 0x68190;  n = 72; d = 24; break;
    case 4: BaseAdd = 0xEDF00;  n = 128; d = 32; break;
    default: return;
    }
    while (text[i] > 0x00) {
        if (((text[i] >= 0xA1) && (text[i] <= 0xA9)) && (text[i + 1] >= 0xA1)) {
            FontAddr = (text[i] - 0xA1) * 94 + (text[i + 1] - 0xA1);
            FontAddr = (uint32_t)(FontAddr * n + BaseAdd);
        } else if (((text[i] >= 0xB0) && (text[i] <= 0xF7)) && (text[i + 1] >= 0xA1)) {
            FontAddr = (text[i] - 0xB0) * 94 + (text[i + 1] - 0xA1) + 846;
            FontAddr = (uint32_t)(FontAddr * n + BaseAdd);
        } else {
            x += d; i += 2; continue;
        }
        AddrHigh = (FontAddr & 0xff0000) >> 16;
        AddrMid  = (FontAddr & 0xff00) >> 8;
        AddrLow  = FontAddr & 0xff;
        get_n_bytes_data_from_ROM(AddrHigh, AddrMid, AddrLow, FontBuf, n);
        Display_GB2312(x, y, zk_num, fc, bc);
        x += d;
        i += 2;
    }
}

void Display_Asc(uint16_t x, uint16_t y, uint8_t zk_num, uint16_t fc, uint16_t bc)
{
    uint8_t i, k;
    switch (zk_num) {
    case 1: LCD_Address_Set(x, y, x+7, y+7);   for(i=0;i<7;i++)  for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 2: LCD_Address_Set(x, y, x+7, y+7);   for(i=0;i<8;i++)  for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 3: LCD_Address_Set(x, y, x+7, y+11);  for(i=0;i<12;i++) for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 4: LCD_Address_Set(x, y, x+7, y+15);  for(i=0;i<16;i++) for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 5: LCD_Address_Set(x, y, x+15, y+24); for(i=0;i<48;i++) for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 6: LCD_Address_Set(x, y, x+15, y+31); for(i=0;i<64;i++) for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    }
}

void Display_Asc_String(uint16_t x, uint16_t y, uint16_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc)
{
    uint8_t i = 0;
    uint8_t AddrHigh, AddrMid, AddrLow;
    uint32_t FontAddr, BaseAdd;
    uint8_t n, d;
    switch (zk_num) {
    case 1: BaseAdd=0x1DDF80; n=8;  d=6;  break;
    case 2: BaseAdd=0x1DE280; n=8;  d=8;  break;
    case 3: BaseAdd=0x1DBE00; n=12; d=6;  break;
    case 4: BaseAdd=0x1DD780; n=16; d=8;  break;
    case 5: BaseAdd=0x1DFF00; n=48; d=12; break;
    case 6: BaseAdd=0x1E5A50; n=64; d=16; break;
    default: return;
    }
    while (text[i] > 0x00) {
        if (text[i] >= 0x20 && text[i] <= 0x7E) {
            FontAddr = (uint32_t)((text[i] - 0x20) * n + BaseAdd);
            AddrHigh = (FontAddr & 0xff0000) >> 16;
            AddrMid  = (FontAddr & 0xff00) >> 8;
            AddrLow  = FontAddr & 0xff;
            get_n_bytes_data_from_ROM(AddrHigh, AddrMid, AddrLow, FontBuf, n);
            Display_Asc(x, y, zk_num, fc, bc);
        }
        i++;
        x += d;
    }
}

void Display_Arial_TimesNewRoman(uint16_t x, uint16_t y, uint8_t zk_num, uint16_t fc, uint16_t bc)
{
    uint8_t i, k;
    switch (zk_num) {
    case 1: LCD_Address_Set(x,y,x+15,y+12); for(i=2;i<26;i++)  for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 2: LCD_Address_Set(x,y,x+15,y+17); for(i=2;i<34;i++)  for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 3: LCD_Address_Set(x,y,x+23,y+23); for(i=2;i<74;i++)  for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    case 4: LCD_Address_Set(x,y,x+31,y+31); for(i=2;i<130;i++) for(k=0;k<8;k++) LCD_WR_DATA((FontBuf[i]&(0x80>>k))?fc:bc); break;
    }
}

void Display_Arial_String(uint16_t x, uint16_t y, uint16_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc)
{
    uint8_t i = 0;
    uint8_t AddrHigh, AddrMid, AddrLow;
    uint32_t FontAddr, BaseAdd;
    uint8_t n, d;
    switch (zk_num) {
    case 1: BaseAdd=0x1DC400; n=26;  d=8;  break;
    case 2: BaseAdd=0x1DE580; n=34;  d=12; break;
    case 3: BaseAdd=0x1E22D0; n=74;  d=16; break;
    case 4: BaseAdd=0x1E99D0; n=130; d=24; break;
    default: return;
    }
    while (text[i] > 0x00) {
        if (text[i] >= 0x20 && text[i] <= 0x7E) {
            FontAddr = (uint32_t)((text[i] - 0x20) * n + BaseAdd);
            AddrHigh = (FontAddr & 0xff0000) >> 16;
            AddrMid  = (FontAddr & 0xff00) >> 8;
            AddrLow  = FontAddr & 0xff;
            get_n_bytes_data_from_ROM(AddrHigh, AddrMid, AddrLow, FontBuf, n);
            Display_Arial_TimesNewRoman(x, y, zk_num, fc, bc);
        }
        i++;
        x += d;
    }
}

void Display_TimesNewRoman_String(uint16_t x, uint16_t y, uint16_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc)
{
    uint8_t i = 0;
    uint8_t AddrHigh, AddrMid, AddrLow;
    uint32_t FontAddr, BaseAdd;
    uint8_t n, d;
    switch (zk_num) {
    case 1: BaseAdd=0x1DCDC0; n=26;  d=8;  break;
    case 2: BaseAdd=0x1DF240; n=34;  d=12; break;
    case 3: BaseAdd=0x1E3E90; n=74;  d=16; break;
    case 4: BaseAdd=0x1ECA90; n=130; d=24; break;
    default: return;
    }
    while (text[i] > 0x00) {
        if (text[i] >= 0x20 && text[i] <= 0x7E) {
            FontAddr = (uint32_t)((text[i] - 0x20) * n + BaseAdd);
            AddrHigh = (FontAddr & 0xff0000) >> 16;
            AddrMid  = (FontAddr & 0xff00) >> 8;
            AddrLow  = FontAddr & 0xff;
            get_n_bytes_data_from_ROM(AddrHigh, AddrMid, AddrLow, FontBuf, n);
            Display_Arial_TimesNewRoman(x, y, zk_num, fc, bc);
        }
        i++;
        x += d;
    }
}
