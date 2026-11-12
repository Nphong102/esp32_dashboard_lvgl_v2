// include/lgfx_config.h
#pragma once

#ifdef DISPLAY_ST7789
    #include "7789.h"
#elif defined(DISPLAY_ILI9341)
    #include "9341.h"
#else
    #error "Chưa chọn màn hình! Hãy thêm -D DISPLAY_ST7789 hoặc -D DISPLAY_ILI9341 vào build_flags."
#endif