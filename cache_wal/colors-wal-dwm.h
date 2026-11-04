static const char norm_fg[] = "#c3ccf0";
static const char norm_bg[] = "#0B0917";
static const char norm_border[] = "#888ea8";

static const char sel_fg[] = "#c3ccf0";
static const char sel_bg[] = "#4F50A7";
static const char sel_border[] = "#c3ccf0";

static const char urg_fg[] = "#c3ccf0";
static const char urg_bg[] = "#394595";
static const char urg_border[] = "#394595";

static const char *colors[][3]      = {
    /*               fg           bg         border                         */
    [SchemeNorm] = { norm_fg,     norm_bg,   norm_border }, // unfocused wins
    [SchemeSel]  = { sel_fg,      sel_bg,    sel_border },  // the focused win
    [SchemeUrg] =  { urg_fg,      urg_bg,    urg_border },
};
