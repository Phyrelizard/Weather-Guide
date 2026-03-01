#ifndef WEATHER_ICONS_H
#define WEATHER_ICONS_H

#include <lvgl.h>

/*
  IMPORTANT (PlatformIO / C++ linkage note):

  The image converter exported the icon definitions as .c files (compiled as C),
  while your app (main.cpp) is compiled as C++. Without C linkage, the symbols
  (sunny, cloudy, etc.) will be name-mangled and won't link when referenced.

  Wrapping LV_IMG_DECLARE() in extern "C" ensures the linker can find them.
*/
#ifdef __cplusplus
extern "C" {
#endif

// Declare all weather icons (defined in the .c files from the LVGL image converter)
LV_IMG_DECLARE(sunny);
LV_IMG_DECLARE(cloudy);
LV_IMG_DECLARE(partly_cloudy);
LV_IMG_DECLARE(rain);
LV_IMG_DECLARE(storm);
LV_IMG_DECLARE(snow);
LV_IMG_DECLARE(fog);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WEATHER_ICONS_H
