// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_TESTS_UI_CURSES_H
#define DSD_NEO_TESTS_UI_CURSES_H

typedef struct WINDOW WINDOW;
typedef int chtype;

#define ERR                    (-1)
#define TRUE                   1
#define KEY_ENTER              343
/* PDCurses keypad Enter (0x1cb there) and Shift+keypad Enter (0x212), and its Shift modifier bit. Defined here so
   every platform runs the keypad Enter and Shift+Enter cases. */
#define PADENTER               459
#define SHF_PADENTER           530
#define PDC_KEY_MODIFIER_SHIFT 1

#define COLOR_BLACK            0
#define COLOR_RED              1
#define COLOR_GREEN            2
#define COLOR_YELLOW           3
#define COLOR_BLUE             4
#define COLOR_MAGENTA          5
#define COLOR_CYAN             6
#define COLOR_WHITE            7

extern WINDOW* stdscr;
WINDOW* initscr(void);
int curs_set(int visibility);
void timeout(int delay);
int start_color(void);
int keypad(WINDOW* win, int bf);
int has_colors(void);
int use_default_colors(void);
int assume_default_colors(int fg, int bg);
int init_pair(short pair, short fg, short bg);
int noecho(void);
int cbreak(void);
int endwin(void);
int set_escdelay(int size);
int wgetch(WINDOW* win);
/* PDCurses: the modifiers held with the key getch() returned last. A test that compiles code calling it defines it. */
unsigned long PDC_get_key_modifiers(void);

#define getch() wgetch(stdscr)

#endif /* DSD_NEO_TESTS_UI_CURSES_H */
