idf_component_register(
    SRCS "main.cpp" "i2s_mic.cpp" "lcd_disp.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp-sr driver esp_lcd
)
