/*
 * PlutoBrowser — error_page.h
 * Error page display (port of Source/ui/error_page.lua).
 */
#ifndef PLUTO_ERROR_PAGE_H
#define PLUTO_ERROR_PAGE_H

/* ErrorPage.show(errorMsg, failedUrl): resets selection to the first button. */
void error_page_show(const char *errorMsg, const char *failedUrl);

/* ErrorPage.handleInput(): feed the button edge masks from
 * pd->system->getButtonState(current, pushed, released). Returns
 * "retry"/"search"/"home" action (malloc'd via SDK; free with pluto_free)
 * or NULL when no button was pressed. */
char *error_page_handle_input(unsigned int current, unsigned int pushed,
                              unsigned int released);

/* ErrorPage.draw(). */
void error_page_draw(void);

/* Test/state accessors. */
int error_page_selected_index(void);
const char *error_page_message(void);
const char *error_page_failed_url(void);

#endif /* PLUTO_ERROR_PAGE_H */
