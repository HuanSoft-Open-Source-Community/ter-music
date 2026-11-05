#ifndef UI_DIALOG_H
#define UI_DIALOG_H

#include "types.h"
#include <ncursesw/ncurses.h>

/* ── Dialog / input functions (defined in ui/dialog.c) ── */
int  prompt_text_input(WINDOW *win, int row, int col, const char *prompt,
                       char *buffer, size_t buffer_size, int trim_whitespace,
                       int password_mode, int prefill);
void prompt_open_folder(void);
void prompt_append_folder(void);

#endif /* UI_DIALOG_H */
