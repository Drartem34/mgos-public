const char *colorname[] = {

  /* 8 normal colors */
  [0] = "#0f1724", /* black   */
  [1] = "#4D4D6B", /* red     */
  [2] = "#4F3D8A", /* green   */
  [3] = "#5D419C", /* yellow  */
  [4] = "#746495", /* blue    */
  [5] = "#6243A3", /* magenta */
  [6] = "#877C99", /* cyan    */
  [7] = "#c3c5c8", /* white   */

  /* 8 bright colors */
  [8]  = "#5e6774",  /* black   */
  [9]  = "#4D4D6B",  /* red     */
  [10] = "#4F3D8A", /* green   */
  [11] = "#5D419C", /* yellow  */
  [12] = "#746495", /* blue    */
  [13] = "#6243A3", /* magenta */
  [14] = "#877C99", /* cyan    */
  [15] = "#c3c5c8", /* white   */

  /* special colors */
  [256] = "#0f1724", /* background */
  [257] = "#c3c5c8", /* foreground */
  [258] = "#c3c5c8",     /* cursor */
};

/* Default colors (colorname index)
 * foreground, background, cursor */
 unsigned int defaultbg = 0;
 unsigned int defaultfg = 257;
 unsigned int defaultcs = 258;
 unsigned int defaultrcs= 258;
