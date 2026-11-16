/* PLUTO_NAV_AUTOTEST target (build with -DPLUTO_NAV_AUTOTEST_TARGET_FILE).
 * A -D value cannot carry "//" (the preprocessor reads it as a comment), so
 * the seam includes this header instead. Point it at any live page; the
 * nav autotest navigates there directly at boot and watches the render. */
#define PLUTO_NAV_AUTOTEST_URL_STR "https://bryanwandrych.com/projects"
