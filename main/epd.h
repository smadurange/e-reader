#ifndef EPD_H
#define EPD_H

void epd_init(void);
void epd_clear(void);
void epd_draw(const uint8_t pb[48000]);
void epd_sleep(void);

#endif /* EPD_H */
