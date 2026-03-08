#pragma once

#include "boot_info.h"

void fb_init(const FramebufferInfo &fb);
void fb_putchar(char c);
void fb_flush();
bool fb_available();
