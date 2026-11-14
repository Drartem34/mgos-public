static const char norm_fg[] = "#cdcacd";
static const char norm_bg[] = "#392e38";
static const char norm_border[] = "#866e83";

static const char sel_fg[] = "#cdcacd";
static const char sel_bg[] = "#71D2F5";
static const char sel_border[] = "#cdcacd";

static const char urg_fg[] = "#cdcacd";
static const char urg_bg[] = "#51CFF6";
static const char urg_border[] = "#51CFF6";

static const char *colors[][3]      = {
    /*               fg           bg         border                         */
    [SchemeNorm] = { norm_fg,     norm_bg,   norm_border }, // unfocused wins
    [SchemeSel]  = { sel_fg,      sel_bg,    sel_border },  // the focused win
    [SchemeUrg] =  { urg_fg,      urg_bg,    urg_border },
};
