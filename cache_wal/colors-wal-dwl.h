/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }

static const float rootcolor[]             = COLOR(0x392e38ff);
static const float bordercolor[]           = COLOR(0x71D2F5ff);
static const float focuscolor[]            = COLOR(0x51CFF6ff);
static const float urgentcolor[]           = COLOR(0xA4B6D7ff);
