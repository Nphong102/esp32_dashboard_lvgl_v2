#pragma once

#include <LovyanGFX.hpp>
#include <driver/spi_master.h>

// ───────────── LovyanGFX ILI9341 (CYD) ─────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;        // = USE_HSPI_PORT
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;         // = SPI_FREQUENCY 40MHz
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = 12;               // = TFT_SCLK
            cfg.pin_mosi    = 11;               // = TFT_MOSI
            cfg.pin_miso    = 13;               // = TFT_MISO
            cfg.pin_dc      = 46;               // = TFT_DC
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs          = 10;           // = TFT_CS
            cfg.pin_rst         = -1;           // = TFT_RST  (-1 = dùng RST board)
            cfg.pin_busy        = -1;
            cfg.panel_width     = 240;          // = TFT_WIDTH
            cfg.panel_height    = 320;          // = TFT_HEIGHT
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.readable        = false;
            cfg.invert          = true;         // ⚠ xem ghi chú bên dưới
            cfg.rgb_order       = false;        // = TFT_RGB_ORDER TFT_BGR  (false = BGR)
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = 45;               // = TFT_BL
            cfg.invert      = false;            // = TFT_BACKLIGHT_ON HIGH
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};