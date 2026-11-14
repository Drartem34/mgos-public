const char *colorname[] = {

  /* 8 normal colors */
  [0] = "#0c181d", /* black   */
  [1] = "#3A4B60", /* red     */
  [2] = "#1F3997", /* green   */
  [3] = "#24439C", /* yellow  */
  [4] = "#424EC2", /* blue    */
  [5] = "#7C8CF1", /* magenta */
  [6] = "#939EF8", /* cyan    */
  [7] = "#c2c5c6", /* white   */

  /* 8 bright colors */
  [8]  = "#5c6970",  /* black   */
  [9]  = "#3A4B60",  /* red     */
  [10] = "#1F3997", /* green   */
  [11] = "#24439C", /* yellow  */
  [12] = "#424EC2", /* blue    */
  [13] = "#7C8CF1", /* magenta */
  [14] = "#939EF8", /* cyan    */
  [15] = "#c2c5c6", /* white   */

  /* special colors */
  [256] = "#0c181d", /* background */
  [257] = "#c2c5c6", /* foreground */
  [258] = "#c2c5c6",     /* cursor */
};

/* Default colors (colorname index)
 * foreground, background, cursor */
 unsigned int defaultbg = 0;
 unsigned int defaultfg = 257;
 unsigned int defaultcs = 258;
 unsigned int defaultrcs= 258;
