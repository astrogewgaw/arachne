/* Calculate probability of shifting the bit: 8 bit injection */
int m;

int cal_bit_shift_prob(int in, double lvl, double signal) {
  int i = in + (pow(2, m) / 2) - 1;
  double plvl = 0, pval = 0;
  int out = in; // prob 255--> 255
  if (in == 0) {
    for (int m = 0; m <= 255 - in; m++) {

      if (in + m == 0) {
        plvl = prob(-127 * lvl - signal) / prob(-127 * lvl); // prob 0--> 0
        if (pval < plvl) {
          pval = plvl;
          out = in + m;
        }
      } else if (in + m == 255) {
        plvl = (prob(-127 * lvl) - prob(127 * lvl - signal)) /
               prob(-127 * lvl); // prob o--> 255
        if (pval < plvl) {
          pval = plvl;
          out = in + m;
        }
      } else {
        plvl = (prob(min(-127 * lvl, (m - 127) * lvl - signal)) -
                prob((m - 128) * lvl - signal)) /
               prob((in - 127) * lvl); // prob 0--> m
        if (pval < plvl) {
          pval = plvl;
          out = in + m;
        }
      }
    }
  } else {
    for (int m = 0; m <= 255 - in; m++) {
      if (in + m == 255) {
        plvl = ((prob((in - 127) * lvl) -
                 prob(max((in - 128) * lvl, 127 * lvl - signal))) /
                (prob((in - 127) * lvl) -
                 prob((in - 128) * lvl))); // prob n--> 255
        if (pval < plvl) {
          pval = plvl;
          out = in + m;
        }
      } else {
        plvl = ((prob(min((in - 127) * lvl, (in + m - 127) * lvl - signal)) -
                 prob(max((in - 128) * lvl, (in + m - 128) * lvl - signal))) /
                (prob((in - 127) * lvl) -
                 prob((in - 128) * lvl))); // prob n--> n+m
        if (pval < plvl) {
          pval = plvl;
          out = in + m;
        }
      }
    }
  }
  return out;
}