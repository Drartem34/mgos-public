static const char norm_fg[] = "#c8c8d8";
static const char norm_bg[] = "#242563";
static const char norm_border[] = "#787890";

static const char sel_fg[] = "#c8c8d8";
static const char sel_bg[] = "#C57AC4";
static const char sel_border[] = "#c8c8d8";

static const char *colors[][3]      = {
    /*               fg           bg         border                         */
    [SchemeNorm] = { norm_fg,     norm_bg,   norm_border }, // unfocused wins
    [SchemeSel]  = { sel_fg,      sel_bg,    sel_border },  // the focused win
};
