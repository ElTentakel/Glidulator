/* Write-combining for the display adapter's framebuffer under Windows 9x
 * (ring 0 through a temporary call gate), see wc_win9x.c. */

#ifndef GLIDULATOR_WC_WIN9X_H
#define GLIDULATOR_WC_WIN9X_H

#include <stdint.h>

/* need: bytes of the current screen mode (lower bound of the WC range).
 * 1 = switched on, 0 = already WC, -1 = not possible or not Windows 9x. */
int wc_win9x_on(uint32_t need);

/* Writes the original MTRRs back (only if wc_win9x_on switched WC on). */
void wc_win9x_off(void);

#endif /* GLIDULATOR_WC_WIN9X_H */
