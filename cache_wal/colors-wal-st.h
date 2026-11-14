const char *colorname[] = {

  /* 8 normal colors */
  [0] = "#392e38", /* black   */
  [1] = "#51CFF6", /* red     */
  [2] = "#71D2F5", /* green   */
  [3] = "#A4B6D7", /* yellow  */
  [4] = "#9DDBF5", /* blue    */
  [5] = "#ACD7F5", /* magenta */
  [6] = "#E9D3E0", /* cyan    */
  [7] = "#cdcacd", /* white   */

  /* 8 bright colors */
  [8]  = "#866e83",  /* black   */
  [9]  = "#51CFF6",  /* red     */
  [10] = "#71D2F5", /* green   */
  [11] = "#A4B6D7", /* yellow  */
  [12] = "#9DDBF5", /* blue    */
  [13] = "#ACD7F5", /* magenta */
  [14] = "#E9D3E0", /* cyan    */
  [15] = "#cdcacd", /* white   */

  /* special colors */
  [256] = "#392e38", /* background */
  [257] = "#cdcacd", /* foreground */
  [258] = "#cdcacd",     /* cursor */
};

/* Default colors (colorname index)
 * foreground, background, cursor */
 unsigned int defaultbg = 0;
 unsigned int defaultfg = 257;
 unsigned int defaultcs = 258;
 unsigned int defaultrcs= 258;
