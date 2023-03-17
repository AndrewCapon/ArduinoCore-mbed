#include "video_driver.h"
#include "SDRAM.h"
#include "video_modes.h"
#include "anx7625.h"
#include "dsi.h"

static uint32_t lcd_x_size = 0;
static uint32_t lcd_y_size = 0;

static uint16_t * fb;
static lv_disp_drv_t disp_drv;

struct edid recognized_edid;

/* Display flushing */
static void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {

#if defined(__CORTEX_M7)
  SCB_CleanInvalidateDCache();
  SCB_InvalidateICache();
#endif

  DMA2D_HandleTypeDef * dma2d = stm32_get_DMA2D();

  lv_color_t * pDst = (lv_color_t*)fb;
  pDst += area->y1 * lcd_x_size + area->x1;

  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  /*##-1- Configure the DMA2D Mode, Color Mode and output offset #############*/
  dma2d->Init.Mode         = DMA2D_M2M;
  dma2d->Init.ColorMode    = DMA2D_OUTPUT_RGB565;
  dma2d->Init.OutputOffset = lcd_x_size - w;
  dma2d->Init.AlphaInverted = DMA2D_REGULAR_ALPHA;  /* No Output Alpha Inversion*/
  dma2d->Init.RedBlueSwap   = DMA2D_RB_REGULAR;     /* No Output Red & Blue swap */

  /*##-2- DMA2D Callbacks Configuration ######################################*/
  dma2d->XferCpltCallback  = NULL;

  /*##-3- Foreground Configuration ###########################################*/
  dma2d->LayerCfg[1].AlphaMode = DMA2D_NO_MODIF_ALPHA;
  dma2d->LayerCfg[1].InputAlpha = 0xFF;
  dma2d->LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
  dma2d->LayerCfg[1].InputOffset = 0;
  dma2d->LayerCfg[1].RedBlueSwap = DMA2D_RB_REGULAR; /* No ForeGround Red/Blue swap */
  dma2d->LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA; /* No ForeGround Alpha inversion */

  /* DMA2D Initialization */
  if (HAL_DMA2D_Init(dma2d) == HAL_OK) {
    if (HAL_DMA2D_ConfigLayer(dma2d, 1) == HAL_OK) {
      HAL_DMA2D_Start(dma2d, (uint32_t)color_p, (uint32_t)pDst, w, h);
      HAL_DMA2D_PollForTransfer(dma2d, 1000);
    }
  }

  lv_disp_flush_ready(disp); /* tell lvgl that flushing is done */
}


/* If your MCU has hardware accelerator (GPU) then you can use it to blend to memories using opacity
   It can be used only in buffered mode (LV_VDB_SIZE != 0 in lv_conf.h)*/
static void gpu_blend(lv_disp_drv_t * disp_drv, lv_color_t * dest, const lv_color_t * src, uint32_t length, lv_opa_t opa)
{

#if defined(__CORTEX_M7)
  SCB_CleanInvalidateDCache();
#endif

  DMA2D_HandleTypeDef * dma2d = stm32_get_DMA2D();

  dma2d->Instance = DMA2D;
  dma2d->Init.Mode = DMA2D_M2M_BLEND;
  dma2d->Init.OutputOffset = 0;

  /* Foreground layer */
  dma2d->LayerCfg[1].AlphaMode = DMA2D_REPLACE_ALPHA;
  dma2d->LayerCfg[1].InputAlpha = opa;
  dma2d->LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
  dma2d->LayerCfg[1].InputOffset = 0;
  dma2d->LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;

  /* Background layer */
  dma2d->LayerCfg[0].AlphaMode = DMA2D_NO_MODIF_ALPHA;
  dma2d->LayerCfg[0].InputColorMode = DMA2D_INPUT_RGB565;
  dma2d->LayerCfg[0].InputOffset = 0;

  /* DMA2D Initialization */
  if (HAL_DMA2D_Init(dma2d) == HAL_OK) {
    if (HAL_DMA2D_ConfigLayer(dma2d, 0) == HAL_OK && HAL_DMA2D_ConfigLayer(dma2d, 1) == HAL_OK) {
      HAL_DMA2D_BlendingStart(dma2d, (uint32_t) src, (uint32_t) dest, (uint32_t) dest, length, 1);
      HAL_DMA2D_PollForTransfer(dma2d, 1000);
    }
  }
}

/* If your MCU has hardware accelerator (GPU) then you can use it to fill a memory with a color */
static void gpu_fill(lv_disp_drv_t * disp_drv, lv_color_t * dest_buf, lv_coord_t dest_width,
                     const lv_area_t * fill_area, lv_color_t color) {
#if defined(__CORTEX_M7)
  SCB_CleanInvalidateDCache();
#endif

  DMA2D_HandleTypeDef * dma2d = stm32_get_DMA2D();

  lv_color_t * destination = dest_buf + (dest_width * fill_area->y1 + fill_area->x1);

  uint32_t w = fill_area->x2 - fill_area->x1 + 1;
  dma2d->Instance = DMA2D;
  dma2d->Init.Mode = DMA2D_R2M;
  dma2d->Init.ColorMode = DMA2D_OUTPUT_RGB565;
  dma2d->Init.OutputOffset = dest_width - w;
  dma2d->LayerCfg[1].InputAlpha = DMA2D_NO_MODIF_ALPHA;
  dma2d->LayerCfg[1].InputColorMode = DMA2D_OUTPUT_RGB565;

  /* DMA2D Initialization */
  if (HAL_DMA2D_Init(dma2d) == HAL_OK) {
    if (HAL_DMA2D_ConfigLayer(dma2d, 1) == HAL_OK) {
      lv_coord_t h = lv_area_get_height(fill_area);
      if (HAL_DMA2D_BlendingStart(dma2d, lv_color_to32(color), (uint32_t)destination, (uint32_t)destination, w, h) == HAL_OK) {
        HAL_DMA2D_PollForTransfer(dma2d, 1000);
      }
    }
  }
}

void portenta_init_video() {
  int ret = -1;

  //Initialization of ANX7625
  ret = anx7625_init(0);
  if(ret < 0) {
    printf("Cannot continue, anx7625 init failed.\n");
    while(1);
  }

  //Checking HDMI plug event
  anx7625_wait_hpd_event(0);

  //Read EDID
  anx7625_dp_get_edid(0, &recognized_edid);

  //DSI Configuration
  anx7625_dp_start(0, &recognized_edid, EDID_MODE_720x480_60Hz);

  //Configure SDRAM 
  SDRAM.begin(getFramebufferEnd());

  //Initialization of LVGL library (Embedded Graphic Library)
  lv_init();

  //Retrieving LCD size (X,Y)
  lcd_x_size = stm32_getXSize();
  lcd_y_size = stm32_getYSize();

  fb = (uint16_t *)getNextFrameBuffer();
  getNextFrameBuffer();

  //Initialization of display buffer with LVGL
  static lv_color_t buf[LV_HOR_RES_MAX * LV_VER_RES_MAX / 6];
  
  // Compatibility with v7 and v8 APIs
  #if LVGL_VERSION_MAJOR == 8
    static lv_disp_draw_buf_t  disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, LV_HOR_RES_MAX * LV_VER_RES_MAX / 6);

    /*Initialize the display*/
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = my_disp_flush;
    #if LVGL_VERSION_MINOR < 1
    disp_drv.gpu_fill_cb = gpu_fill;
    #endif
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res = LV_HOR_RES_MAX;
    disp_drv.ver_res = LV_VER_RES_MAX;

    lv_disp_drv_register(&disp_drv);

  #else
    static lv_disp_buf_t disp_buf;
    lv_disp_buf_init(&disp_buf, buf, NULL, LV_HOR_RES_MAX * LV_VER_RES_MAX / 6);

    /*Initialize the display*/
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.gpu_fill_cb = gpu_fill;
    disp_drv.gpu_blend_cb = gpu_blend;
    disp_drv.buffer = &disp_buf;
    
    lv_disp_drv_register(&disp_drv);

  #endif
}

void giga_init_video(bool landscape) {
  // put your setup code here, to run once:
  int ret = -1;

  SDRAM.begin();

  extern struct envie_edid_mode envie_known_modes[];
  struct edid _edid;
  int mode = EDID_MODE_480x800_60Hz;
  struct display_timing dt;
  dt.pixelclock = envie_known_modes[mode].pixel_clock;
  dt.hactive = envie_known_modes[mode].hactive;
  dt.hsync_len = envie_known_modes[mode].hsync_len;
  dt.hback_porch = envie_known_modes[mode].hback_porch;
  dt.hfront_porch = envie_known_modes[mode].hfront_porch;
  dt.vactive = envie_known_modes[mode].vactive;
  dt.vsync_len = envie_known_modes[mode].vsync_len;
  dt.vback_porch = envie_known_modes[mode].vback_porch;
  dt.vfront_porch = envie_known_modes[mode].vfront_porch;
  dt.hpol = envie_known_modes[mode].hpol;
  dt.vpol = envie_known_modes[mode].vpol;

  stm32_dsi_config(0, &_edid, &dt);

  lv_init();

  lcd_x_size = stm32_getXSize();
  lcd_y_size = stm32_getYSize();

  fb = (uint16_t *)getNextFrameBuffer();
  getNextFrameBuffer();

  static lv_color_t buf[800 * 480 / 6];

  // Compatibility with v7 and v8 APIs
  #if LVGL_VERSION_MAJOR == 8
    static lv_disp_draw_buf_t  disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, lcd_x_size * lcd_y_size / 6);

    /*Initialize the display*/
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = my_disp_flush;
    #if LVGL_VERSION_MINOR < 1
    disp_drv.gpu_fill_cb = gpu_fill;
    #endif
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res = lcd_x_size;
    disp_drv.ver_res = lcd_y_size;
    if (landscape) {
      disp_drv.rotated = LV_DISP_ROT_90;
      disp_drv.sw_rotate = 1;
    }

    lv_disp_drv_register(&disp_drv);

  #else
    static lv_disp_buf_t disp_buf;
    lv_disp_buf_init(&disp_buf, buf, NULL, lcd_x_size * lcd_y_size / 6);

    /*Initialize the display*/
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.gpu_fill_cb = gpu_fill;
    disp_drv.gpu_blend_cb = gpu_blend;
    disp_drv.buffer = &disp_buf;
    
    lv_disp_drv_register(&disp_drv);

  #endif
}

//Get debug info
void printInfo()  {
  printf("Tick: %ld\n", lv_tick_get());
  printf("\r");
}
