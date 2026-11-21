const char *colorname[] = {

  /* 8 normal colors */
  [0] = "#1f2024", /* black   */
  [1] = "#679CDE", /* red     */
  [2] = "#9C9CA6", /* green   */
  [3] = "#8A98B0", /* yellow  */
  [4] = "#94A3BA", /* blue    */
  [5] = "#99AAC6", /* magenta */
  [6] = "#AAB6CE", /* cyan    */
  [7] = "#c7c7c8", /* white   */

  /* 8 bright colors */
  [8]  = "#636a79",  /* black   */
  [9]  = "#679CDE",  /* red     */
  [10] = "#9C9CA6", /* green   */
  [11] = "#8A98B0", /* yellow  */
  [12] = "#94A3BA", /* blue    */
  [13] = "#99AAC6", /* magenta */
  [14] = "#AAB6CE", /* cyan    */
  [15] = "#c7c7c8", /* white   */

  /* special colors */
  [256] = "#1f2024", /* background */
  [257] = "#c7c7c8", /* foreground */
  [258] = "#c7c7c8",     /* cursor */
};

/* Default colors (colorname index)
 * foreground, background, cursor */
 unsigned int defaultbg = 0;
 unsigned int defaultfg = 257;
 unsigned int defaultcs = 258;
 unsigned int defaultrcs= 258;
