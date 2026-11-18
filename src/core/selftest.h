#ifndef KATOSHE_SELFTEST_H
#define KATOSHE_SELFTEST_H

/* Runs the engine's own checks (no framework). Returns 0 when everything passes,
   otherwise the number of failed checks. Prints one line per failure. */
int katoshe_selftest(void);

#endif
