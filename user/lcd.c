#include "lcd.h"
#include "lcdfont.h"

spi_device_handle_t s_spi = NULL;
static void lcd_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void lcd_spi_send(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_spi, &t);
}

void LCD_GPIO_Init(void)
{
    // DC, BLK, RES 为普通 GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL<<LCD_RES_PIN)|(1ULL<<LCD_DC_PIN)|(1ULL<<LCD_BLK_PIN);
    io_conf.pull_down_en = 0; io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(LCD_RES_PIN, 1);
    gpio_set_level(LCD_DC_PIN,  1);
    gpio_set_level(LCD_BLK_PIN, 0);  // 背光初始关闭

    // 初始化 SPI2 总线（无 CS，无 MISO）
    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_MOSI_PIN,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 80 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = -1,  // 无 CS
        .queue_size     = 7,
    };
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
}

void LCD_Writ_Bus(uint8_t dat) { lcd_spi_send(&dat, 1); }
void LCD_WR_DATA8(uint8_t dat) { lcd_spi_send(&dat, 1); }
void LCD_WR_DATA(uint16_t dat)
{
    uint8_t buf[2] = {dat >> 8, dat & 0xFF};
    lcd_spi_send(buf, 2);
}
void LCD_WR_REG(uint8_t dat)
{
    gpio_set_level(LCD_DC_PIN, 0);
    lcd_spi_send(&dat, 1);
    gpio_set_level(LCD_DC_PIN, 1);
}

void LCD_Send_Buf(const uint8_t *buf, uint32_t len)
{
    gpio_set_level(LCD_DC_PIN, 1);
    uint32_t remain = len;
    const uint8_t *p = buf;
    while (remain > 0) {
        uint32_t send = (remain > 32768) ? 32768 : remain;
        spi_transaction_t t = { .length = send * 8, .tx_buffer = p };
        spi_device_polling_transmit(s_spi, &t);
        p += send;
        remain -= send;
    }
}

/* 静态 DMA 事务对象：DMA 传输期间必须保持有效，故用静态分配 */
static spi_transaction_t s_dma_trans;
static bool              s_dma_pending = false;  /* 是否有未完成的异步事务 */

/* 异步 DMA 发送：将数据分块（每块 ≤ 4092 字节）逐块通过中断驱动 DMA 发送。
 * 最后一块异步入队后立即返回，前面各块同步等待完成。
 * ESP32-S3 SPI DMA 单次事务硬件上限为 4092 字节。 */
void LCD_Send_Buf_Async(const uint8_t *buf, uint32_t len)
{
    if (len == 0) return;

    gpio_set_level(LCD_DC_PIN, 1);  /* 像素数据，DC 置高 */

#define DMA_MAX_CHUNK 4092U  /* ESP32-S3 SPI DMA 单次事务最大字节数 */

    const uint8_t *p      = buf;
    uint32_t       remain = len;

    while (remain > 0) {
        uint32_t chunk = (remain > DMA_MAX_CHUNK) ? DMA_MAX_CHUNK : remain;
        bool     last  = (chunk == remain);  /* 是否为最后一块 */

        if (last) {
            /* 最后一块：异步入队，函数返回后 DMA 仍在传输 */
            memset(&s_dma_trans, 0, sizeof(s_dma_trans));
            s_dma_trans.length    = (size_t)chunk * 8;
            s_dma_trans.tx_buffer = p;
            spi_device_queue_trans(s_spi, &s_dma_trans, portMAX_DELAY);
            s_dma_pending = true;
        } else {
            /* 中间块：用中断驱动 DMA（任务阻塞等待，不占 CPU 忙等）*/
            spi_transaction_t t = { .length = chunk * 8, .tx_buffer = p };
            spi_device_transmit(s_spi, &t);
        }

        p      += chunk;
        remain -= chunk;
    }
}

/* 等待上次异步 DMA 传输完成，确保 SPI 总线空闲后再进行下一次操作 */
void LCD_Send_Buf_Wait(void)
{
    if (!s_dma_pending) return;
    spi_transaction_t *ret_trans;
    spi_device_get_trans_result(s_spi, &ret_trans, portMAX_DELAY);
    s_dma_pending = false;
}

void LCD_Address_Set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    LCD_WR_REG(0x2a);
    LCD_WR_DATA(x1); LCD_WR_DATA(x2);
    LCD_WR_REG(0x2b);
    LCD_WR_DATA(y1); LCD_WR_DATA(y2);
    LCD_WR_REG(0x2c);
}

void LCD_Backlight(uint8_t on)
{
    gpio_set_level(LCD_BLK_PIN, on ? 1 : 0);
}

void LCD_Init(void)
{
    LCD_GPIO_Init();
    LCD_RES_Clr(); lcd_delay_ms(200);
    LCD_RES_Set(); lcd_delay_ms(200);

    LCD_WR_REG(0x11); lcd_delay_ms(200);
    LCD_WR_REG(0xCF);
    LCD_WR_DATA8(0x00); LCD_WR_DATA8(0xC1); LCD_WR_DATA8(0x30);
    LCD_WR_REG(0xED);
    LCD_WR_DATA8(0x64); LCD_WR_DATA8(0x03); LCD_WR_DATA8(0x12); LCD_WR_DATA8(0x81);
    LCD_WR_REG(0xE8);
    LCD_WR_DATA8(0x85); LCD_WR_DATA8(0x00); LCD_WR_DATA8(0x79);
    LCD_WR_REG(0xCB);
    LCD_WR_DATA8(0x39); LCD_WR_DATA8(0x2C); LCD_WR_DATA8(0x00); LCD_WR_DATA8(0x34); LCD_WR_DATA8(0x02);
    LCD_WR_REG(0xF7); LCD_WR_DATA8(0x20);
    LCD_WR_REG(0xEA); LCD_WR_DATA8(0x00); LCD_WR_DATA8(0x00);
    LCD_WR_REG(0xC0); LCD_WR_DATA8(0x1D);
    LCD_WR_REG(0xC1); LCD_WR_DATA8(0x12);
    LCD_WR_REG(0xC5); LCD_WR_DATA8(0x33); LCD_WR_DATA8(0x3F);
    LCD_WR_REG(0xC7); LCD_WR_DATA8(0x92);
    LCD_WR_REG(0x3A); LCD_WR_DATA8(0x55);
    LCD_WR_REG(0x36);
    if(USE_HORIZONTAL==0)LCD_WR_DATA8(0x08);
    else if(USE_HORIZONTAL==1)LCD_WR_DATA8(0xC8);
    else if(USE_HORIZONTAL==2)LCD_WR_DATA8(0x78);
    else LCD_WR_DATA8(0xA8);
    LCD_WR_REG(0xB1); LCD_WR_DATA8(0x00); LCD_WR_DATA8(0x12);
    LCD_WR_REG(0xB6); LCD_WR_DATA8(0x0A); LCD_WR_DATA8(0xA2);
    LCD_WR_REG(0x44); LCD_WR_DATA8(0x02);
    LCD_WR_REG(0xF2); LCD_WR_DATA8(0x00);
    LCD_WR_REG(0x26); LCD_WR_DATA8(0x01);
    LCD_WR_REG(0xE0);
    LCD_WR_DATA8(0x0F); LCD_WR_DATA8(0x22); LCD_WR_DATA8(0x1C); LCD_WR_DATA8(0x1B);
    LCD_WR_DATA8(0x08); LCD_WR_DATA8(0x0F); LCD_WR_DATA8(0x48); LCD_WR_DATA8(0xB8);
    LCD_WR_DATA8(0x34); LCD_WR_DATA8(0x05); LCD_WR_DATA8(0x0C); LCD_WR_DATA8(0x09);
    LCD_WR_DATA8(0x0F); LCD_WR_DATA8(0x07); LCD_WR_DATA8(0x00);
    LCD_WR_REG(0xE1);
    LCD_WR_DATA8(0x00); LCD_WR_DATA8(0x23); LCD_WR_DATA8(0x24); LCD_WR_DATA8(0x07);
    LCD_WR_DATA8(0x10); LCD_WR_DATA8(0x07); LCD_WR_DATA8(0x38); LCD_WR_DATA8(0x47);
    LCD_WR_DATA8(0x4B); LCD_WR_DATA8(0x0A); LCD_WR_DATA8(0x13); LCD_WR_DATA8(0x06);
    LCD_WR_DATA8(0x30); LCD_WR_DATA8(0x38); LCD_WR_DATA8(0x0F);
    LCD_WR_REG(0x29);
    LCD_Backlight(1);  // 初始化完成后开背光
}

// ================================================================
// 绘图函数
// ================================================================

void LCD_Fill(uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color)
{
    uint8_t color_h = color >> 8;
    uint8_t color_l = color & 0xFF;
    static uint8_t fill_buf[LCD_W * 2];  // 一行缓冲
    uint32_t line_len = (xend - xsta) * 2;
    for (uint32_t i = 0; i < line_len; i += 2) {
        fill_buf[i]   = color_h;
        fill_buf[i+1] = color_l;
    }
    LCD_Address_Set(xsta, ysta, xend-1, yend-1);
    gpio_set_level(LCD_DC_PIN, 1);
    for (uint16_t i = ysta; i < yend; i++) {
        spi_transaction_t t = { .length = line_len * 8, .tx_buffer = fill_buf };
        spi_device_polling_transmit(s_spi, &t);
    }
}

void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_Address_Set(x, y, x, y);
    LCD_WR_DATA(color);
}

void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint16_t t;
    int xerr=0, yerr=0, delta_x, delta_y, distance, incx, incy, uRow, uCol;
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

void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    LCD_DrawLine(x1,y1,x2,y1,color); LCD_DrawLine(x1,y1,x1,y2,color);
    LCD_DrawLine(x1,y2,x2,y2,color); LCD_DrawLine(x2,y1,x2,y2,color);
}

void Draw_Circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)
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

void LCD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode)
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

void LCD_ShowString(uint16_t x, uint16_t y, const uint8_t *p, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode)
{ while(*p!='\0'){LCD_ShowChar(x,y,*p,fc,bc,sizey,mode);x+=sizey/2;p++;} }

uint32_t mypow(uint8_t m, uint8_t n){ uint32_t r=1; while(n--)r*=m; return r; }

void LCD_ShowIntNum(uint16_t x, uint16_t y, uint16_t num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey)
{
    uint8_t t,temp,enshow=0,sizex=sizey/2;
    for(t=0;t<len;t++){
        temp=(num/mypow(10,len-t-1))%10;
        if(enshow==0&&t<(len-1)){if(temp==0){LCD_ShowChar(x+t*sizex,y,' ',fc,bc,sizey,0);continue;}else enshow=1;}
        LCD_ShowChar(x+t*sizex,y,temp+48,fc,bc,sizey,0);
    }
}

void LCD_ShowFloatNum1(uint16_t x, uint16_t y, float num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey)
{
    uint8_t t,temp,sizex=sizey/2; uint16_t num1=num*100;
    for(t=0;t<len;t++){
        temp=(num1/mypow(10,len-t-1))%10;
        if(t==(len-2)){LCD_ShowChar(x+(len-2)*sizex,y,'.',fc,bc,sizey,0);t++;len+=1;}
        LCD_ShowChar(x+t*sizex,y,temp+48,fc,bc,sizey,0);
    }
}

void LCD_ShowPicture(uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[])
{
    uint16_t i,j; uint32_t k=0;
    LCD_Address_Set(x,y,x+length-1,y+width-1);
    for(i=0;i<length;i++) for(j=0;j<width;j++){LCD_WR_DATA8(pic[k*2]);LCD_WR_DATA8(pic[k*2+1]);k++;}
}
