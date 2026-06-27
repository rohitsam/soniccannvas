#pragma once
// Physical HUB75 panel geometry — include before any header that uses these constants.
// SD_W and g_max_panels are runtime (read from NVS); defined in .ino.cpp.

#define PANEL_RES_X  32   // LED columns per individual panel tile
#define PANEL_RES_Y  16   // LED rows per individual panel tile

extern int SD_W;          // active display width = PANEL_RES_X × active panel count
extern int g_max_panels;  // physical chain length (inactive panels receive black)
