const char *colorname[] = {

  /* 8 normal colors */
  [0] = "#242563", /* black   */
  [1] = "#C57AC4", /* red     */
  [2] = "#A49AEA", /* green   */
  [3] = "#CF8BD3", /* yellow  */
  [4] = "#DCA5E8", /* blue    */
  [5] = "#ABC5EA", /* magenta */
  [6] = "#D3D2FD", /* cyan    */
  [7] = "#c8c8d8", /* white   */

  /* 8 bright colors */
  [8]  = "#787890",  /* black   */
  [9]  = "#C57AC4",  /* red     */
  [10] = "#A49AEA", /* green   */
  [11] = "#CF8BD3", /* yellow  */
  [12] = "#DCA5E8", /* blue    */
  [13] = "#ABC5EA", /* magenta */
  [14] = "#D3D2FD", /* cyan    */
  [15] = "#c8c8d8", /* white   */

  /* special colors */
  [256] = "#242563", /* background */
  [257] = "#c8c8d8", /* foreground */
  [258] = "#c8c8d8",     /* cursor */
};

/* Default colors (colorname index)
 * foreground, background, cursor */
 unsigned int defaultbg = 0;
 unsigned int defaultfg = 257;
 unsigned int defaultcs = 258;
 unsigned int defaultrcs= 258;
