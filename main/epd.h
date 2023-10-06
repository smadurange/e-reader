#ifndef EPD_H
#define EPD_H

void epd_init(void);
void epd_clear(void);
void epd_draw_async(const uint8_t *buf, size_t n);
void epd_draw_await(void);
void epd_sleep(void);

#endif /* EPD_H */
