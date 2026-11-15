// include/lgfx_config.h
#pragma once

#ifdef DISPLAY_ST7789
    #include "7789.h"
    #define BGCOLOR lv_color_hex(0x000033) // màu nền tối hơn một chút so với lv_color_black() để giảm chói
#elif defined(DISPLAY_ILI9341)
    #include "9341.h"
    #define BGCOLOR lv_color_hex(0x000010) // màu nền tối hơn một chút so với lv_color_black() để giảm chói
#else
    #error "Chưa chọn màn hình! Hãy thêm -D DISPLAY_ST7789 hoặc -D DISPLAY_ILI9341 vào build_flags."
#endif