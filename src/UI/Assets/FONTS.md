# UI fonts

`font_ui_<size>.c` (14 16 20 24 28 40 48) are generated, not hand-written. LVGL's built-in Montserrat has ASCII only, so
accented letters and the middle dot showed as boxes.

Contents: Montserrat Medium 0x20-0x7F, 0xA0-0xFF (Latin-1), 0x2013 0x2014 0x2018 0x2019 0x201C 0x201D 0x2022 0x2026;
FontAwesome 5 for LVGL's built-in symbol set plus U+F2DC snowflake, U+F0EB lightbulb, U+F52B / U+F52A door open / closed,
U+F362 exchange (their UTF-8 forms are the `UI_ICON_*` macros in `Theme.h`).

Regenerate (needs Node; run from LVGL's `scripts/built_in_font` so the .ttf / .woff are found), per size `$s`:

    npx lv_font_conv@1.5.3 --bpp 4 --size $s --no-compress --no-prefilter --font Montserrat-Medium.ttf \
      -r 0x20-0x7F,0xA0-0xFF,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2022,0x2026 \
      --font FontAwesome5-Solid+Brands+Regular.woff -r <decimal code points, see built_in_font_gen.py `syms`, plus 62172,61675,62763,62762,62306> \
      --format lvgl --force-fast-kern-format --lv-font-name font_ui_$s -o font_ui_$s.c

`font_clock_200.c` is the big clock (digits and colon only). `img_mw_logo.c` is the MW monogram as an 8-bit alpha mask.
