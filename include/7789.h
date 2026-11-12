#pragma once

#include <LovyanGFX.hpp>
#include <driver/spi_master.h>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX() {
        auto busConfig = _bus.config();
        busConfig.spi_host = SPI2_HOST;
        busConfig.spi_mode = 0;
        busConfig.freq_write = 80000000;
        busConfig.freq_read = 16000000;
        busConfig.spi_3wire = false;
        busConfig.use_lock = true;
        busConfig.dma_channel = SPI_DMA_CH_AUTO;
        busConfig.pin_sclk = 15;
        busConfig.pin_mosi = 7;
        busConfig.pin_miso = -1;
        busConfig.pin_dc = 6;
        _bus.config(busConfig);
        _panel.setBus(&_bus);

        auto panelConfig = _panel.config();
        panelConfig.pin_cs = 4;
        panelConfig.pin_rst = 5;
        panelConfig.pin_busy = -1;
        panelConfig.memory_width = 240;
        panelConfig.memory_height = 320;
        panelConfig.panel_width = 240;
        panelConfig.panel_height = 320;
        panelConfig.offset_x = 0;
        panelConfig.offset_y = 0;
        panelConfig.offset_rotation = 0;
        panelConfig.readable = false;
        panelConfig.invert = false;
        panelConfig.rgb_order = false;
        panelConfig.dlen_16bit = false;
        panelConfig.bus_shared = false;
        _panel.config(panelConfig);

        auto lightConfig = _light.config();
        lightConfig.pin_bl = 16;
        lightConfig.invert = false;
        lightConfig.freq = 44100;
        lightConfig.pwm_channel = 7;
        _light.config(lightConfig);
        _panel.setLight(&_light);

        setPanel(&_panel);
    }
};
