#ifndef VIDEO_H
#define VIDEO_H

void video_init(const void *fdt);
void video_display(const unsigned int *bmp_image,
                   unsigned int width,
                   unsigned int height);
void flush_dcache(void *addr, unsigned long len);

#endif
