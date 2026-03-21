#include "lcd.h"
#include "lcdfont.h"

// ================================================================
// LCD 底层 - GPIO & SPI
// ================================================================

static void lcd_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void LCD_GPIO_Init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<LCD_MOSI_PIN)|(1ULL<<LCD_SCLK_PIN)|(1ULL<<LCD_CS_PIN)|(1ULL<<LCD_DC_PIN)|(1ULL<<LCD_BLK_PIN);
    io_conf.pull_down_en = 0; io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << LCD_MISO_PIN);
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
    gpio_set_level(LCD_CS_PIN,1); gpio_set_level(LCD_DC_PIN,1);
    gpio_set_level(LCD_BLK_PIN,1); gpio_set_level(LCD_SCLK_PIN,1); gpio_set_level(LCD_MOSI_PIN,1);
}

void LCD_Writ_Bus(uint8_t dat)
{
    uint8_t i;
    LCD_CS_Clr();
    for (i=0;i<8;i++) { LCD_SCLK_Clr(); if(dat&0x80) LCD_MOSI_Set(); else LCD_MOSI_Clr(); LCD_SCLK_Set(); dat<<=1; }
    LCD_CS_Set();
}

void LCD_WR_DATA8(uint8_t dat)  { LCD_Writ_Bus(dat); }
void LCD_WR_DATA(uint16_t dat)  { LCD_Writ_Bus(dat>>8); LCD_Writ_Bus(dat); }
void LCD_WR_REG(uint8_t dat)    { LCD_DC_Clr(); LCD_Writ_Bus(dat); LCD_DC_Set(); }

void LCD_Address_Set(uint16_t x1,uint16_t y1,uint16_t x2,uint16_t y2)
{ LCD_WR_REG(0x2a);LCD_WR_DATA(x1);LCD_WR_DATA(x2); LCD_WR_REG(0x2b);LCD_WR_DATA(y1);LCD_WR_DATA(y2); LCD_WR_REG(0x2c); }

void LCD_Init(void)
{
    LCD_GPIO_Init(); LCD_BLK_Set(); lcd_delay_ms(100);
    LCD_WR_REG(0x11); lcd_delay_ms(120);
    LCD_WR_REG(0xB1);LCD_WR_DATA8(0x05);LCD_WR_DATA8(0x3C);LCD_WR_DATA8(0x3C);
    LCD_WR_REG(0xB2);LCD_WR_DATA8(0x05);LCD_WR_DATA8(0x3C);LCD_WR_DATA8(0x3C);
    LCD_WR_REG(0xB3);LCD_WR_DATA8(0x05);LCD_WR_DATA8(0x3C);LCD_WR_DATA8(0x3C);LCD_WR_DATA8(0x05);LCD_WR_DATA8(0x3C);LCD_WR_DATA8(0x3C);
    LCD_WR_REG(0xB4);LCD_WR_DATA8(0x03);
    LCD_WR_REG(0xC0);LCD_WR_DATA8(0x28);LCD_WR_DATA8(0x08);LCD_WR_DATA8(0x04);
    LCD_WR_REG(0xC1);LCD_WR_DATA8(0xC0);
    LCD_WR_REG(0xC2);LCD_WR_DATA8(0x0D);LCD_WR_DATA8(0x00);
    LCD_WR_REG(0xC3);LCD_WR_DATA8(0x8D);LCD_WR_DATA8(0x2A);
    LCD_WR_REG(0xC4);LCD_WR_DATA8(0x8D);LCD_WR_DATA8(0xEE);
    LCD_WR_REG(0xC5);LCD_WR_DATA8(0x1A);
    LCD_WR_REG(0x36);
    if(USE_HORIZONTAL==0)LCD_WR_DATA8(0x00);
    else if(USE_HORIZONTAL==1)LCD_WR_DATA8(0xC0);
    else if(USE_HORIZONTAL==2)LCD_WR_DATA8(0x70);
    else LCD_WR_DATA8(0xA0);
    LCD_WR_REG(0xE0);LCD_WR_DATA8(0x04);LCD_WR_DATA8(0x22);LCD_WR_DATA8(0x07);LCD_WR_DATA8(0x0A);LCD_WR_DATA8(0x2E);LCD_WR_DATA8(0x30);LCD_WR_DATA8(0x25);LCD_WR_DATA8(0x2A);LCD_WR_DATA8(0x28);LCD_WR_DATA8(0x26);LCD_WR_DATA8(0x2E);LCD_WR_DATA8(0x3A);LCD_WR_DATA8(0x00);LCD_WR_DATA8(0x01);LCD_WR_DATA8(0x03);LCD_WR_DATA8(0x13);
    LCD_WR_REG(0xE1);LCD_WR_DATA8(0x04);LCD_WR_DATA8(0x16);LCD_WR_DATA8(0x06);LCD_WR_DATA8(0x0D);LCD_WR_DATA8(0x2D);LCD_WR_DATA8(0x26);LCD_WR_DATA8(0x23);LCD_WR_DATA8(0x27);LCD_WR_DATA8(0x27);LCD_WR_DATA8(0x25);LCD_WR_DATA8(0x2D);LCD_WR_DATA8(0x3B);LCD_WR_DATA8(0x00);LCD_WR_DATA8(0x01);LCD_WR_DATA8(0x04);LCD_WR_DATA8(0x13);
    LCD_WR_REG(0x3A);LCD_WR_DATA8(0x05);
    LCD_WR_REG(0x29);
}

// ================================================================
// 绘图函数
// ================================================================

void LCD_Fill(uint16_t xsta,uint16_t ysta,uint16_t xend,uint16_t yend,uint16_t color)
{ uint16_t i,j; LCD_Address_Set(xsta,ysta,xend-1,yend-1); for(i=ysta;i<yend;i++) for(j=xsta;j<xend;j++) LCD_WR_DATA(color); }

void LCD_DrawPoint(uint16_t x,uint16_t y,uint16_t color)
{ LCD_Address_Set(x,y,x,y); LCD_WR_DATA(color); }

void LCD_DrawLine(uint16_t x1,uint16_t y1,uint16_t x2,uint16_t y2,uint16_t color)
{
    uint16_t t; int xerr=0,yerr=0,delta_x,delta_y,distance,incx,incy,uRow,uCol;
    delta_x=x2-x1; delta_y=y2-y1; uRow=x1; uCol=y1;
    if(delta_x>0)incx=1; else if(delta_x==0)incx=0; else{incx=-1;delta_x=-delta_x;}
    if(delta_y>0)incy=1; else if(delta_y==0)incy=0; else{incy=-1;delta_y=-delta_y;}
    distance=(delta_x>delta_y)?delta_x:delta_y;
    for(t=0;t<distance+1;t++){
        LCD_DrawPoint(uRow,uCol,color); xerr+=delta_x; yerr+=delta_y;
        if(xerr>distance){xerr-=distance;uRow+=incx;}
        if(yerr>distance){yerr-=distance;uCol+=incy;}
    }
}

void LCD_DrawRectangle(uint16_t x1,uint16_t y1,uint16_t x2,uint16_t y2,uint16_t color)
{ LCD_DrawLine(x1,y1,x2,y1,color); LCD_DrawLine(x1,y1,x1,y2,color); LCD_DrawLine(x1,y2,x2,y2,color); LCD_DrawLine(x2,y1,x2,y2,color); }

void Draw_Circle(uint16_t x0,uint16_t y0,uint8_t r,uint16_t color)
{
    int a=0,b=r;
    while(a<=b){
        LCD_DrawPoint(x0-b,y0-a,color);LCD_DrawPoint(x0+b,y0-a,color);
        LCD_DrawPoint(x0-a,y0+b,color);LCD_DrawPoint(x0-a,y0-b,color);
        LCD_DrawPoint(x0+b,y0+a,color);LCD_DrawPoint(x0+a,y0-b,color);
        LCD_DrawPoint(x0+a,y0+b,color);LCD_DrawPoint(x0-b,y0+a,color);
        a++; if((a*a+b*b)>(r*r))b--;
    }
}

// ================================================================
// 字符显示
// ================================================================

void LCD_ShowChinese(uint16_t x,uint16_t y,uint8_t *s,uint16_t fc,uint16_t bc,uint8_t sizey,uint8_t mode)
{
    while(*s!=0){
        if(sizey==12)LCD_ShowChinese12x12(x,y,s,fc,bc,sizey,mode);
        else if(sizey==16)LCD_ShowChinese16x16(x,y,s,fc,bc,sizey,mode);
        else if(sizey==24)LCD_ShowChinese24x24(x,y,s,fc,bc,sizey,mode);
        else if(sizey==32)LCD_ShowChinese32x32(x,y,s,fc,bc,sizey,mode);
        else return;
        s+=2; x+=sizey;
    }
}

#define _SHOW_CN_FONT(fname, ftype, fsize) \
void fname(uint16_t x,uint16_t y,uint8_t *s,uint16_t fc,uint16_t bc,uint8_t sizey,uint8_t mode){ \
    uint8_t i,j,m=0; uint16_t k,x0=x; \
    uint16_t HZnum=sizeof(fsize)/sizeof(ftype); \
    uint16_t TFN=(sizey/8+((sizey%8)?1:0))*sizey; \
    for(k=0;k<HZnum;k++){ \
        if((fsize[k].Index[0]==*(s))&&(fsize[k].Index[1]==*(s+1))){ \
            LCD_Address_Set(x,y,x+sizey-1,y+sizey-1); \
            for(i=0;i<TFN;i++) for(j=0;j<8;j++){ \
                if(!mode){if(fsize[k].Msk[i]&(0x01<<j))LCD_WR_DATA(fc);else LCD_WR_DATA(bc);m++;if(m%sizey==0){m=0;break;}} \
                else{if(fsize[k].Msk[i]&(0x01<<j))LCD_DrawPoint(x,y,fc);x++;if((x-x0)==sizey){x=x0;y++;break;}} \
            } \
        } continue; \
    } \
}

_SHOW_CN_FONT(LCD_ShowChinese12x12, typFNT_GB12, tfont12)
_SHOW_CN_FONT(LCD_ShowChinese16x16, typFNT_GB16, tfont16)
_SHOW_CN_FONT(LCD_ShowChinese24x24, typFNT_GB24, tfont24)
_SHOW_CN_FONT(LCD_ShowChinese32x32, typFNT_GB32, tfont32)

void LCD_ShowChar(uint16_t x,uint16_t y,uint8_t num,uint16_t fc,uint16_t bc,uint8_t sizey,uint8_t mode)
{
    uint8_t temp,sizex,t,m=0; uint16_t i,TypefaceNum,x0=x;
    sizex=sizey/2; TypefaceNum=(sizex/8+((sizex%8)?1:0))*sizey; num=num-' ';
    LCD_Address_Set(x,y,x+sizex-1,y+sizey-1);
    for(i=0;i<TypefaceNum;i++){
        if(sizey==12)temp=ascii_1206[num][i];
        else if(sizey==16)temp=ascii_1608[num][i];
        else if(sizey==24)temp=ascii_2412[num][i];
        else if(sizey==32)temp=ascii_3216[num][i];
        else return;
        for(t=0;t<8;t++){
            if(!mode){if(temp&(0x01<<t))LCD_WR_DATA(fc);else LCD_WR_DATA(bc);m++;if(m%sizex==0){m=0;break;}}
            else{if(temp&(0x01<<t))LCD_DrawPoint(x,y,fc);x++;if((x-x0)==sizex){x=x0;y++;break;}}
        }
    }
}

void LCD_ShowString(uint16_t x,uint16_t y,const uint8_t *p,uint16_t fc,uint16_t bc,uint8_t sizey,uint8_t mode)
{ while(*p != '\0'){LCD_ShowChar(x,y,*p,fc,bc,sizey,mode);x+=sizey/2;p++;} }

uint32_t mypow(uint8_t m,uint8_t n){ uint32_t r=1; while(n--)r*=m; return r; }

void LCD_ShowIntNum(uint16_t x,uint16_t y,uint16_t num,uint8_t len,uint16_t fc,uint16_t bc,uint8_t sizey)
{
    uint8_t t,temp,enshow=0,sizex=sizey/2;
    for(t=0;t<len;t++){
        temp=(num/mypow(10,len-t-1))%10;
        if(enshow==0&&t<(len-1)){if(temp==0){LCD_ShowChar(x+t*sizex,y,' ',fc,bc,sizey,0);continue;}else enshow=1;}
        LCD_ShowChar(x+t*sizex,y,temp+48,fc,bc,sizey,0);
    }
}

void LCD_ShowFloatNum1(uint16_t x,uint16_t y,float num,uint8_t len,uint16_t fc,uint16_t bc,uint8_t sizey)
{
    uint8_t t,temp,sizex=sizey/2; uint16_t num1=num*100;
    for(t=0;t<len;t++){
        temp=(num1/mypow(10,len-t-1))%10;
        if(t==(len-2)){LCD_ShowChar(x+(len-2)*sizex,y,'.',fc,bc,sizey,0);t++;len+=1;}
        LCD_ShowChar(x+t*sizex,y,temp+48,fc,bc,sizey,0);
    }
}

void LCD_ShowPicture(uint16_t x,uint16_t y,uint16_t length,uint16_t width,const uint8_t pic[])
{
    uint16_t i,j; uint32_t k=0;
    LCD_Address_Set(x,y,x+length-1,y+width-1);
    for(i=0;i<length;i++) for(j=0;j<width;j++){LCD_WR_DATA8(pic[k*2]);LCD_WR_DATA8(pic[k*2+1]);k++;}
}

// ================================================================
// UTF-8 转 GB2312
// ================================================================

static uint32_t utf8_decode(const char *s,int *bc)
{
    uint8_t c=(uint8_t)s[0];
    if(c<0x80){*bc=1;return c;}
    else if((c&0xE0)==0xC0){*bc=2;return((c&0x1F)<<6)|((uint8_t)s[1]&0x3F);}
    else if((c&0xF0)==0xE0){*bc=3;return((c&0x0F)<<12)|(((uint8_t)s[1]&0x3F)<<6)|((uint8_t)s[2]&0x3F);}
    *bc=1; return '?';
}

static int unicode_to_gb2312(uint32_t cp,uint8_t *out)
{
    if(cp<0x80){out[0]=(uint8_t)cp;return 1;}
    static const struct{uint32_t u;uint8_t g[2];}tbl[]={
        {0x9759,{0xbe,0xb2}},{0x591c,{0xd2,0xb9}},{0x601d,{0xcb,0xbc}},
        {0x5e8a,{0xb4,0xb2}},{0x524d,{0xc7,0xb0}},{0x660e,{0xc3,0xf7}},
        {0x6708,{0xd4,0xc2}},{0x5149,{0xb9,0xe2}},{0x7591,{0xd2,0xc9}},
        {0x662f,{0xca,0xc7}},{0x5730,{0xb5,0xd8}},{0x4e0a,{0xc9,0xcf}},
        {0x971c,{0xcb,0xaa}},{0x4e3e,{0xbe,0xd9}},{0x5934,{0xcd,0xb7}},
        {0x671b,{0xcd,0xfb}},{0x4f4e,{0xb5,0xcd}},{0x6545,{0xb9,0xca}},
        {0x4e61,{0xcf,0xe7}},
    };
    for(int i=0;i<(int)(sizeof(tbl)/sizeof(tbl[0]));i++){
        if(tbl[i].u==cp){out[0]=tbl[i].g[0];out[1]=tbl[i].g[1];return 2;}
    }
    out[0]='?'; return 1;
}

int utf8_to_gb2312(const char *utf8,uint8_t *buf,int buf_size)
{
    int out_len=0,i=0,len=strlen(utf8);
    while(i<len&&out_len<buf_size-2){
        int consumed; uint32_t cp=utf8_decode(utf8+i,&consumed);
        uint8_t tmp[2]; int n=unicode_to_gb2312(cp,tmp);
        if(out_len+n<buf_size){buf[out_len++]=tmp[0];if(n==2)buf[out_len++]=tmp[1];}
        i+=consumed;
    }
    buf[out_len]=0; return out_len;
}
