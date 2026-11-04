const char *colorname[] = {

  /* 8 normal colors */
  [0] = "#0B0917", /* black   */
  [1] = "#394595", /* red     */
  [2] = "#4F50A7", /* green   */
  [3] = "#6074D8", /* yellow  */
  [4] = "#817EDF", /* blue    */
  [5] = "#7882B7", /* magenta */
  [6] = "#6F89E9", /* cyan    */
  [7] = "#c3ccf0", /* white   */

  /* 8 bright colors */
  [8]  = "#888ea8",  /* black   */
  [9]  = "#394595",  /* red     */
  [10] = "#4F50A7", /* green   */
  [11] = "#6074D8", /* yellow  */
  [12] = "#817EDF", /* blue    */
  [13] = "#7882B7", /* magenta */
  [14] = "#6F89E9", /* cyan    */
  [15] = "#c3ccf0", /* white   */

  /* special colors */
  [256] = "#0B0917", /* background */
  [257] = "#c3ccf0", /* foreground */
  [258] = "#c3ccf0",     /* cursor */
};

/* Default colors (colorname index)
 * foreground, background, cursor */
 unsigned int defaultbg = 0;
 unsigned int defaultfg = 257;
 unsigned int defaultcs = 258;
 unsigned int defaultrcs= 258;
