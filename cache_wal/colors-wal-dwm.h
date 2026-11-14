static const char norm_fg[] = "#c2c5c6";
static const char norm_bg[] = "#0c181d";
static const char norm_border[] = "#5c6970";

static const char sel_fg[] = "#c2c5c6";
static const char sel_bg[] = "#3A4B60";
static const char sel_border[] = "#c2c5c6";

static const char *colors[][3]      = {
    /*               fg           bg         border                         */
    [SchemeNorm] = { norm_fg,     norm_bg,   norm_border }, // unfocused wins
    [SchemeSel]  = { sel_fg,      sel_bg,    sel_border },  // the focused win
};
