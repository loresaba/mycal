/*
 * MIT License
 *
 * Copyright (c) 2026 Lorenzo Sabatino
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#define DTPARSE_IMPLEMENTATION
#include "dtparse.h"

#define HOURS_IN_DAY      24
#define MINUTES_IN_HOUR   60
#define SECONDS_IN_MINUTE 60
#define DAYS_IN_WEEK      7

#define BLINK_MS 500 /* Blink time in milliseconds during the selection */

#define FULL_DATETIME_FMT "%A, %d %B %Y %H:%M"
#define ACTIVITY_DATE_FMT "%A, %d %B %Y"
#define ACTIVITY_TIME_FMT "%H:%M"
#define TIMESLOT_FMT      "%I:%M %p"
#define TIME_SLOT_SIZE 30 /* Number of minutes in each time slot */

#define MIN_WDAY_LENGTH 2
#define MAX_MDAY_LENGTH 2

#define MDAY_HEADER_ROWS 1 /* Number of header rows for a day of the month */
#define MDAY_BODY_ROWS   3 /* Number of body rows for a day of the month */

#define DEFAULT_LAST_ACTIVITY_COUNT 10 /* Number of last activities in 
                                        * activity view */

#define MAX_FORM_WIDTH 60

#define TITLE_SIZE          64
#define START_STR_SIZE      128
#define END_STR_SIZE        128
#define NOTES_SIZE          256
#define CATEGORY_NAME_SIZE  32

#define UNCAT_COLOR COLOR_CYAN /* Background color for uncategorized 
                                * activities */

#define CATEGORY_ID_NONE 0
#define CATEGORY_ID_NEW  SIZE_MAX

/* ================================== Data ================================== */

/* Represent both special keys inside the standard ASCII range and escape
 * sequences. */
enum key {
    /* Standard ASCII control characters */
    KEY_NULL   = 0,     /* NULL */
    CTRL_C     = 3,     /* Ctrl-c */
    CTRL_D     = 4,     /* Ctrl-d */
    CTRL_F     = 6,     /* Ctrl-f */
    CTRL_H     = 8,     /* Ctrl-h */
    TAB        = 9,     /* Tab */
    CTRL_L     = 12,    /* Ctrl+l */
    ENTER      = 13,    /* Enter */
    CTRL_Q     = 17,    /* Ctrl-q */
    CTRL_S     = 19,    /* Ctrl-s */
    CTRL_U     = 21,    /* Ctrl-u */
    ESC        = 27,    /* Escape */
    BACKSPACE  = 127,   /* Backspace */
    /* Escape sequences */
    KEY_ARROW_LEFT = 1000,
    KEY_ARROW_RIGHT,
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_DEL,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN
};

/* Represent all the colors supported by the program. */
typedef enum {
    COLOR_DEFAULT = 0,
    COLOR_CYAN,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_BLUE,
    COLOR_YELLOW,
    COLOR_MAGENTA,
    COLOR_WHITE,
    COLOR_BLACK,
    __color_count,
} Color;

/* Represent a category used for grouping activities together. 
 * Each category is a node of the category tree and the children of that
 * node represent its sub-categories. */
typedef struct {
    size_t id;
    const char *name;
    Color color;
    size_t parent_id;
} Category;

/* Represent a dynamic array of categories. */
typedef struct {
    Category **items;
    size_t count;
    size_t capacity;
} CategoryList;

/* Represent an activity. */
typedef struct {
    size_t id;
    const char *title;
    time_t start_ts, end_ts;
    const char *notes;
    Category *category; /* owned category copy */
} Activity;

/* Represent a dynamic array of activities. */
typedef struct {
    Activity **items;
    size_t count;
    size_t capacity;
} ActivityList;

/* A text style is a 32-bit integer that holds all the styling for a text
 * character.
 * [ Unused 8 bit | Background 8 bits | Foreground 8 bits | Flags 8 bits ] */
typedef uint32_t TextStyle;

/* Text mode flags */
#define TEXT_STYLE_NONE      0
#define TEXT_STYLE_BOLD      (1 << 0)
#define TEXT_STYLE_DIM       (1 << 1)
#define TEXT_STYLE_ITALIC    (1 << 2)
#define TEXT_STYLE_UNDERLINE (1 << 3)
#define TEXT_STYLE_BLINK     (1 << 4)
#define TEXT_STYLE_INVERT    (1 << 5)
#define TEXT_STYLE_HIDDEN    (1 << 6)

/* Text mode shifts and masks */
#define FG_SHIFT   8
#define BG_SHIFT   16
#define COLOR_MASK 0xFF
#define FLAGS_MASK 0xFF

/* Represent a single line in the view that holds characters associated with
 * their text styles. We use this structure to proper render the text on the
 * screen using colors and effects. */
struct vrow {
    char *chars;
    TextStyle *tstyle;
    int len;
    int size; /* does not include the null term */
};

/* Represent a bounding box. */
struct bounding_box {
    int x;
    int y; 
    int width; 
    int height; 
};

/* Represent a dynamic array of bounding boxes. */
struct bbox_list {
    struct bounding_box *items;
    size_t count;
    size_t capacity;
};

/* Represent the horizontal layout grid for dividing rows into units.
 * It maps a row space into temporal periods (e.g. days in a week). */
struct row_grid {
    /* Represent a single unit of the grid. */
    struct grid_cell {
        int offset;      /* Starting column of the cell */
        int width;       /* Number of columns of the cell */
        time_t start_ts; /* Start timestamp of the cell */
        time_t end_ts;   /* End timestamp of the cell */
    } *cells;
    int cell_count;   /* Number of units in the grid */
    int delim_width;  /* Number of columns for the delimiter characters 
                       * between cells */
};

/* Represent the possible types for a view. */
typedef enum {
    VIEW_NONE,
    VIEW_CUSTOM,
    VIEW_ACTIVITY,
    VIEW_CATEGORY,
} ViewType;

/* Represent a tagged union structure used for display contents on the 
 * screen. */
typedef struct {
    int ncols;         /* Number of characters in a row */
    int nrows;         /* Number of rows in view */
    int rows_size;     /* Allocated number of rows */
    struct vrow *rows; /* Array of rows */

    void **entities;              /* Array of entities */
    struct bbox_list *bbox_lists; /* Array of bounding box list for entities */
    size_t entity_count;          /* Number of entities */

    ViewType type;
    union {
        /* Represent an activity view. */
        struct {
            /* Type of the activity view. */
            enum act_view_type {
                ACTIVITY_VIEW_CUSTOM,
                ACTIVITY_VIEW_DAY,
                ACTIVITY_VIEW_WEEK,
                ACTIVITY_VIEW_MONTH,
                ACTIVITY_VIEW_YEAR,
            } act_type;

            time_t curr_ts, start_ts, end_ts;
            int header_rows; /* Number of rows for the header */
            int body_off;    /* Column start of body content */
            int body_len;    /* Length of the body content */
            int body_rows;   /* Number of rows in the body */

            struct row_grid grid; /* Grid of the row */
            int day_gap;          /* Number of days to skip when moving to 
                                   * the next row grid. Used in month and 
                                   * year views. */

            /* Time slot */
            char *tfmt;       /* Format string of the time in time slots */
            int tslot_size;   /* Number of minutes in each time slot */
            int all_day_rows; /* Number of rows for the all-day activities */
        } act;
        /* Represent the category view. */
        struct {
            int header_rows; /* Number of rows for the header */
            int *depths;     /* Array of tree depth for each category */
        } cat;
    } as;
} View;

/* Represent the entity that holds all the informations for the rendering
 * content on the screen. */
typedef struct {
    int x, y;       /* Horizontal and vertical position of the cursor in 
                     * the screen, -1 for keep current cursor position. */
    int rowoff;     /* First visible row index */
    int coloff;     /* First visible column index */
    int screenrows; /* Number of rows to show */
    int screencols; /* Number of columns to show */

    int vert_scroll; /* 1 if the vertical scroll is active */
    int horz_scroll; /* 1 if the horizontal scroll is active */

    /* Indicate the alignment of the text */
    enum { 
        RENDERER_ALIGN_START,        /* Align text to the left of the screen */
        RENDERER_ALIGN_CENTER,       /* Centers individually lines in the 
                                      * screen */
        RENDERER_ALIGN_CENTER_BLOCK, /* Centers the entire block of text in the
                                      * screen */
        RENDERER_ALIGN_END,          /* Align text to the right of the screen */
    } text_align;
} Renderer;

/* Represent the global instace of the program. */
struct mycal {
    sqlite3 *db;                 /* Database */
    struct termios orig_termios; /* Terminal */
    int raw_mode_active;         /* 1 if raw mode is active */
    int win_width, win_height;   /* Width and height of the window */
    const char *progname;        /* Program name */
    time_t now;                  /* Current timestamp */
} Mycal;

Category *getCategoryById(sqlite3 *db, size_t id);
int getCategoriesByParentId(sqlite3 *db, size_t parent_id, 
        CategoryList *child_list);
void getDayBounds(time_t ts, time_t *start_ts, time_t *end_ts);

/* ================================= Macros ================================= */

/* Round up the interger division between 'a' and 'b'. */
#define ROUND_INT_DIV(a, b) (((a) + (b) - 1) / (b))

/* Return true if the range [a1,a2) overlaps with the range [b1,b2),
 * false otherwise. */
#define RANGES_OVERLAP(a1, a2, b1, b2) ((a1) < (b2) && (b1) < (a2))


/*** Text style macros ***/

/* Create a text style. */
#define MAKE_TEXT_STYLE(fg, bg, flags)   \
    ((flags) |                           \
     (((fg) & COLOR_MASK) << FG_SHIFT) | \
     (((bg) & COLOR_MASK) << BG_SHIFT))

/* Extract properties from text style. */
#define GET_FG(tstyle)         ((Color)(((tstyle) >> FG_SHIFT) & COLOR_MASK))
#define GET_BG(tstyle)         ((Color)(((tstyle) >> BG_SHIFT) & COLOR_MASK))
#define GET_FLAGS(tstyle)      ((int)((tstyle) & FLAGS_MASK))
#define HAS_FLAG(tstyle, flag) (((tstyle) & (flag)) != 0)

#define TEXT_STYLE_DEFAULT \
    MAKE_TEXT_STYLE(COLOR_DEFAULT, COLOR_DEFAULT, TEXT_STYLE_NONE)
#define TEXT_STYLE_INV     \
    MAKE_TEXT_STYLE(COLOR_DEFAULT, COLOR_DEFAULT, TEXT_STYLE_INVERT)

#define FORM_INFO_STYLE   \
    MAKE_TEXT_STYLE(COLOR_CYAN, COLOR_DEFAULT, TEXT_STYLE_NONE)
#define FORM_WARN_STYLE   \
    MAKE_TEXT_STYLE(COLOR_YELLOW, COLOR_DEFAULT, TEXT_STYLE_NONE)
#define FORM_ERR_STYLE    \
    MAKE_TEXT_STYLE(COLOR_RED, COLOR_DEFAULT, TEXT_STYLE_NONE)
#define FORM_PROMPT_STYLE \
    MAKE_TEXT_STYLE(COLOR_DEFAULT, COLOR_DEFAULT, TEXT_STYLE_INVERT)


/*** Dynamic array macros ***/

#define DA_INIT_CAPACITY 8

/* Initialize the dynamic array. */
#define daInit(arr) \
    daInitCap(arr, DA_INIT_CAPACITY)

/* Initialize the dynamic array with capacity. */
#define daInitCap(arr, cap)                                              \
    do {                                                                 \
        (arr)->count = 0;                                                \
        (arr)->capacity = (cap);                                         \
        (arr)->items = Malloc(sizeof(*(arr)->items) * (arr)->capacity); \
    } while (0)

/* Add an element to the dynamic array. */
#define daAdd(arr, elem)                                                  \
    do {                                                                  \
        if ((arr)->capacity == 0) daInit((arr));                          \
        if ((arr)->count >= (arr)->capacity) {                            \
            size_t newCap = (arr)->capacity * 2;                          \
            /* Check for capacity overflow */                             \
            if (newCap < (arr)->capacity ||                               \
                newCap > SIZE_MAX / sizeof(*(arr)->items)) {              \
                fprintf(stderr, "Capacity overflow in dynamic array.\n"); \
                exit(1);                                                  \
            }                                                             \
            (arr)->capacity = newCap;                                     \
            (arr)->items = Realloc((arr)->items,                         \
                    sizeof(*(arr)->items) * (arr)->capacity);             \
        }                                                                 \
        (arr)->items[(arr)->count++] = (elem);                            \
    } while (0)

/* Free the memory allocated for the dynamic array. */
#define daFree(arr)          \
    do {                     \
        free((arr)->items);  \
        (arr)->items = NULL; \
        (arr)->count = 0;    \
        (arr)->capacity = 0; \
    } while (0)

/* Free the memory allocated for the dynamic array and for each of its 
* element. */
#define daFreeEach(arr, func)                       \
    do {                                            \
        for (size_t i = 0; i < (arr)->count; i++) { \
            func((arr)->items[i]);                  \
        }                                           \
        daFree((arr));                              \
    } while (0)

/* =========================== Wrapper functions ============================ */

/* Print the error message using the provided string and the errno number, 
 * then exit. */
void die(const char *s) {
    perror(s);
    exit(1);
}

/* Print the error message informing the user that the window is too small, 
 * and terminate the program. */
void die_small_window(void) {
    static char *msg = "Your window is too skinny! Is your display living in "
            "the 1980s? Widen the border or buy a larger device to give this "
            "text some room to breathe.";
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/* Allocate 'size' bytes and return a pointer to the allocated memory.
 * Also checks for allocation failures. */
void *Malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "Out of memory allocating %zu bytes\n", size);
        exit(1);
    }
    return ptr;
}

/* Change the size of the memory block pointed by 'ptr' to 'size' bytes.
 * Also checks for reallocation failures. */
void *Realloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (new_ptr == NULL && size > 0) {
        fprintf(stderr, "Out of memory reallocating %zu bytes\n", size);
        exit(1);
    }
    return new_ptr;
}

/* Returns a pointer to a new string which is a duplicate of the string 's'.
 * It's up to the caller to free the allocated memory for the duplicate string.
 * Also checks for duplicate failures. */
char *Strdup(const char *s) {
    char *copy = strdup(s);
    if (copy == NULL) {
        fprintf(stderr, "Out of memory duplicating string `%s`\n", s);
        exit(1);
    }

    return copy;
}

/* Convert the calendar time 'ts' to broken-down time representation and 
 * store the data in the 'res' structure.
 * Return the address of the structure pointed to by 'res'. 
 * Also checks for function failures. */
struct tm *Localtime_r(const time_t *ts, struct tm *res) {
    if ((res = localtime_r(ts, res)) == NULL) die("localtime_r");
    return res;
}

/* Converts a broken-down time structure to calendar time representation.
 * Also checks for function failures. */
time_t Mktime(struct tm *tm) {
    tm->tm_isdst = -1;
    time_t ts = mktime(tm);
    if (ts == -1) die("mktime");
    return ts;
}

/* ================================ Terminal ================================ */

/* Restore the terminal to its original settings. */
void disableRawMode(void) {
    if (!Mycal.raw_mode_active) return;

    printf("\x1b[0m");   // Reset all mode
    printf("\x1b[?25h"); // Show cursor

    // Restore the original terminal attributes.
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &Mycal.orig_termios) == -1)
        die("tcsetattr");
    // Restore blocking mode for the file descriptor.
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags == -1) die("fcntl");
    if (fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) == -1) die("fcntl");

    Mycal.raw_mode_active = 0;
}

/* Configure the terminal for non-blocking, unbuffered input. */
void enableRawMode(void) {
    // Save the current terminal attributes.
    if (tcgetattr(STDIN_FILENO, &Mycal.orig_termios) == -1) die("tcgetattr");

    // Restore terminal settings on normal exit.
    if (atexit(disableRawMode) != 0) die("atexit");

    struct termios raw = Mycal.orig_termios;

    // Disable canonical mode to read byte-by-byte.
    // Disable ECHO so doesn't print to the screen.
    // Disable Ctrl-C signal.
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON); // Disable Ctrl-S/Ctrl-Q.

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");

    // Set the file descriptor to non-blocking.
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags == -1) die("fcntl");
    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == -1) die("fcntl");

    Mycal.raw_mode_active = 1;
}

/* Return the ANSI escape code of the foreground color 'c' if any, 
 * -1 otherwise. */
int getFGColor(Color c) {
    switch (c) {
        case COLOR_BLACK:   return 30;
        case COLOR_RED:     return 31;
        case COLOR_GREEN:   return 32;
        case COLOR_YELLOW:  return 33;
        case COLOR_BLUE:    return 34;
        case COLOR_MAGENTA: return 35;
        case COLOR_CYAN:    return 36;
        case COLOR_WHITE:   return 37;
        case COLOR_DEFAULT: return 39;
        default:            return -1;
    }
}

/* Return the ANSI escape code of the background color 'c' if any, 
 * -1 otherwise. */
int getBGColor(Color c) {
    switch (c) {
        case COLOR_BLACK:   return 40;
        case COLOR_RED:     return 41;
        case COLOR_GREEN:   return 42;
        case COLOR_YELLOW:  return 43;
        case COLOR_BLUE:    return 44;
        case COLOR_MAGENTA: return 45;
        case COLOR_CYAN:    return 46;
        case COLOR_WHITE:   return 47;
        case COLOR_DEFAULT: return 49;
        default:            return -1;
    }
}

/* Get the ANSI escape code string associated with the text style 'tstyle'. 
 * The funciton writes at most 'size'-1 bytes to the buffer.
 * Its up to the caller to set a large enough buffer for the whole string.
 * Return the length of the constructed string. */
int getTextStyleStr(char *buf, size_t size, TextStyle tstyle) {
    int len = snprintf(buf, size, "\x1b[0;%d;%d",
                       getFGColor(GET_FG(tstyle)),
                       getBGColor(GET_BG(tstyle)));
    if (len < 0 || (size_t)len >= size) return (int)size-1;

    // Macro to safely handle snprintf and buffer truncation
    #define SAFE_APPEND_STR(str)                                   \
        do {                                                       \
            int n = snprintf(buf+len, size-(size_t)len, (str));    \
            if (n > 0 && (size_t)len + (size_t)n < size) len += n; \
        } while (0)

    if (HAS_FLAG(tstyle, TEXT_STYLE_BOLD))      SAFE_APPEND_STR(";1");
    if (HAS_FLAG(tstyle, TEXT_STYLE_DIM))       SAFE_APPEND_STR(";2");
    if (HAS_FLAG(tstyle, TEXT_STYLE_ITALIC))    SAFE_APPEND_STR(";3");
    if (HAS_FLAG(tstyle, TEXT_STYLE_UNDERLINE)) SAFE_APPEND_STR(";4");
    if (HAS_FLAG(tstyle, TEXT_STYLE_BLINK))     SAFE_APPEND_STR(";5");
    if (HAS_FLAG(tstyle, TEXT_STYLE_INVERT))    SAFE_APPEND_STR(";7");
    if (HAS_FLAG(tstyle, TEXT_STYLE_HIDDEN))    SAFE_APPEND_STR(";8");

    SAFE_APPEND_STR("m");
    #undef SAFE_APPEND_STR
    return len;
}

/* Apply the text style mode 'tstyle' to standard output. */
void setTextStyle(TextStyle tstyle) {
    char buf[32];
    getTextStyleStr(buf, sizeof(buf), tstyle);
    printf("%s", buf);
}

/* Return the ANSI code of the character pressed or the integer value encoded 
 * by the key enumeration. 
 * Handles standard ANSI escape sequences too. */
int readKey(void) {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == ESC) {
        char seq[3];

        // Timeout/fail reading the next characters, just pressed the ESC key.
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return ESC;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return ESC;

        // Parse VT100 / ANSI escape sequences
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return ESC;
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return KEY_HOME;
                        case '3': return KEY_DEL;
                        case '4': return KEY_END;
                        case '5': return KEY_PAGE_UP;
                        case '6': return KEY_PAGE_DOWN;
                        case '7': return KEY_HOME;
                        case '8': return KEY_END;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return KEY_ARROW_UP;
                    case 'B': return KEY_ARROW_DOWN;
                    case 'C': return KEY_ARROW_RIGHT;
                    case 'D': return KEY_ARROW_LEFT;
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
        return ESC;
    }

    return c;
}

/* =========================== Terminal rendering =========================== */

/* Represent the buffer used to render the content in one-shot to the 
 * screen. */
struct rbuf {
    char *buf;
    int len;
};

/* Append the first 'len' characters of the string 'str' to the render 
 * buffer 'rb'. */
void rbufAppendStr(struct rbuf *rb, const char *str, int len) {
    rb->buf = Realloc(rb->buf, rb->len+len+1);

    memcpy(rb->buf + rb->len, str, len);
    rb->len += len;
    rb->buf[rb->len] = '\0';
}

/* Render the rows of the 'view' to the screen according to the renderer 
 * state. */
void viewRenderRows(View *view, Renderer *r) {
    struct rbuf rb = {NULL, 0};

    // To keep things efficient we append the new text style only if it
    // differs from the previous text style.
    TextStyle curr_style = TEXT_STYLE_DEFAULT;
    TextStyle prev_style = TEXT_STYLE_DEFAULT;

    char buf[32];
    int  len;

    if (r->y >= 0) {
        len = snprintf(buf, sizeof(buf), "\x1b[%dH", r->y+1);
        rbufAppendStr(&rb, buf, len); // set cursor to line y
    }

    int row = (r->vert_scroll) ? r->rowoff : 0;
    int col = (r->horz_scroll) ? r->coloff : 0;

    // Compute maximum row length for align block at center.
    int maxlen = 0;
    if (r->text_align == RENDERER_ALIGN_CENTER_BLOCK) {
        for (int i = row; i < row + r->screenrows && i < view->nrows; i++) {
            if (view->rows[i].len - col > maxlen) 
                maxlen = view->rows[i].len - col;
        }
    }

    // Append rows to render buffer.
    for (int y = 0; y < r->screenrows; y++) {
        if (r->x >= 0) {
            len = snprintf(buf, sizeof(buf), "\x1b[%dG", r->x+1);
            rbufAppendStr(&rb, buf, len); // set cursor to column x
        }

        col = (r->horz_scroll) ? r->coloff : 0;
        for (int x = 0; x < r->screencols; x++) {
            if (row >= view->nrows || col >= view->rows[row].len) break;

            if (x == 0) {
                int padding = 0;
                switch (r->text_align) {
                case RENDERER_ALIGN_START:
                    padding = 0;
                    break;
                case RENDERER_ALIGN_CENTER:
                    if (r->screencols > view->rows[row].len - col)
                        padding = (r->screencols - view->rows[row].len - col)/2;
                    break;
                case RENDERER_ALIGN_CENTER_BLOCK:
                    if (r->screencols > maxlen)
                        padding = (r->screencols - maxlen)/2;
                    break;
                case RENDERER_ALIGN_END:
                    if (r->screencols > view->rows[row].len - col)
                        padding = r->screencols - view->rows[row].len - col;
                    break;
                }

                if (padding > 0) {
                    // Set default style
                    curr_style = TEXT_STYLE_DEFAULT;
                    if (curr_style != prev_style) {
                        len = getTextStyleStr(buf, sizeof(buf), curr_style);
                        rbufAppendStr(&rb, buf, len);
                    }

                    // Append the padding
                    for (int i = 0; i < padding; i++) {
                        rbufAppendStr(&rb, " ", 1);
                    }

                    // Restore previous style
                    if (prev_style != curr_style) {
                        len = getTextStyleStr(buf, sizeof(buf), prev_style);
                        rbufAppendStr(&rb, buf, len);
                        curr_style = prev_style;
                    }
                }
            }

            // Append the new style only on changes.
            curr_style = view->rows[row].tstyle[col];
            if (curr_style != prev_style) { 
                len = getTextStyleStr(buf, sizeof(buf), curr_style);
                rbufAppendStr(&rb, buf, len);
                prev_style = curr_style;
            }

            // Append the character
            rbufAppendStr(&rb, &view->rows[row].chars[col], 1);
            col++;
        }

        // Append default style before ereasing line.
        if (curr_style != TEXT_STYLE_DEFAULT) {
            len = getTextStyleStr(buf, sizeof(buf), TEXT_STYLE_DEFAULT);
            rbufAppendStr(&rb, buf, len);
            prev_style = TEXT_STYLE_DEFAULT;
        }
        rbufAppendStr(&rb, "\x1b[0K", 4); // erase rest of line
        rbufAppendStr(&rb, "\n", 1);      // append new line
        row++;
    }

    // Restore default style at the end.
    len = getTextStyleStr(buf, sizeof(buf), TEXT_STYLE_DEFAULT);
    rbufAppendStr(&rb, buf, len);

    printf("%s", rb.buf);
    free(rb.buf);
}

/* ================================ Category ================================ */

/* Initialize and allocate memory for the category. 
 * Pass 0 to 'parent_id' if the category has no parent.
 * Return a pointer to the allocated memory. It's up to the caller to free 
 * this memory. */
Category *makeCategory(const char *name, Color color, size_t parent_id) {
    Category *cat = Malloc(sizeof(*cat));

    cat->id    = 0;
    cat->name  = name ? Strdup(name) : NULL;
    cat->color = color;
    cat->parent_id = parent_id;

    return cat;
}

/* Free the memory allocated for the category. */
void freeCategory(Category *cat) {
    free((void*)cat->name);
    free(cat);
}

/* Represent a node of the category tree. Here the children represent
 * the sub-categories of that category node. */
struct catnode {
    Category *cat;
    struct catnode **children;
    size_t child_count;
};

/* Represent a dynamic array of category nodes. */
typedef struct {
    struct catnode **items;
    size_t count;
    size_t capacity;
} CategoryNodeList;

/* Free the memory allocated for the category 'node'. */
void freeCategoryNode(struct catnode *node) {
    for (size_t i = 0; i < node->child_count; i++) {
        if (node->children[i]) freeCategoryNode(node->children[i]);
    }

    if (node->children) free(node->children);
    free(node);
}

/* ================================ Activity ================================ */

/* Construct the time string from the 'ts' timestamp using the 'fmt' 
 * format string.
 * The format string follows the libc `strftime` function specificiation 
 * formats.
 * At most 'bufsize'-1 bytes of the resulting string are stored in the buffer. 
 * Return the number of bytes written (excluding the NULL term). */
int getTimeStr(char *buf, size_t bufsize, const char *fmt, time_t ts) {
    struct tm time;
    Localtime_r(&ts, &time);
    return strftime(buf, bufsize, fmt, &time);
}

/* Initialize and allocate memory for the activity.
 * Return the memory of the allocated activity. It's up to the caller to free
 * this memory. */
Activity *makeActivity(const char *title, time_t start_ts, time_t end_ts, 
                       const char *notes, Category *category) 
{
    Activity *act = Malloc(sizeof(*act));

    act->title    = title ? Strdup(title) : NULL;
    act->start_ts = start_ts;
    act->end_ts   = end_ts;
    act->notes    = notes ? Strdup(notes) : NULL;
    act->category = category;

    return act;
}

/* Free the memory allocated for the activity. */
void freeActivity(Activity *act) {
    if (act->title)    free((void*)act->title);
    if (act->notes)    free((void*)act->notes);
    if (act->category) freeCategory(act->category);
    free(act);
}

/* ================================ Database ================================ */

/* Execute the 'sql' query on the database.
 * The 'callback' function (if not NULL) is called with the 'cb_arg' data 
 * for each returned row (if any) during the query execution.
 * The 'ftypes' format string indicate the format of the binding query
 * variables passed as variadic argument ('s': string, 'I': int64, 'i': int).
 * Return 1 on success, 0 on failure. */
int execQuery(sqlite3 *db, const char *sql,
               int(*callback)(sqlite3_stmt *, void *), void *cb_arg,
               const char *ftypes, ...) 
{
    sqlite3_stmt *stmt;
    int rc;

    /* Prepare the statement */
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n",
                sqlite3_errmsg(db));
        return 0;
    }

    /* Bind parameters */
    if (ftypes != NULL) {
        int arg_count = strlen(ftypes);

        va_list args;
        va_start(args, ftypes);

        for (int i = 0; i < arg_count; i++) {
            switch (ftypes[i]) {
                case 's':
                    sqlite3_bind_text(stmt, i+1, va_arg(args, char *), -1,
                            SQLITE_TRANSIENT);
                    break;
                case 'I':
                    sqlite3_bind_int64(stmt, i+1, va_arg(args, sqlite3_int64));
                    break;
                case 'i':
                    sqlite3_bind_int(stmt, i+1, va_arg(args, int));
                    break;
                default:
                    fprintf(stderr, "execQuery: Unknown format specifier %c\n", 
                            ftypes[i]);
                    va_end(args);
                    sqlite3_finalize(stmt);
                    return 0;
            }
        }

        va_end(args);
    }

    /* Execute and handle row */
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (callback != NULL) {
            if (!callback(stmt, cb_arg)) break;
        }
    }

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "Execution error: %s\n", sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 1 : 0;
}

/* Initialize the database and create the tables if they don't exist. 
 * The database tables are:
 *
 * activities
 * +------------------------------------------------------------------+
 * | id          INTEGER  Primary key                                 |
 * | title       TEXT     The title of the activity                   |
 * | start_time  INTEGER  Unix timestamp of the start of the activity |
 * | end_time    INTEGER  Unix timestamp of the end of the activity   |
 * | notes       TEXT     Additional notes                            |
 * +------------------------------------------------------------------+
 *
 * categories
 * +------------------------------------------------------------------+
 * | id         INTEGER  Primary key                                  |
 * | name       TEXT     Text label for the category                  |
 * | color      INTEGER  The number of the color in Color enumeration |
 * | parent_id  INTEGER  Foreign key to the categories table          |
 * +------------------------------------------------------------------+
 *
 * activity_category_map
 * +-----------------------------------------------------------+
 * | activity_id  INTEGER  Foreign key to the activities table |
 * | category_id  INTEGER  Foreign key to the categories table |
 * +-----------------------------------------------------------+
 *
 * Return 1 on success, 0 otherwise. */
int initDB(const char *filename, sqlite3 **db) {
    int rc = sqlite3_open(filename, db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(*db));
        return 0;
    }

    /* Enable foreign key for ON DELETE CASCADE */
    execQuery(*db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);

    const char *sql_activities = 
        "CREATE TABLE IF NOT EXISTS activities ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "title TEXT NOT NULL,"
        "start_time INTEGER NOT NULL,"
        "end_time INTEGER NOT NULL,"
        "notes TEXT NOT NULL,"
        "CHECK(end_time >= start_time)"
        ");";

    const char *sql_categories =
        "CREATE TABLE IF NOT EXISTS categories ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL UNIQUE,"
        "color INTEGER NOT NULL,"
        "parent_id INTEGER,"
        "FOREIGN KEY(parent_id) REFERENCES categories(id) ON DELETE CASCADE"
        ");";

    const char *sql_act_cat_map = 
        "CREATE TABLE IF NOT EXISTS activity_category_map ("
        "activity_id INTEGER,"
        "category_id INTEGER,"
        "PRIMARY KEY(activity_id, category_id),"
        "FOREIGN KEY(activity_id) REFERENCES activities(id) ON DELETE CASCADE,"
        "FOREIGN KEY(category_id) REFERENCES categories(id) ON DELETE CASCADE"
        ");";

    return execQuery(*db, sql_activities,  NULL, NULL, NULL, NULL) &&
           execQuery(*db, sql_categories,  NULL, NULL, NULL, NULL) &&
           execQuery(*db, sql_act_cat_map, NULL, NULL, NULL, NULL);
}

/* Insert a new activity to the database.
 * Return 1 on success, 0 otherwise. */
int insertActivity(sqlite3 *db, const Activity *act) {
    const char *sql = 
        "INSERT INTO activities (title, start_time, end_time, notes)"
        "VALUES (?, ?, ?, ?);";

    if (!execQuery(db, sql, NULL, NULL, "sIIs",
                     act->title, 
                     (sqlite3_int64)act->start_ts, 
                     (sqlite3_int64)act->end_ts, 
                     act->notes)) return 0;

    /* Map the category to the activity if a category is assigned. */
    if (act->category) {
        sqlite3_int64 last_id = sqlite3_last_insert_rowid(db);
        const char *sql_map =
            "INSERT INTO activity_category_map (activity_id, category_id)"
            "VALUES (?, ?);";
        return execQuery(db, sql_map, NULL, NULL, "II",
                         last_id, (sqlite3_int64)act->category->id);
    }

    return 1;
}

/* Remove an activity from the database using its 'id'. 
 * Return 1 on success, 0 otherwise. */
int deleteActivity(sqlite3 *db, size_t id) {
    const char *sql = "DELETE FROM activities WHERE id = ?;";

    return execQuery(db, sql, NULL, NULL, "I", (sqlite3_int64)id);
}

/* Edit an existing activity in the database using its id.
 * It sets the fileds of the 'src_act' activity to the activity having
 * identifier 'dst_id'.
 * Return 1 on success, 0 otherwise. */
int editActivity(sqlite3 *db, size_t dst_id, const Activity *src_act) {
    const char *sql = 
        "UPDATE activities SET "
        "title = ?, start_time = ?, end_time = ?, notes = ? "
        "WHERE id = ?;";

    if (!execQuery(db, sql, NULL, NULL, "sIIsI", 
                   src_act->title,
                   (sqlite3_int64)src_act->start_ts,
                   (sqlite3_int64)src_act->end_ts,
                   src_act->notes,
                   (sqlite3_int64)dst_id)) return 0;

    /* Update the category map. */
    const char *sql_del_map =
        "DELETE FROM activity_category_map "
        "WHERE activity_id = ?;";
    execQuery(db, sql_del_map, NULL, NULL, "I", dst_id);

    if (src_act->category) {
        const char *sql_map =
            "INSERT INTO activity_category_map (activity_id, category_id)"
            "VALUES (?, ?);";
        return execQuery(db, sql_map, NULL, NULL, "II",
                         dst_id, 
                         (sqlite3_int64)src_act->category->id);
    }

    return 1;
}

/* Add an activity to the provided argument activity list ('arg'). 
 * The activity is allocated and initialized using the database statement 
 * object. It's up to the caller to free this allocated memory. */
int activityCallback(sqlite3_stmt *stmt, void *arg) {
    ActivityList *list = (ActivityList *)arg;

    size_t id         = sqlite3_column_int64(stmt, 0);
    const char *title = (const char *)sqlite3_column_text(stmt, 1);
    time_t start_ts   = (time_t)sqlite3_column_int64(stmt, 2);
    time_t end_ts     = (time_t)sqlite3_column_int64(stmt, 3);
    const char *notes = (const char *)sqlite3_column_text(stmt, 4);

    // Parse category if exists.
    Category *cat = NULL;
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
        size_t cat_id    = sqlite3_column_int64(stmt, 5);
        char *cat_name   = (char *)sqlite3_column_text(stmt, 6);
        Color cat_color  = (Color)sqlite3_column_int(stmt, 7); 
        size_t parent_id = sqlite3_column_int64(stmt, 8);

        cat = makeCategory(cat_name, cat_color, parent_id);
        cat->id = cat_id;
    }

    Activity *act = makeActivity(title, start_ts, end_ts, notes, cat);
    act->id = id;

    daAdd(list, act);
    return 1;
}

/* Fill the activity list with the activity that occur in range (start, end).
 * The activities are sorted by start time in ascending order and (in case of
 * equal start) by end time in descending order.
 * Return 1 on success, 0 otherwise. */
int getActivitiesInRange(sqlite3 *db, time_t start, time_t end, 
                         ActivityList *list) 
{
    const char *sql =
        "SELECT a.id, a.title, a.start_time, a.end_time, a.notes, "
        "c.id AS cat_id, c.name AS cat_name, c.color AS cat_color, "
        "c.parent_id AS cat_parent_id "
        "FROM activities a "
        "LEFT JOIN activity_category_map m ON a.id = m.activity_id "
        "LEFT JOIN categories c ON m.category_id = c.id "
        "WHERE a.end_time >= ? AND a.start_time <= ? "
        "ORDER BY a.start_time ASC, a.end_time DESC;";

    return execQuery(db, sql, activityCallback, list, "II", 
                     (sqlite3_int64)start, 
                     (sqlite3_int64)end);
}

/* Fill the activity list with the last 'n' activities from timestamp 'ts'.
 * The activities are sorted by start time in descending order and (in case of
 * equal start) by end time in descending order.
 * Return 1 on success, 0 otherwise. */
int getLastActivities(sqlite3 *db, time_t ts, int n, ActivityList *list) {
    const char *sql = 
        "SELECT a.id, a.title, a.start_time, a.end_time, a.notes, "
        "c.id AS cat_id, c.name AS cat_name, c.color AS cat_color, "
        "c.parent_id AS cat_parent_id "
        "FROM activities a "
        "LEFT JOIN activity_category_map m ON a.id = m.activity_id "
        "LEFT JOIN categories c ON m.category_id = c.id "
        "WHERE a.start_time <= ? "
        "ORDER BY a.start_time DESC, a.end_time DESC "
        "LIMIT ?;";

    return execQuery(db, sql, activityCallback, list, "Ii", 
                     (sqlite3_int64)ts, n);
}

/* Insert a new category to the database.
 * If the parent_id is 0, it is as a root category.
 * Return 1 on success, 0 otherwise. */
int insertCategory(sqlite3 *db, const Category *cat) {
    if (cat->parent_id == 0) {
        const char *sql = 
            "INSERT INTO categories (name, color, parent_id)"
            "VALUES (?, ?, NULL);";
        return execQuery(db, sql, NULL, NULL, "si", 
                         cat->name, 
                         (int)cat->color);
    } else {
        const char *sql = 
            "INSERT INTO categories (name, color, parent_id)"
            "VALUES (?, ?, ?);";
        return execQuery(db, sql, NULL, NULL, "siI", 
                         cat->name, 
                         (int)cat->color, 
                         (sqlite3_int64)cat->parent_id);
    }
}

/* Edit an existing category in the database using its id.
 * It sets the fileds of the 'src_cat' category to the category having
 * identifier 'dst_id'.
 * Return 1 on success, 0 otherwise. */
int editCategory(sqlite3 *db, size_t dst_id, const Category *src_cat) {
    if (src_cat->parent_id == 0) {
        const char *sql = 
            "UPDATE categories SET "
            "name = ?, color = ?, parent_id = NULL "
            "WHERE id = ?;";
        return execQuery(db, sql, NULL, NULL, "siI",
                         src_cat->name,
                         (int)src_cat->color,
                         (sqlite3_int64)dst_id);
    } else {
        const char *sql = 
            "UPDATE categories SET "
            "name = ?, color = ?, parent_id = ? "
            "WHERE id = ?;";
        return execQuery(db, sql, NULL, NULL, "siII",
                         src_cat->name,
                         (int)src_cat->color,
                         (sqlite3_int64)src_cat->parent_id,
                         (sqlite3_int64)dst_id);
    }
}

/* Remove a category from the database using its id. 
 * If the 'cascade' flag is 1 it indicate to cascade delete all the
 * sub-categories.
 * Return 1 on success, 0 otherwise. */
int deleteCategory(sqlite3 *db, size_t id, int cascade) {
    if (!cascade) {
        /* Get the parent_id of the deleting category */
        Category *cat = getCategoryById(db, id);
        if (cat == NULL) return 0;

        size_t parent_id = cat->parent_id;
        freeCategory(cat);

        /* Reparent the children of the deleting category to the parent 
         * of the deleting category. */
        CategoryList child_list = {0};
        daInit(&child_list);

        if (!getCategoriesByParentId(db, id, &child_list)) {
            daFreeEach(&child_list, freeCategory);
            return 0;
        }

        for (size_t i = 0; i < child_list.count; i++) {
            Category *child = child_list.items[i];
            child->parent_id = parent_id;
            if (!editCategory(db, child->id, child)) {
                daFreeEach(&child_list, freeCategory);
                return 0;
            }
        }

        daFreeEach(&child_list, freeCategory);
    }

    const char *sql = "DELETE FROM categories WHERE id = ?;";
    return execQuery(db, sql, NULL, NULL, "I", (sqlite3_int64)id);
}

/* Add a category to the provided argument category node list ('arg'). 
 * The category is allocated and initialized using the database statement 
 * object. It's up to the caller to free this allocated memory. */
int categoryTreeCallback(sqlite3_stmt *stmt, void *arg) {
    CategoryNodeList *list = (CategoryNodeList *)arg;

    struct catnode *node = Malloc(sizeof(struct catnode));
    node->children = NULL;
    node->child_count = 0;
    
    size_t id        = sqlite3_column_int64(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    Color color      = (Color)sqlite3_column_int64(stmt, 2);

    size_t parent_id = 0;
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
        parent_id = sqlite3_column_int64(stmt, 3);
    }

    Category *cat = makeCategory(name, color, parent_id);
    cat->id   = id;
    node->cat = cat;

    daAdd(list, node);
    return 1;
}

/* Add a category to the provided argument category list ('arg'). 
 * The category is allocated and initialized using the database statement
 * object. It's up to the caller to free this allocated memory. */
int categoryListCallback(sqlite3_stmt *stmt, void *arg) {
    CategoryList *list = (CategoryList *)arg;

    size_t id        = sqlite3_column_int64(stmt, 0);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    Color color      = (Color)sqlite3_column_int64(stmt, 2);

    size_t parent_id = 0;
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
        parent_id = sqlite3_column_int64(stmt, 3);
    }

    Category *cat = makeCategory(name, color, parent_id);
    cat->id = id;

    daAdd(list, cat);
    return 1;
}

/* Binary search to find a parent category node by its 'id' into the sorted
 * category node list. 
 * Return the category node, if any, NULL otherwise. */
struct catnode *findNodeById(CategoryNodeList *list, size_t id) {
    int left = 0;
    int right = (int)list->count-1;

    while (left <= right) {
        int mid = left + (right-left) / 2;
        size_t mid_id = list->items[mid]->cat->id;

        if (mid_id == id) {
            return list->items[mid];
        } else if (mid_id < id) {
            left  = mid+1;
        } else {
            right = mid-1;
        }
    }

    return NULL;
}

/* Fetch categories form the database and build their trees.
 * The root of the trees are appended to the 'roots' list.
 * Return 1 on success, 0 otherwise. */
int getCategoryTrees(sqlite3 *db, CategoryNodeList *roots) {
    CategoryNodeList flat_list;
    daInit(&flat_list);

    /* Populate the sorted flat list. */
    const char *sql =
        "SELECT id, name, color, parent_id "
        "FROM categories "
        "ORDER BY id ASC;";

    if (!execQuery(db, sql, categoryTreeCallback, &flat_list, NULL)) {
        for (size_t i = 0; i < flat_list.count; i++) {
            freeCategoryNode(flat_list.items[i]);
        }
        daFree(&flat_list);
        return 0;
    }

    /* Link children to their respective parents. */
    for (size_t i = 0; i < flat_list.count; i++) {
        struct catnode *cnode = flat_list.items[i];

        // Root node
        if (cnode->cat->parent_id == 0) {
            daAdd(roots, cnode);
        } else {
            struct catnode *parent = findNodeById(&flat_list, 
                                                  cnode->cat->parent_id);
            if (parent != NULL) {
                parent->child_count++;
                parent->children = Realloc(parent->children,
                        sizeof(struct catnode *) * parent->child_count);
                parent->children[parent->child_count-1] = cnode;
            } else {
                daAdd(roots, cnode);
            }
        }
    }

    daFree(&flat_list);
    return 1;
}

/* Return the category with id 'id', if any, NULL otherwise. 
 * The returned category is allocated and initialized with the data fetched
 * from the database. It's up to the caller to free this allocated memory. */
Category *getCategoryById(sqlite3 *db, size_t id) {
    Category *cat = NULL;
    sqlite3_stmt *stmt;

    const char *sql = "SELECT name, color, parent_id "
                      "FROM categories "
                      "WHERE id = ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
       sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
       if (sqlite3_step(stmt) == SQLITE_ROW) {
           const char *name = (const char *)sqlite3_column_text(stmt, 0);
           Color color      = (Color)sqlite3_column_int(stmt, 1);
           size_t parent_id = (size_t)sqlite3_column_int64(stmt, 2);

           cat = makeCategory(name, color, parent_id);
           cat->id = id;
       }

       sqlite3_finalize(stmt);
    }

    return cat;
}

/* Get all the direct children of a category with id 'parent_id'.
 * The children are allocated and added one by one in the 'child_list'. 
 * It's up to the caller to free this allocated memory. 
 * Return 1 on success, 0 otherwise. */
int getCategoriesByParentId(sqlite3 *db, size_t parent_id, 
                            CategoryList *child_list) {
    const char *sql = 
        "SELECT id, name, color, parent_id "
        "FROM categories "
        "WHERE parent_id = ? "
        "ORDER BY name ASC;";

    return execQuery(db, sql, categoryListCallback, child_list, "I",
                     (sqlite3_int64)parent_id);
}

/* Check if a category name already exists in the database.
 * 'exclude_id' is used when we want to ignore a specific id from the 
 * searching process. Pass 0 to 'exclude_id' if the category has not an 
 * id yet.
 * Return 1 if the name exists, 0 otherwise. */
int categoryNameExists(sqlite3 *db, const char *name, size_t exclude_id) {
    int exists = 0;
    sqlite3_stmt *stmt;

    const char *sql = "SELECT 1 FROM categories "
                      "WHERE name = ? AND id != ? LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
       sqlite3_bind_text(stmt,  1, name, -1, SQLITE_TRANSIENT);
       sqlite3_bind_int64(stmt, 2, (sqlite3_int64)exclude_id);

       if (sqlite3_step(stmt) == SQLITE_ROW) exists = 1;
       sqlite3_finalize(stmt);
    }

    return exists;
}

/* Store the color used by the categories in the 'colors' array of capacity
 * 'cap'.
 * It's up to the caller to allocate a large-enough array for store all the
 * colors (at least a space per color type). 
 * Return the number of colors stored in the array. */
int getCategoryUsedColors(sqlite3 *db, Color *colors, size_t cap) {
    const char *sql = "SELECT DISTINCT color FROM categories ORDER BY color;";
    sqlite3_stmt *stmt;
    int count = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if ((size_t)count < cap) {
                colors[count++] = sqlite3_column_int(stmt, 0);
            }
        }

        sqlite3_finalize(stmt);
    }

    return count;
}

/* Return the first unused color from the categories of the database, if any.
 * Return a random color otherwise. 
 * We always exclude the COLOR_DEFAULT and the UNCAT_COLOR because it is 
 * used for all the activities that are uncategorized. */
Color getUnusedColor(void) {
    Color used_colors[__color_count];
    int count = getCategoryUsedColors(Mycal.db, used_colors, __color_count);
    assert(count <= __color_count);

    int is_used[__color_count] = {0};
    for (int i = 0; i < count; i++) {
        is_used[used_colors[i]] = 1;
    }

    /* Choose the first color that is not used. */
    for (int c = 0; c < __color_count; c++) {
        if (!is_used[c] && 
            (Color)c != COLOR_DEFAULT && 
            (Color)c != UNCAT_COLOR)
            return c;
    }


    /* All colors are used. Choose a random color. */
    Color color = (Color)(rand() % __color_count);
    while (color == COLOR_DEFAULT || color == UNCAT_COLOR) 
        color = (color+1) % __color_count;
    return color;
}

/* Return the number of actvities associated with the category having id 'id'.
 * Return 0 if the category has no activities or on failure. */
int countActivitiesForCategory(sqlite3 *db, size_t id) {
    int count = 0;
    sqlite3_stmt *stmt;

    const char *sql = "SELECT COUNT(*) FROM activity_category_map "
                      "WHERE category_id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }

        sqlite3_finalize(stmt);
    }

    return count;
}

/* =============================== Status bar =============================== */

/* Represent a status bar.
 * A status bar is a single line of text consisting of a prompt, an input 
 * field and a status.
 * The input includes a cursor that tracks the position from which to begin
 * typing characters.
 * +------------+-----------------+------+
 * |   PROMPT   |       INPUT    C|STATUS|
 * +------------+-----------------+------+
 *                               ^
 *                             cursor
 * If the cursor moves past the visible left or right borders, the input 
 * window slides to keep the cursor visible. */
struct status_bar {
    int screencols;         /* Number of columns */
    const char *prompt;     /* Prompt string */
    int prompt_len;         /* Number of characters in the prompt */
    const char *input;      /* Input string */
    int input_len;          /* Number of characters in the input */
    const char *status;     /* Status string */
    int status_len;         /* Number of characters in the status */

    int cursor_pos;         /* The logical index of the cursor in the
                             * input buffer. */ 
    int *input_scroll;      /* Track the current horizontal offset in the
                             * input buffer. */
};

/* Compute the maximum length of the prompt, input and status strings 
 * starting from their base lenght.
 * If any of the prompt, input or status string is longer than its allotted
 * space, it will be truncated. 
 *
 * The available space for each component is:
 * - 1/2 of the total space for the STATUS string
 * - 3/5 of the remaining (left) for the PROMPT
 * - 2/5 of the remaining (left) for the INPUT 
 * - 1 column for the CURSOR (C)
 *
 *     3/5      2/5           1/2
 * +---------+--------+------------------+
 * | PROMPT  |INPUT  C|      STATUS      |
 * +---------+--------+------------------+
 *
 * Store the length of the status bar components by overwriting the value
 * of the pointers provided as arguments. */
void getStatusBarLengths(int screencols, 
                         int *prompt_len, int *input_len, int *status_len) 
{
    if (*status_len > (screencols/2)) *status_len = screencols/2;

    int avail_left = screencols - *status_len;
    int max_prompt_len = (*input_len > 0) ? ((avail_left*3)/5) : avail_left;
    if (*prompt_len > max_prompt_len) *prompt_len = max_prompt_len;

    int max_input_len = avail_left - *prompt_len -1; // -1 for the cursor
    if (max_input_len < 0) max_input_len = 0;
    if (*input_len > max_input_len) *input_len = max_input_len;
}

/* Return 1 if the prompt or the right strings of the status bar are 
 * truncated, 0 otherwise. */
int isStatusBarOverflow(struct status_bar *sbar) {
    int max_prompt_len  = sbar->prompt_len;
    int max_input_len   = sbar->input_len;
    int max_status_len  = sbar->status_len;
    getStatusBarLengths(sbar->screencols, 
                        &max_prompt_len, 
                        &max_input_len, 
                        &max_status_len);

    return (max_prompt_len < sbar->prompt_len) || 
           (max_status_len < sbar->status_len);
}

/* Render a full-width status bar. 
 * Return the exact terminal column number where the physical cursor should 
 * be placed to align with the logical cursor. */
int renderStatusBar(struct status_bar *sbar, TextStyle style) {
    /* Get length of each component. */
    int max_prompt_len = sbar->prompt_len;
    int max_input_len  = sbar->input_len;
    int max_status_len = sbar->status_len;
    getStatusBarLengths(sbar->screencols, 
                        &max_prompt_len, 
                        &max_input_len, 
                        &max_status_len);
    int input_window_size = sbar->screencols-max_prompt_len-max_status_len-1;
    
    /* Calculate the scrolling offset for the sliding window of the input. */
    int input_off = 0;
    if (sbar->input_scroll) {
        /* Prevent out-of-bounds scrolling if the buffer shrank. */
        if (*sbar->input_scroll > sbar->input_len - input_window_size) {
            *sbar->input_scroll = sbar->input_len > input_window_size ? 
                sbar->input_len - input_window_size : 0;
        }

        /* Slide the window left or right if the cursor hits the boundaries. */
        if (sbar->cursor_pos < *sbar->input_scroll) {
            *sbar->input_scroll = sbar->cursor_pos;
        } else if (sbar->cursor_pos >= *sbar->input_scroll+input_window_size) {
            *sbar->input_scroll = sbar->cursor_pos - input_window_size;
        }
        input_off = *sbar->input_scroll;
    } else {
        /* If no scroll state is tracked, just show the tail of the string. */
        input_off = sbar->input_len > input_window_size ? 
            sbar->input_len - input_window_size : 0;
    }

    /* Set the physical column position for the terminal cursor. */
    int next_col = max_prompt_len + (sbar->cursor_pos - input_off) + 1;

    /* Render status bar */
    setTextStyle(style);

    if (sbar->prompt) printf("%.*s", max_prompt_len, sbar->prompt);
    if (sbar->input)  printf("%.*s", max_input_len, sbar->input+input_off);

    int padding = sbar->screencols-max_prompt_len-max_input_len-max_status_len;
    printf("%*s", padding, "");

    if (sbar->status) printf("%.*s", max_status_len, sbar->status);

    setTextStyle(TEXT_STYLE_DEFAULT);
    fflush(stdout);

    return next_col;
}

/* ================================== View ================================== */

/* Write the string and its style on the 'row' starting at 'off'. */
void vrowSet(struct vrow *row, int off, const char *str, int len, 
             TextStyle tstyle)
{
    if (len <= 0) return;
    assert(off >= 0 && off < row->size);

    // Truncate the string if exceed the row size.
    if (off + len > row->size) len = row->size - off;

    memcpy(row->chars+off, str, len);
    for (int i = 0; i < len; i++) row->tstyle[off + i] = tstyle;

    if (off + len > row->len) row->len = off + len;
}

/* Append the string and its style at end of the 'row'. */
void vrowAppend(struct vrow *row, const char *str, int len, TextStyle tstyle) 
{
    if (row->len + len > row->size) {
        int new_size = row->size == 0 ? 32 : row->size * 2;
        while (new_size < row->len + len) new_size *= 2;

        row->size   = new_size;
        row->chars  = Realloc(row->chars,  sizeof(char)      * row->size);
        row->tstyle = Realloc(row->tstyle, sizeof(TextStyle) * row->size);
    }

    vrowSet(row, row->len, str, len, tstyle);
}

/* Create space for 'n' new rows in the 'view' by reusing existing rows 
 * (if any) or allocating new ones as needed.
 * The added rows are initialized as empty. */
void viewAppendEmpty(View *view, int n) {
    assert(n >= 0);
    view->nrows += n;

    if (view->nrows > view->rows_size) {
        int nalloc = view->nrows - view->rows_size; 
        view->rows_size = view->nrows;
        view->rows = Realloc(view->rows, sizeof(struct vrow) *
                              view->rows_size);

        /* Initialize allocated rows. */
        for (int i = 0; i < nalloc; i++) {
            struct vrow *row = &view->rows[view->nrows - nalloc + i];
            row->len    = 0;
            row->size   = 0;
            row->chars  = NULL;
            row->tstyle = NULL;
        }
    }

    /* Empty added rows. */
    for (int i = 0; i < n; i++) {
        struct vrow *row = &view->rows[view->nrows - n + i];
        row->len = 0;
    }
}

/* Append the row constructed by the string and its style at end of 'view'. 
 * It also resizes the view if the string length overflows the view width. */
void viewAppendRow(View *view, const char *str, int len, TextStyle tstyle) {
    viewAppendEmpty(view, 1);

    struct vrow *row = &view->rows[view->nrows - 1];
    vrowAppend(row, str, len, tstyle);
    if (len > view->ncols) view->ncols = len;
}

/* Append the row constructed by the string and its style at end of 'view'. 
 * It handles line overflow by wrapping the exceeding part to next rows. */
void viewAppendRowWrap(View *view, const char *str, int len, TextStyle tstyle) 
{
    int n = ROUND_INT_DIV(len, view->ncols);
    viewAppendEmpty(view, n);

    for (int i = 0; i < n; i++) {
        struct vrow *row = &view->rows[view->nrows - n + i];

        int write_len = len;
        if (write_len > view->ncols) write_len = view->ncols;
        vrowAppend(row, str+(i*view->ncols), write_len, tstyle);
        len -= write_len;
    }
}

/* Copy the 'src_row' at row position 'dst_index' of the view.
 * Overwrites the contents of the desination row, if any. */
void viewCopyRow(View *view, int dst_index, struct vrow *src_row) {
    assert(dst_index >= 0 && dst_index <= view->nrows-1);

    struct vrow *dst_row = &view->rows[dst_index];
    if (dst_row->chars == NULL || dst_row->tstyle == NULL || 
        dst_row->size < src_row->len) {
        dst_row->size = src_row->len;
        dst_row->chars = Realloc(dst_row->chars, sizeof(char) *
                dst_row->size);
        dst_row->tstyle = Realloc(dst_row->tstyle, sizeof(TextStyle) *
                dst_row->size);
    }

    memcpy(dst_row->chars,  src_row->chars,  sizeof(char) * src_row->len);
    memcpy(dst_row->tstyle, src_row->tstyle, sizeof(TextStyle) * src_row->len);
    dst_row->len = src_row->len;
}

/* Initialize the consecutive cells of the 'grid' with the daily start and
 * end timestamps. */
void initDailyGridCellTimes(struct row_grid *grid, time_t start_ts) {
    struct tm day;
    Localtime_r(&start_ts, &day);
    for (int i = 0; i < grid->cell_count; i++) {
        time_t day_ts = Mktime(&day);
        getDayBounds(day_ts, &grid->cells[i].start_ts, &grid->cells[i].end_ts);
        day.tm_mday += 1;
    }
}

/* Initialize and allocate memory for the 'grid' with 'cell_count' cells. 
 * The grid has a maximum width of 'width' columns and between each pairs of
 * cells in the grid is placed a delimiter having 'delim_width' width . 
 * It's up to the caller to free the allocated memory. */
void initGrid(struct row_grid *grid, int cell_count, 
              int width, int delim_width, time_t start_ts) 
{
    grid->cell_count  = cell_count;
    grid->delim_width = delim_width;
    if (grid->cell_count == 0) return;

    grid->cells = Malloc(sizeof(struct grid_cell) * grid->cell_count);

    /* Compute the offset and width of each cell.
     * If the total width is not perfectly divisible by the number 
     * of cells, distribute the extra spaces by the first cells. */
    int delim_count = grid->cell_count-1;
    int cell_width  = (width - delim_count*delim_width) / grid->cell_count;
    int cell_rest   = (width - delim_count*delim_width) % grid->cell_count;

    int offset = 0;
    for (int i = 0; i < cell_count; i++) {
        struct grid_cell *cell = &grid->cells[i];
        cell->start_ts = 0;
        cell->end_ts   = 0;
        cell->width    = cell_width;
        if (cell_rest > 0) {
            cell->width += 1;
            cell_rest--;
        }

        cell->offset = offset;
        offset += cell->width + delim_width;
    }

    initDailyGridCellTimes(grid, start_ts);
}

/* Set the text and style at center of the cell grid in the 'row' starting 
 * from 'offset'. 
 * Return the start column of the writted string. */
int vrowSetGridCellText(struct vrow *row, struct grid_cell *cell,
                        const char *str, int len, TextStyle tstyle, 
                        int offset) 
{
    if (len > cell->width) len = cell->width;
    int pad = (cell->width - len) / 2;
    int off = offset + cell->offset + pad;
    vrowSet(row, off, str, len, tstyle);
    return off;
}

/* Free the memory allocated for the 'grid'. */
void freeGrid(struct row_grid *grid) {
    free(grid->cells);
    grid->cells = NULL;
    grid->cell_count = 0;
}

/* Initialize and allocate memory for the 'view'. */
void initView(View *view, ViewType type, int ncols, int nrows,
              void **entities, size_t count) 
{
    view->type      = type;
    view->ncols     = ncols;
    view->nrows     = nrows;
    view->rows_size = nrows;
    view->rows      = NULL;

    view->entities     = entities;
    view->entity_count = count;

    if (view->entity_count == 0) {
        view->bbox_lists = NULL;
    } else {
        view->bbox_lists = Malloc(sizeof(struct bbox_list) * count);
        for (size_t i = 0; i < view->entity_count; i++) {
            daInit(&view->bbox_lists[i]);
        }
    }
}

/* Free the memory allocated for the 'view'. */
void freeView(View *view) {
    if (view->rows) {
        for (int i = 0; i < view->rows_size; i++) {
            free(view->rows[i].chars);
            free(view->rows[i].tstyle);
        }
        free(view->rows);
    }

    if (view->entities) {
        for (size_t i = 0; i < view->entity_count; i++) {
            switch (view->type) {
                case VIEW_ACTIVITY:
                    freeActivity(view->entities[i]);
                    break;
                case VIEW_CATEGORY:
                    freeCategory(view->entities[i]);
                    break;
                default:
                    break;
            }
        }
        free(view->entities);
        view->entity_count = 0;
    }

    if (view->type == VIEW_ACTIVITY) {
        freeGrid(&view->as.act.grid);
    } else if (view->type == VIEW_CATEGORY && view->as.cat.depths) {
        free(view->as.cat.depths);
    }

    if (view->bbox_lists) {
        for (size_t i = 0; i < view->entity_count; i++) {
            daFree(&view->bbox_lists[i]);
        }
        free(view->bbox_lists);
    }
}

/* Clear the rows of the 'view' without freeing their memory. */
void clearView(View *view) {
    for (int i = 0; i < view->nrows; i++) {
        view->rows[i].len = 0;
    }
    view->nrows = 0;
}

/* Return the string associated with the activity view 'type'. */
const char *getActivityViewString(enum act_view_type type) {
    switch(type) {
    case ACTIVITY_VIEW_DAY:   return "daily";
    case ACTIVITY_VIEW_WEEK:  return "weekly";
    case ACTIVITY_VIEW_MONTH: return "monthly";
    case ACTIVITY_VIEW_YEAR:  return "yearly";
    default:                  return NULL;
    }
}

/* Add the category and its 'bbox_count' bounding boxes to the view at 
 * position 'index'. 
 * If the 'index' is not at the extremes of the entity array (0,entity_count),
 * then the category is inserted at position 'index' and all the entities
 * between [index-entity_count-1] are shifted. 
 * 'depth' is the level of the category in its category tree. 0 if is the root
 * of the tree. */
void viewAddCategory(View *view, int index, Category *cat, 
                     struct bounding_box *bboxes, int bbox_count, int depth)
{
    assert(index >= 0 && (size_t)index <= view->entity_count);

    // Create room for the new category.
    size_t size = view->entity_count + 1;
    view->entities   = Realloc(view->entities, sizeof(void *) * size);
    view->bbox_lists = Realloc(view->bbox_lists, sizeof(struct bbox_list) 
                               * size);
    view->as.cat.depths = Realloc(view->as.cat.depths, sizeof(int) * size);

    // Shift entities to the right to make room for the new category.
    if (index < (int)view->entity_count) {
        memmove(&view->entities[index + 1], &view->entities[index],
                sizeof(void *) * (view->entity_count - index));
        memmove(&view->bbox_lists[index + 1], &view->bbox_lists[index],
                sizeof(struct bbox_list) * (view->entity_count - index));
        memmove(&view->as.cat.depths[index + 1], 
                &view->as.cat.depths[index],
                sizeof(int) * (view->entity_count - index));

        // Recompute the bounding boxes position of the entities after
        // the inserted one.
        int total_height = 0;
        for (int b = 0; b < bbox_count; b++) {
            total_height += bboxes[b].height;
        }

        for (size_t j = index + 1; j < view->entity_count + 1; j++) {
            for (size_t nb = 0; nb < view->bbox_lists[j].count; nb++) {
                view->bbox_lists[j].items[nb].y += total_height;
            }
        }
    }

    // Add the category.
    view->entities[index] = cat;
    daInit(&view->bbox_lists[index]);
    for (int b = 0; b < bbox_count; b++) {
        daAdd(&view->bbox_lists[index], bboxes[b]);
    }
    view->as.cat.depths[index] = depth;
    view->entity_count++;
}

/* Remove the category with id 'id' from the 'view'. 
 * Return the removed entity if any, NULL otherwise. */
void *viewRemoveCategory(View *view, size_t id) {
    // Search the index of the category to remove.
    size_t i;
    for (i = 0; i < view->entity_count; i++) {
        if (((Category *)view->entities[i])->id == id) break;
    }

    if (i == view->entity_count) return NULL; // category not found

    Category *removed = view->entities[i];

    if (i < view->entity_count-1) {
        int total_height = 0;
        for (size_t b = 0; b < view->bbox_lists[i].count; b++) {
            total_height += view->bbox_lists[i].items[b].height;
        }

        int in_tree = 1;
        for (size_t j = i+1; j < view->entity_count; j++) {
            // Recompute the bounding boxes position of the categories 
            // after the removed one.
            for (size_t nb = 0; nb < view->bbox_lists[j].count; nb++) {
                view->bbox_lists[j].items[nb].y -= total_height;
            }

            // Recompute the depth of the categories of the sub-tree after 
            // the removed one.
            if (in_tree) {
                if (view->as.cat.depths[j] > view->as.cat.depths[i]) {
                    view->as.cat.depths[j]--;
                } else { 
                    in_tree = 0;
                }
            }
        }
    }

    daFree(&view->bbox_lists[i]);

    if (i < view->entity_count-1) {
        // Shift elements to the left for remove the category.
        memmove(&view->entities[i], &view->entities[i + 1], 
                sizeof(void *) * (view->entity_count - i - 1));
        memmove(&view->bbox_lists[i], &view->bbox_lists[i + 1], 
                sizeof(struct bbox_list) * (view->entity_count - i - 1));
        memmove(&view->as.cat.depths[i], 
                &view->as.cat.depths[i + 1], 
                sizeof(int) * (view->entity_count - i - 1));
    }
    
    view->entity_count--;
    return removed;
}

/* Place the 'width' x 'height' entity in 'view' at position (x,y). 
 * The 'text' of the entity is written starting from (0,0) and wrapped to the 
 * box sizes.
 * The full box of the entity uses the text 'style'. */
void placeEntityInView(View *view, const char *text, TextStyle style,
                         int x, int y, int width, int height) 
{
    /* Compute text dimensions. */
    int text_len    = text ? strlen(text) : 0;
    if (width <= 0) die_small_window();
    int text_height = ROUND_INT_DIV(text_len, width);
    if (text_height > height) text_height = height;

    /* Place entity in view rows. */
    int len = 0;
    for (int y0 = 0; y0 < height; y0++) {
        int chars_to_write = 0;

        // Write title
        if (y0 < text_height && len < text_len) {
            int chars_left = text_len - len;
            chars_to_write = (chars_left < width) ? chars_left : width;

            vrowSet(&view->rows[y+y0], x, &text[len], chars_to_write,
                    style);
            len += chars_to_write;
        }

        // Write spaces
        int nspaces = width - chars_to_write;
        char spaces[nspaces+1];
        memset(spaces, ' ', nspaces);
        spaces[nspaces] = '\0';

        vrowSet(&view->rows[y+y0], x + chars_to_write, spaces, nspaces, style);
    }
}

/* Place the 'i'-th activity in 'view'. */
void placeActivityInView(View *view, int i) {
    assert(i >= 0 && (size_t)i < view->entity_count);
    Activity *act = view->entities[i];

    Color act_bgcolor = act->category ? act->category->color : UNCAT_COLOR;
    Color act_fgcolor = act_bgcolor == COLOR_WHITE ? COLOR_BLACK : COLOR_DEFAULT;

    TextStyle title_style = MAKE_TEXT_STYLE(act_fgcolor, 
                                            act_bgcolor,
                                            TEXT_STYLE_BOLD);
    TextStyle time_style = MAKE_TEXT_STYLE(act_fgcolor, 
                                           act_bgcolor,
                                           TEXT_STYLE_NONE);
    TextStyle category_style = MAKE_TEXT_STYLE(act_fgcolor,
                                               act_bgcolor,
                                               TEXT_STYLE_ITALIC);

    for (size_t b = 0; b < view->bbox_lists[i].count; b++) {
        struct bounding_box act_box = view->bbox_lists[i].items[b];
        if (act_box.width <= 0) die_small_window();

        /* Deduce which grid cells this specific bounding box belongs to. */
        struct row_grid *grid = &view->as.act.grid;
        struct grid_cell *start_cell = NULL;
        struct grid_cell *end_cell   = NULL;

        int box_left  = act_box.x;
        int box_right = act_box.x + act_box.width - 1;

        for (int c = 0; c < grid->cell_count; c++) {
            int cell_left  = grid->cells[c].offset + view->as.act.body_off;
            int cell_right = cell_left + grid->cells[c].width;

            if (box_left >= cell_left && box_left < cell_right)
                start_cell = &grid->cells[c];

            if (box_right >= cell_left && box_right < cell_right)
                end_cell = &grid->cells[c];

            if (start_cell && end_cell) break;
        }

        /* Clamp the activity timestamp to the cell bounds. */
        time_t start_ts = act->start_ts;
        time_t end_ts   = act->end_ts;

        if (start_cell && start_ts < start_cell->start_ts) 
            start_ts = start_cell->start_ts;
        if (end_cell && end_ts > end_cell->end_ts) 
            end_ts = end_cell->end_ts;

        /* Write the activity title in at least 1 row. 
         * Write the activity time interval in the following rows.
         * Finally, write the category name. */
        int rows = act_box.height;
        int rest_rows = act_box.height;

        /* Title */
        int title_rows = ROUND_INT_DIV(strlen(act->title), act_box.width);
        if (title_rows >= rest_rows) title_rows = rest_rows;

        placeEntityInView(view, act->title, title_style, 
                          act_box.x, act_box.y+(rows-rest_rows), 
                          act_box.width, title_rows);

        rest_rows -= title_rows;

        /* Time */
        char start_str[16], end_str[16];
        char time_str[16 + 16];
        int start_str_len = getTimeStr(start_str, sizeof(start_str),
                                       ACTIVITY_TIME_FMT, start_ts);
        int end_str_len   = getTimeStr(end_str, sizeof(end_str), 
                                       ACTIVITY_TIME_FMT, end_ts);
        int time_str_len  = snprintf(time_str, sizeof(time_str), "%s-%s", 
                                     start_str, end_str);

        int time_rows = rest_rows;

        // Write the time string.
        if (time_str_len <= act_box.width) {
            if (time_rows > 1) time_rows = 1;
            placeEntityInView(view, time_str, time_style, 
                              act_box.x, act_box.y+(rows-rest_rows), 
                              act_box.width, time_rows);
            rest_rows -= time_rows;
        } 
        // The time string overflows write only the start time.
        else if (start_str_len <= act_box.width) {
            if (time_rows > 1) time_rows = 1;
            placeEntityInView(view, start_str, time_style, 
                              act_box.x, act_box.y+(rows-rest_rows), 
                              act_box.width, time_rows);
            rest_rows -= time_rows;

            // Write the end time below the start time.
            time_rows = rest_rows;
            if (end_str_len <= act_box.width) {
                if (time_rows > 1) time_rows = 1;
                placeEntityInView(view, end_str, time_style, 
                                  act_box.x, act_box.y+(rows-rest_rows), 
                                  act_box.width, time_rows);
                rest_rows -= time_rows;
            }
        }

        /* Category */
        const char *cat_str = act->category ? act->category->name : NULL;
        int category_rows = cat_str ? 
            ROUND_INT_DIV(strlen(cat_str), act_box.width) : 0;
        if (category_rows >= rest_rows) category_rows = rest_rows;

        placeEntityInView(view, cat_str, category_style,
                          act_box.x, act_box.y+(rows-rest_rows), 
                          act_box.width, category_rows);

        rest_rows -= category_rows;

        /* Remaining rows */
        placeEntityInView(view, NULL, title_style, 
                          act_box.x, act_box.y+(rows-rest_rows), 
                          act_box.width, rest_rows);

    }
}

/* ============================= Time slot view ============================= */

/* Return the total number of time slots of size 'tslot_size' required for 
 * cover all the time in the inverval ['start_ts', 'end_ts']. */
int getTimeSlotCount(time_t start_ts, time_t end_ts, int tslot_size) {
    struct tm start, end;
    Localtime_r(&start_ts, &start);
    Localtime_r(&end_ts,   &end);

    int start_total_mins = (start.tm_hour * MINUTES_IN_HOUR) + start.tm_min;
    int end_total_mins   = (end.tm_hour   * MINUTES_IN_HOUR) + end.tm_min;
    return ROUND_INT_DIV(end_total_mins - start_total_mins, tslot_size);
}

/* Compute the time slot index range (y0, y1) for an activity on a specific
 * day. 
 * 'y0' is clamped to 0 is the activity starts before the day.
 * 'y1' is clamped to 'body_rows' is the activity ends after the day. 
 * y0 and y1 values are stored in the pointer provided as arugments. */
void getActivitySlotRange(View *view, Activity *act, 
                          time_t day_start_ts, time_t day_end_ts,
                          int *y0, int *y1) 
{
    struct tm day_start, act_start, act_end;
    Localtime_r(&day_start_ts, &day_start);
    Localtime_r(&act->start_ts, &act_start);
    Localtime_r(&act->end_ts, &act_end);

    int tslot_per_hour = MINUTES_IN_HOUR / view->as.act.tslot_size;

    // Check if activity starts on this day
    if (act->start_ts >= day_start_ts) {
        int hour_diff = act_start.tm_hour - day_start.tm_hour;
        *y0 = (hour_diff * tslot_per_hour) + 
              (act_start.tm_min / view->as.act.tslot_size);
    } else {
        *y0 = 0;
    }

    // Check if activity ends on this day
    if (act->end_ts <= day_end_ts) {
        int hour_diff = act_end.tm_hour - day_start.tm_hour;
        *y1 = (hour_diff * tslot_per_hour) + 
              ROUND_INT_DIV(act_end.tm_min, view->as.act.tslot_size);
    } else {
        *y1 = view->as.act.body_rows;
    }
}

/* Check if two activities share the same time slots on a specific day. 
 * Return 1 on success, 0 otherwise. */
int activitiesShareTimeSlots(View *view, Activity *act1, Activity *act2, 
                             time_t day_start_ts, time_t day_end_ts) 
{
    int act1_y0, act1_y1, act2_y0, act2_y1;
    getActivitySlotRange(view, act1, 
                         day_start_ts, day_end_ts, 
                         &act1_y0, &act1_y1);
    getActivitySlotRange(view, act2, 
                         day_start_ts, day_end_ts, 
                         &act2_y0, &act2_y1);
    return (act1_y0 < act2_y1) && (act2_y0 < act1_y1);
}

/* Compute the index and span for the activities considering overlaps.
 * Two activities overlaps if they share the same time intervals.
 *
 * Fills the 'col_idxs' and 'col_spans' arrays with the column index and 
 * the column width for the activities, respectively.
 * These variables use an abstract coordinate system to define an activity's 
 * placement. They do not represent final physical dimensions but rather 
 * relative to a grid of columns.
 *
 * indexes:   0   1   2   3   4   ...
 *          +---+-------+---+---+
 *          | a |   b   | c | d | ...
 *          +---+-------+---+---+
 * spans:     1     2     1   1   ...
 *
 * 'acts' array is sorted by start time and (in case of equal start) by 
 *        end time. 
 *
 * The activities with fewer overlaps get more space. 
 * Here we try to minimize the number of overlapping columns always placing 
 * the overlapping activities to a previous column when possible (see the
 * implementation comments of this cases for more detail). */
void activityOverlaps(View *view, Activity **acts, int count,
                      int *col_idxs, int *col_spans,
                      time_t day_start_ts, time_t day_end_ts) 
{
    int max_col_idx = 0; // Maximum column index avilable
    int is_pinned = 0;   // 1 if the activity position is fixed 
                         // (cannot go back)
    int occupied[count]; // for each column store the index of the 
                         // activity that occupies the column if any, 
                         // -1 otherwise
    int is_last_col[count]; // 1 if the i-th activity is the final activity
                            // in the column (no activities follow it)

    for (int i = 0; i < count; i++) {
        col_idxs[i]  = 0;
        col_spans[i] = 1;
        is_pinned = 0;
        is_last_col[i] = 1;

        for (int k = 0; k < count; k++) {
            occupied[k] = -1;
        }
        for (int j = 0; j < i; j++) {
            /* Overlap */
            if (RANGES_OVERLAP(acts[i]->start_ts, acts[i]->end_ts, 
                               acts[j]->start_ts, acts[j]->end_ts) ||
                activitiesShareTimeSlots(view, acts[i], acts[j], 
                                         day_start_ts, day_end_ts)) {
                for (int s = 0; s < col_spans[j]; s++) {
                    occupied[col_idxs[j] + s] = j;
                }

                /* Update column index. */
                if (!is_pinned || col_idxs[i] == col_idxs[j]) {

                    /* We are overlapping two activities ('i' and 'j')
                     * that are fixed at the same position. 
                     *               |     |                  |     |
                     *  +-----------+|     |     +-----+-----+|     |
                     *  |    j/i    ||  *  | ==> |  j  |  i  ||  *  |
                     *  +-----------+|     |     +-----+-----+|     |
                     *               |     |                  |     |
                     * Here 'j' can share some of it's space with 'i',
                     * if any. Otherwise 'i' should be placed elsewhere
                     * after 'j'. */ 
                    if (is_pinned && col_idxs[i] == col_idxs[j]) {

                        /* Since 'i' is pinned, its span indicate the
                         * available space for all the overlapping 
                         * activities between 'j' and 'i'.
                         *                |   |                 |   |
                         *  +------+-----+|   |   +----+---+---+|   |
                         *  |  j   |  *  || * |   | j  | * | * || * |
                         *  +------+-----+|   |   +----+---+---+|   |
                         *  <<<< span >>>>|   |   <<<< span >>>>|   |
                         *
                         *  So, to compute the number of overlapping 
                         *  activities from 'j' (included) to 'i' (excluded)
                         *  we use the formula:
                         *   (avail_span / j_span) + 
                         *     ((avail_span % j_span) != 0)
                         *  where ((avail_span % j_span) != 0) adds 1 if the
                         *  (avail_span / j_span) division is not perfect. */
                        int avail_span = col_spans[i];
                        int group_count = (avail_span / col_spans[j])
                                          + ((avail_span % col_spans[j]) != 0);

                        // There is a rooms for the activity.
                        if (avail_span > group_count) {
                            // Update the 'j' span to prepare for the 
                            // insertion of 'i'.
                            int n = group_count+1;
                            int new_span = (avail_span / n)
                                           + ((avail_span % n) != 0);
                            int gap = col_spans[j] - new_span;
                            col_spans[j] = new_span;

                            // Remove the occupied columns from 'j' gap
                            for (int g = 0; g < gap; g++) {
                                occupied[col_idxs[j] + col_spans[j] + g] = -1;
                            }

                            // Doing this shrinking causes an incorrect 
                            // index and span for subsequent group 
                            // activities.
                            // +-------+-------+ 
                            // |   j   |   k   | 
                            // +-------+-------+ 
                            // +-----+-+-------+
                            // |  j  | |   k   |
                            // +-----+-+-------+
                            //        ^
                            //      free
                            //
                            // So we adjust the next activity index and
                            // span by the amount removed from 'j'.
                            // This allows us to repeat this process over
                            // and over again.
                            // +-------+-------+ 
                            // |   j   |   k   | 
                            // +-------+-------+ 
                            // +----+----------+
                            // | j  |    k     |
                            // +----+----------+
                            // +-----+----+----+
                            // |  j  | k  | i  |
                            // +-----+----+----+
                            if (group_count > 1) {
                                // There are other activities in the group
                                // after 'j'.
                                int k = j+1;
                                col_idxs[k]  -= gap;
                                col_spans[k] += gap;
                                // Update the occupied columns
                                // REDUNDANT (done on next iteration)
                            }
                        }

                        col_spans[i] -= col_spans[j];
                        if (col_spans[i] < 1) col_spans[i] = 1;
                    }

                    /* Place activity 'i' after the overlapping 
                     * activity 'j' */
                    int next_idx = col_idxs[j]+col_spans[j];

                    /* Occupied is used to skip through the totems.
                     * A totem is a previous long activity that the 
                     * current one collides with. 
                     *  |           ||     |
                     *  +-----------+|     |
                     *  +-----+-----+|totem|+-----+
                     *  |  *  |  j  ||     ||  i  |
                     *  +-----+-----+|     |+-----+
                     * */
                    while (occupied[next_idx] != -1 &&
                           /* check for overlapping activity group case */
                           occupied[next_idx] != j+1) {
                        is_last_col[occupied[next_idx]] = 0;
                        next_idx += col_spans[occupied[next_idx]];
                    }

                    if (next_idx > max_col_idx) {
                        max_col_idx = next_idx;
                    }
                    col_idxs[i] = next_idx;
                    is_last_col[j] = 0;

                    // Check if the next columns are free.
                    int all_free = 1;
                    for (int n = col_idxs[i]+col_spans[i]; 
                         n <= max_col_idx; 
                         n++) {
                        if (occupied[n] != -1) {
                            all_free = 0;
                            break;
                        }
                    }
                    if (all_free) is_last_col[i] = 1;
                }

                /* Update column span.
                 * 'i' is overwriting activity 'j' that has column 
                 * greater then its own. We set 'i's span as the 
                 * difference of the positions between 'i' and 'j'.
                 *          +-----+                 +-----+
                 *          |     |                 |     |
                 *          |  j  |         < span >|  j  |
                 *  +-------------+   ==>   +------+|     |
                 *  |             |         |      |+-----+
                 *  |      i      |         |  i   |
                 *  |             |         |      |
                 *  +-------------+         +------+
                 */
                if (col_idxs[i] < col_idxs[j] &&
                    col_idxs[i]+col_spans[i] > col_idxs[j]) {
                    col_spans[i] = col_idxs[j] - col_idxs[i];
                    is_last_col[i] = 0;
                }
            }
            /* Do not overlap */
            else {
                /* Fix 'i' to the same position of the FIRST 
                 * non-overlapping 'j' (they are on the same column).
                 * Here 'i' gets all the available space.
                 *  |     |+-----+-----+
                 *  |     ||  j  | max |
                 *  |  *  |+-----+-----+
                 *  |     |+-----------+
                 *  |     ||     i     |
                 *  |     |+-----------+       
                 *  |     |<<< span >>>>
                 */
                if (!is_pinned) {
                    col_idxs[i] = col_idxs[j];
                    col_spans[i] = (max_col_idx+1) - col_idxs[j];
                    is_pinned = 1;
                }
            }
        }
    }
    
    /* Extend the span of the last column activities that do not 
     * reach the maximum column. 
     *  +-----+             +-----------+
     *  |  i  |             |     i     |
     *  +-----+        ==>  +-----------+
     *  +-----+-----+       +-----+-----+
     *  |  *  | max |       |  *  | max |
     *  +-----+-----+       +-----+-----+
     */
    for (int i = 0; i < count; i++) {
        if (is_last_col[i]) {
            col_spans[i] = (max_col_idx+1) - col_idxs[i];
        }
    }
}

/* Arrange the 'count' 'activities' within a time-slot based 'view' by 
 * computing their bounding boxes considering overlaps.
 *
 * 'tslot_off' indicate the base offset of the time slot.
 * 'tslot_len' indicate the total number of columns for the time slot.
 * 'day_start_ts' and 'day_end_ts' are the beginning and ending timestamp
 *                                 of the day respectively.
 *
 * Fills the 'bboxes' array by storing the 'count' bounding box relative 
 * to the corresponding 'count' activities. */
void overlappingActivityBBoxes(View *view, Activity **acts, int count, 
                               int tslot_off, int tslot_len,
                               time_t day_start_ts, time_t day_end_ts,
                               struct bounding_box *bboxes) 
{
    int col_idxs[count];
    int col_spans[count];
    activityOverlaps(view, acts, count, 
                     col_idxs, col_spans, 
                     day_start_ts, day_end_ts);
    
    // Compute the column width of the overlapping grid.
    int max_col_idx = 0;
    for (int i = 0; i < count; i++) {
        if (col_idxs[i] > max_col_idx) max_col_idx = col_idxs[i];
    }

    int col_width = tslot_len / (max_col_idx+1);
    int col_rest  = tslot_len % (max_col_idx+1);

    /* Compute the bounding boxes */
    for (int i = 0; i < count; i++) {
        Activity *act = acts[i];

        int y0, y1; // top and bottom vertical coordinates
        getActivitySlotRange(view, act, 
                             day_start_ts, day_end_ts,
                             &y0, &y1);

        /* Compute activity offset and width based on column width.
         * Distribute the rest of the width equally among the first 
         * columns. */
        int off   = 0;
        int width = 0;

        if (col_idxs[i] < col_rest) {
            off = col_idxs[i] * (col_width+1);
        } else {
            off = col_rest*(col_width+1) + (col_idxs[i]-col_rest)*col_width;
        }

        for (int s = 0; s < col_spans[i]; s++) {
            width += col_width;
            if (col_idxs[i] + s < col_rest) width++;
        }

        /* Compute the bounding box relative to the view start. */
        struct bounding_box act_box = {
            .x      = off + tslot_off,
            .y      = view->as.act.header_rows + view->as.act.all_day_rows + y0,
            .width  = width,
            .height = y1 - y0,
        };

        bboxes[i] = act_box;
    }
}

/* Return 1 if the activity occupies all the slots of the view between 
 * [start_ts, end_ts], 0 otherwise. */
int isAllDayActivity(View *view, Activity *act, time_t start_ts, time_t end_ts)
{
    int slot_start, slot_end;
    getActivitySlotRange(view, act, 
                         start_ts, end_ts,
                         &slot_start, &slot_end);

    return (slot_start == 0 && slot_end == view->as.act.body_rows);
}

/* Append the time slot rows to the view. */
void viewAppendTimeSlotRows(View *view) {
    size_t bufsize = view->ncols + 1;
    char  *buf = Malloc(bufsize);
    int    buflen;

    struct tm curr, start, end;
    Localtime_r(&view->as.act.curr_ts,  &curr);
    Localtime_r(&view->as.act.start_ts, &start);
    Localtime_r(&view->as.act.end_ts,   &end);

    char time_str[32];
    int time_len = strftime(NULL, sizeof(time_str), 
                            view->as.act.tfmt, &curr);

    // Build the delimiter strings
    char plus_delims[view->as.act.body_len+1];
    char pipe_delims[view->as.act.body_len+1];
    memset(pipe_delims, ' ', view->as.act.body_len);
    memset(plus_delims, ' ', view->as.act.body_len);
    // set '+' or '|' in the first (n-1) grid cell edges 
    struct row_grid *grid = &view->as.act.grid;
    for (int i = 0; i < grid->cell_count-1; i++) {
        int off = grid->cells[i].offset;
        int index = off + grid->cells[i].width;
        pipe_delims[index] = '|';
        plus_delims[index] = '+';
    }
    pipe_delims[view->as.act.body_len] = '\0';
    plus_delims[view->as.act.body_len] = '\0';

    /* Populate the time slot rows */
    int curr_total_mins = (curr.tm_hour * MINUTES_IN_HOUR) + curr.tm_min;
    int h = start.tm_hour;
    int m = start.tm_min;
    struct tm time;
    for (int i = 0; i < view->as.act.body_rows; i++) {
        time.tm_hour = h;
        time.tm_min  = m;

        int tslot_start_mins = (h * MINUTES_IN_HOUR) + m;
        int tslot_end_mins   = tslot_start_mins + view->as.act.tslot_size;

        // We are in the current time slot
        if (curr_total_mins >= tslot_start_mins && 
            curr_total_mins <  tslot_end_mins) {
            if (m == 0) {
                strftime(time_str, sizeof(time_str), view->as.act.tfmt, &time);
                buflen = snprintf(buf, bufsize, "%s +%s+", 
                                  time_str, plus_delims);
            } else {
                strftime(time_str, sizeof(time_str), 
                         view->as.act.tfmt, &curr);
                buflen = snprintf(buf, bufsize, "%s |%s|", 
                                  time_str, pipe_delims);
            }

            viewAppendRow(view, buf, time_len, TEXT_STYLE_INV);
            vrowAppend(&view->rows[view->nrows-1], buf+time_len, 
                       buflen-time_len, TEXT_STYLE_DEFAULT);
        }
        // We are at top of the hour
        else if (m == 0) {
            strftime(time_str, sizeof(time_str), view->as.act.tfmt, &time);
            buflen = snprintf(buf, bufsize, "%s +%s+", 
                              time_str, plus_delims);
            viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
        } 
        // We are off the hour
        else {
            buflen = snprintf(buf, bufsize, "%*s |%s|", 
                              time_len, "", pipe_delims);
            viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
        }

        // Increment time for the next slot
        h += (m + view->as.act.tslot_size) / MINUTES_IN_HOUR;
        m  = (m + view->as.act.tslot_size) % MINUTES_IN_HOUR;
    }

    free(buf);
}

/* Represent a dynamic array of activity with the info of the grid cell
 * in which it resides. */
struct cell_act_list {
    size_t count;
    size_t capacity;
    struct cell_act {
        Activity *act;
        int act_idx;        /* Index of the activity in view 'entities' 
                             * array */
        int cell_start_idx; /* Start cell index of the activity */ 
        int cell_count;     /* Number of cells used by the activity */
    } *items;
};

/* Compute the grid activities from the array of 'act_count' activities using
 * the cells of the 'grid'.
 * Fills the 'grid_acts' list provided as argument. */
void getGridActivities(struct row_grid *grid, Activity **acts, int act_count, 
                       struct cell_act_list *grid_acts) 
{
    if (grid->cell_count == 0) return;

    int cell_start_idx = 0;
    for (int a = 0; a < act_count; a++) {
        Activity *act = acts[a];
        for (int c = cell_start_idx; c < grid->cell_count; c++) {
            time_t cell_start_ts = grid->cells[c].start_ts;
            time_t cell_end_ts   = grid->cells[c].end_ts; 
            if (RANGES_OVERLAP(act->start_ts, act->end_ts,
                               cell_start_ts,  cell_end_ts)) {
                // Check if the cell activity has already been added
                if (grid_acts->count > 0 && 
                    grid_acts->items[grid_acts->count-1].act->id == act->id) {
                    grid_acts->items[grid_acts->count-1].cell_count++;
                } 
                // New cell activity 
                else {
                    struct cell_act cell_act = {
                        .act = act,
                        .act_idx = a,
                        .cell_start_idx = c,
                        .cell_count = 1,
                    };
                    daAdd(grid_acts, cell_act);
                }
            } else if (act->end_ts < cell_start_ts) {
                // The activity ends before the current cell start.
                // It will ends before also for the next cells.
                break; 
            } else if (act->start_ts > cell_end_ts) {
                // The activity starts after the current cell ends.
                // Since, the activities are sorted by start time, all
                // the next activities will start from the next cell.
                cell_start_idx++;
            }
        }
    }
}

/* Split the 'grid_acts' activities into all day activities and time slot
 * activities based on their duration. 
 * 
 * It fills the 'all_day_acts' and 'tslot_acts' lists provideded as argument 
 * with the activities of the two types.
 *
 * For each cell in the 'grid':
 * - An all-day activity is an activity whose duration covers all the time 
 *   slots of the cell.
 * - A time slot activity is an activity whose duration does not cover all 
 *   the time slots of the cell, but only some of them.
 *
 * During the classification process it can happen that an activity is both
 * a all-day activity and a time slot activity (e.g. considering cell 'a' the
 * activity is an all-day activity, while for the next cell 'b' it is a time
 * slot activity). In this cases the activity is added in both lists. */
void classifyGridActivities(View *view,
                            struct row_grid *grid, 
                            struct cell_act_list *grid_acts,
                            struct cell_act_list *all_day_acts,
                            struct cell_act_list *tslot_acts) 
{
    for (size_t a = 0; a < grid_acts->count; a++) {
        struct cell_act *cell_act = &grid_acts->items[a];

        int cell_start_idx = cell_act->cell_start_idx;
        int cell_end_idx   = cell_act->cell_start_idx+cell_act->cell_count-1;

        int all_day_cell_count     = cell_act->cell_count;
        int all_day_cell_start_idx = cell_act->cell_start_idx;

        struct grid_cell *start_cell = &grid->cells[cell_start_idx];
        struct grid_cell *end_cell   = &grid->cells[cell_end_idx];

        /* Add the cell activity as a time slot activity.
         * Check if the start and end cells of the activity are 
         * not all-day.
         * Here we can add the same activity at most two times
         * in the 'tslot_acts' array. In case the start and end 
         * cells of the activity are different it is added twice 
         * to the array.
         * We accept this little redundancy for semplicity in the 
         * further steps.
         * Note that if there are intermediate full days between 
         * the start and end of the activity, the activity is 
         * added as an all-day activity for those days as well. */
        if (!isAllDayActivity(view, 
                              cell_act->act, 
                              start_cell->start_ts, 
                              start_cell->end_ts)) {
            struct cell_act new_cell_act = {
                .act            = cell_act->act,
                .act_idx        = cell_act->act_idx,
                .cell_start_idx = cell_start_idx,
                .cell_count     = 1,
            };
            daAdd(tslot_acts, new_cell_act);
            all_day_cell_count -= new_cell_act.cell_count;
            all_day_cell_start_idx = new_cell_act.cell_start_idx 
                                     + new_cell_act.cell_count;
        }
        if (cell_start_idx != cell_end_idx && 
            !isAllDayActivity(view, 
                              cell_act->act, 
                              end_cell->start_ts, 
                              end_cell->end_ts)) {
            struct cell_act new_cell_act = {
                .act            = cell_act->act,
                .act_idx        = cell_act->act_idx,
                .cell_start_idx = cell_end_idx,
                .cell_count     = 1,
            };
            daAdd(tslot_acts, new_cell_act);
            all_day_cell_count -= new_cell_act.cell_count;
        }

        /* Add to all day activities */
        if (all_day_cell_count > 0) {
            struct cell_act new_cell_act = {
                .act            = cell_act->act,
                .act_idx        = cell_act->act_idx,
                .cell_start_idx = all_day_cell_start_idx,
                .cell_count     = all_day_cell_count,
            };
            daAdd(all_day_acts, new_cell_act);
        }
    }
}

/* Build the all-day activity bounding boxes and store them in the 'view'. */
void buildAllDayActivityBBoxes(View *view, struct cell_act_list *all_day_acts) 
{
    struct row_grid *grid = &view->as.act.grid;

    for (size_t i = 0; i < all_day_acts->count; i++) {
        int act_idx    = all_day_acts->items[i].act_idx;
        int cell_idx   = all_day_acts->items[i].cell_start_idx;
        int cell_count = all_day_acts->items[i].cell_count;

        int act_width = 0;
        for (int j = 0; j < cell_count; j++) {
            act_width += grid->cells[cell_idx + j].width;
            if (j > 0) act_width += grid->delim_width; // add the delim chars
        }

        struct bounding_box bb = {
            .x      = view->as.act.body_off + grid->cells[cell_idx].offset,
            .y      = view->as.act.header_rows + i, 
            .width  = act_width, 
            .height = 1, 
        };

        daAdd(&view->bbox_lists[act_idx], bb);
    }
}

/* Build the time slot activity bounding boxes and store them in the 'view'. */
void buildTimeSlotActivityBBoxes(View *view, struct cell_act_list *tslot_acts)
{
    struct row_grid *grid = &view->as.act.grid;

    ActivityList cell_acts = {0};
    daInit(&cell_acts);

    size_t i = 0;
    while (i < tslot_acts->count) {
        /* Get a subset of 'tslot_acts' activities for a single cell. */
        int start_idx = i;
        int cell_idx  = tslot_acts->items[i].cell_start_idx;
        while (i < tslot_acts->count && 
               tslot_acts->items[i].cell_start_idx == cell_idx) {
            daAdd(&cell_acts, tslot_acts->items[i].act);
            i++;
        }

        /* Build the array of overlapping bounding boxes. */
        time_t cell_start_ts = grid->cells[cell_idx].start_ts;
        time_t cell_end_ts   = grid->cells[cell_idx].end_ts;

        struct bounding_box bboxes[cell_acts.count];
        overlappingActivityBBoxes(view, 
                                  cell_acts.items,
                                  cell_acts.count,
                                  grid->cells[cell_idx].offset,
                                  grid->cells[cell_idx].width,
                                  cell_start_ts,
                                  cell_end_ts,
                                  bboxes);
        for (size_t j = 0; j < cell_acts.count; j++) {
            int act_idx = tslot_acts->items[start_idx + j].act_idx;
            bboxes[j].x += view->as.act.body_off;
            daAdd(&view->bbox_lists[act_idx], bboxes[j]);
        }

        cell_acts.count = 0;
    }

    daFree(&cell_acts);
}

/* ================================ Day view ================================ */

/* Compute the start and end timestamp of the day from the current timestamp
 * 'ts'. 
 * The start and end timestamp are stored in the pointers provided as 
 * argument, if any. */
void getDayBounds(time_t ts, time_t *start_ts, time_t *end_ts) {
    struct tm day;
    Localtime_r(&ts, &day);

    if (start_ts) {
        struct tm day_start = day;
        day_start.tm_hour  = 0;
        day_start.tm_min   = 0;
        day_start.tm_sec   = 0;

        *start_ts = Mktime(&day_start);
    }

    if (end_ts) {
        struct tm day_end = day;
        day_end.tm_hour  = HOURS_IN_DAY-1;
        day_end.tm_min   = MINUTES_IN_HOUR-1;
        day_end.tm_sec   = SECONDS_IN_MINUTE-1;

        *end_ts = Mktime(&day_end);
    }
}

/* Init the day view. */
void initDayView(View *view, time_t ts) {
    time_t start_ts, end_ts;
    getDayBounds(ts, &start_ts, &end_ts);

    view->as.act.act_type = ACTIVITY_VIEW_DAY;

    view->as.act.tfmt       = TIMESLOT_FMT;
    view->as.act.tslot_size = TIME_SLOT_SIZE;

    view->as.act.header_rows  = 0;
    view->as.act.all_day_rows = 0;

    int time_len = getTimeStr(NULL, 32, 
                              view->as.act.tfmt, 
                              view->as.act.curr_ts);
    int ncols = Mycal.win_width;
    view->as.act.body_off = time_len + 1 + 1;         // 1 space, 1 '+' char
    view->as.act.body_len = ncols - time_len - 1 - 2; // 1 space, 2 '+' chars

    int day_count = (view->as.act.body_len >= 1) ? 1 : 0;
    if (day_count < 1) die_small_window();
    int delim_width = 0;
    initGrid(&view->as.act.grid, day_count, view->as.act.body_len, 
             delim_width, start_ts);

    view->as.act.curr_ts  = ts;
    view->as.act.start_ts = view->as.act.grid.cells[0].start_ts;;
    view->as.act.end_ts   = view->as.act.grid.cells[day_count-1].end_ts;

    view->as.act.body_rows = getTimeSlotCount(view->as.act.start_ts,
                                              view->as.act.end_ts,
                                              view->as.act.tslot_size);

    ActivityList act_list = {0};
    daInit(&act_list);
    getActivitiesInRange(Mycal.db, 
                         view->as.act.grid.cells[0].start_ts, 
                         view->as.act.grid.cells[day_count-1].end_ts, 
                         &act_list);

    initView(view, VIEW_ACTIVITY, ncols, 0, 
             (void**)act_list.items, act_list.count);
}

/* Build the day view rows with the content of the day view. */
void populateDayViewRows(View *view) {
    struct tm day_curr, day_start, day_end;
    Localtime_r(&view->as.act.curr_ts,  &day_curr);
    Localtime_r(&view->as.act.start_ts, &day_start);
    Localtime_r(&view->as.act.end_ts,   &day_end);

    int time_len = getTimeStr(NULL, 32, 
                              view->as.act.tfmt,
                              view->as.act.curr_ts);

    size_t bufsize = view->ncols + 1;
    char  *buf = Malloc(bufsize);
    int    buflen;

    /* Header */
    char dtstr[64];
    int dtlen = getTimeStr(dtstr, sizeof(dtstr),
                           FULL_DATETIME_FMT,
                           view->as.act.curr_ts);
    viewAppendRow(view, dtstr, dtlen, TEXT_STYLE_DEFAULT);
    int dtstr_idx = view->nrows-1;
    viewAppendEmpty(view, 1); // empty line

    char dashes[view->as.act.body_len+1];
    memset(dashes, '-', view->as.act.body_len);
    dashes[view->as.act.body_len] = '\0';
    buflen = snprintf(buf, bufsize, "%*s +%s+", time_len, "", dashes);
    viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int dashes_idx = view->nrows-1;

    view->as.act.header_rows = view->nrows;

    /* Split the activities into those that will go in all-day
     * slots and those that will go into regular time slots. */
    struct row_grid *grid = &view->as.act.grid;
    struct cell_act_list grid_acts = {0};
    daInit(&grid_acts);
    getGridActivities(grid, (Activity**)view->entities, view->entity_count,
                      &grid_acts);

    struct cell_act_list all_day_acts = {0};
    struct cell_act_list tslot_acts   = {0};
    daInit(&all_day_acts);
    daInit(&tslot_acts);
    classifyGridActivities(view, grid, &grid_acts, &all_day_acts, &tslot_acts);

    daFree(&grid_acts);

    /* All day slots */
    if (all_day_acts.count > 0) {
        buflen = snprintf(buf, bufsize, "%-*s +%*s+", 
                          time_len, "ALL DAY", view->as.act.body_len, "");
        viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);

        for (size_t j = 1; j < all_day_acts.count; j++) {
            buflen = snprintf(buf, bufsize, "%*s |%*s|", 
                              time_len, "", view->as.act.body_len, "");
            viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
        }
    }

    view->as.act.all_day_rows = view->nrows - view->as.act.header_rows;

    /* Body (time slots) */
    viewAppendTimeSlotRows(view);

    /* Footer */
    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[dashes_idx]);

    viewAppendEmpty(view, 1); // empty line

    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[dtstr_idx]);

    /* Build the activity bounding boxes */
    buildAllDayActivityBBoxes(view, &all_day_acts);
    buildTimeSlotActivityBBoxes(view, &tslot_acts);
    daFree(&all_day_acts);
    daFree(&tslot_acts);

    /* Place the activities */
    for (size_t i = 0; i < view->entity_count; i++) {
        placeActivityInView(view, i);
    }

    free(buf);
}

/* =============================== Week view ================================ */

/* Compute the start and end timestamp of the week from the current timestamp
 * 'ts'. 
 * The start and end timestamp are stored in the pointers provided as 
 * argument, if any. */
void getWeekBounds(time_t ts, time_t *start_ts, time_t *end_ts) {
    struct tm week;
    Localtime_r(&ts, &week);

    if (start_ts) {
        struct tm week_start = week;
        week_start.tm_mday -= week.tm_wday;

        time_t week_start_ts = Mktime(&week_start);
        getDayBounds(week_start_ts, start_ts, NULL);
    }
              
    if (end_ts) {
        struct tm week_end = week;
        week_end.tm_mday += (6 - week.tm_wday);

        time_t week_end_ts = Mktime(&week_end);
        getDayBounds(week_end_ts, NULL, end_ts);
    }
}

/* Init the week view. */
void initWeekView(View *view, time_t ts) {
    time_t start_ts, end_ts;
    getWeekBounds(ts, &start_ts, &end_ts);

    view->as.act.act_type = ACTIVITY_VIEW_WEEK;

    view->as.act.tfmt = TIMESLOT_FMT;
    view->as.act.tslot_size = TIME_SLOT_SIZE;

    view->as.act.header_rows  = 0;
    view->as.act.all_day_rows = 0;

    int time_len = getTimeStr(NULL, 32, 
                              view->as.act.tfmt,
                              view->as.act.curr_ts);
    int ncols = Mycal.win_width;
    view->as.act.body_off = time_len + 1 + 1; // 1 space, 1 '+' char
    view->as.act.body_len = ncols - time_len - 1 - 2; // 1 space, 2 '+' chars

    // Compute the number of days by which the week view should be splitted.
    int day_count = view->as.act.body_len / MAX_MDAY_LENGTH;
    int day_rest  = view->as.act.body_len % MAX_MDAY_LENGTH;

    int delim_width = 1;
    while (day_count > 0 && day_rest < delim_width*(day_count-1)) {
        // Remove a day
        day_count--;
        day_rest += MAX_MDAY_LENGTH;
    }

    if (day_count < 1) die_small_window();
    if (day_count > DAYS_IN_WEEK) day_count = DAYS_IN_WEEK;

    // Init the grid with the current week day at the center if the number
    // of days is smaller than 7. 
    // Compute the timestamp of the view.
    struct tm wday_start, wday_curr;
    Localtime_r(&start_ts, &wday_start);
    Localtime_r(&ts,       &wday_curr);

    int left_days  = 0;
    int right_days = 0;
    if (day_count > 1) left_days  = ROUND_INT_DIV(day_count-1, 2);
    if (day_count > 1) right_days = (day_count-1) / 2;

    if (left_days > wday_curr.tm_wday) {
        right_days += left_days - wday_curr.tm_wday;
        left_days = wday_curr.tm_wday;
    }

    int max_right_days = DAYS_IN_WEEK-1 - wday_curr.tm_wday;
    if (right_days > max_right_days) {
        left_days += right_days - max_right_days;
        right_days = max_right_days;
    }

    wday_start.tm_mday += wday_curr.tm_wday - left_days;
    start_ts = Mktime(&wday_start);

    initGrid(&view->as.act.grid, day_count, view->as.act.body_len, 
             delim_width, start_ts);

    view->as.act.curr_ts  = ts;
    view->as.act.start_ts = view->as.act.grid.cells[0].start_ts;
    view->as.act.end_ts   = view->as.act.grid.cells[day_count-1].end_ts;

    view->as.act.body_rows = getTimeSlotCount(view->as.act.start_ts,
                                              view->as.act.end_ts,
                                              view->as.act.tslot_size);

    ActivityList act_list = {0};
    daInit(&act_list);
    getActivitiesInRange(Mycal.db, 
                         view->as.act.start_ts, 
                         view->as.act.end_ts, 
                         &act_list);

    initView(view, VIEW_ACTIVITY, ncols, 0, 
             (void**)act_list.items, act_list.count);
}

/* Build the week view rows with the content of the week view. */
void populateWeekViewRows(View *view) {
    struct tm week_curr, week_start, week_end;
    Localtime_r(&view->as.act.curr_ts,  &week_curr);
    Localtime_r(&view->as.act.start_ts, &week_start);
    Localtime_r(&view->as.act.end_ts,   &week_end);

    int time_len = getTimeStr(NULL, 32, 
                              view->as.act.tfmt,
                              view->as.act.curr_ts);

    size_t bufsize = view->ncols + 1;
    char  *buf = Malloc(bufsize);
    int    buflen;

    /* Header */
    char dtstr[64];
    int dtlen = getTimeStr(dtstr, sizeof(dtstr), 
                           FULL_DATETIME_FMT,
                           view->as.act.curr_ts);
    viewAppendRow(view, dtstr, dtlen, TEXT_STYLE_DEFAULT);
    int dtstr_idx = view->nrows-1;
    viewAppendEmpty(view, 1); // empty line

    struct row_grid *grid = &view->as.act.grid;

    // Build the weekday delimiter strings.
    char dashes[view->as.act.body_len+1];
    char pipe_delims[view->as.act.body_len+1];
    memset(dashes, '-', view->as.act.body_len);
    memset(pipe_delims, ' ', view->as.act.body_len);
    // set '+' or '|' in the first (n-1) grid cell edges 
    for (int i = 0; i < grid->cell_count-1; i++) {
        int off = grid->cells[i].offset;
        int index = off + grid->cells[i].width;
        dashes[index] = '+';
        pipe_delims[index] = '|';
    }
    dashes[view->as.act.body_len] = '\0';
    pipe_delims[view->as.act.body_len] = '\0';

    buflen = snprintf(buf, bufsize, "%*s +%s+", time_len, "", dashes);
    viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int dashes_idx = view->nrows-1;

    // Build the weekday header string.
    // Each weekday string is made of the name of the weekday and 
    // the day of the month (e.g. "Sunday 26").
    //
    // There is a minimum padding of 1 in the weekday cell (leave 1 space 
    // to the left and 1 space to right from the weekday grid cell).
    //
    // Each weekday name has the same length that is truncated if exceeds 
    // the avaible space. After this truncation, if the weekday name is
    // too short, all the day numbers are wrapped on the next line.
    //
    // Finally a delimiter character is used to separate the weekday cells.
    //
    //  | S | M | T | W | T | F | S |
    //  | Su | Mo | Tu | We | Th | Fr | Sa 
    //  | Sun 26 | Mon 27 | Tue 28 | Wed 29 | Thu 30 | Fri 01 | Sat 02 
    //  | Sunday 26 | Monday 27 | Tuesday 28 | Wednesday 29 | ... 
    int min_cell_width = grid->cells[grid->cell_count-1].width;
    int max_name_len = min_cell_width-1-1-MAX_MDAY_LENGTH-1; // 3 spaces
    int is_day_inline = 1;
    if (max_name_len < 3) {
        max_name_len = min_cell_width-1-1; // remove day length and space
        is_day_inline = 0;
        if (max_name_len < MIN_WDAY_LENGTH) max_name_len = min_cell_width;
    }

    // Append the sketch header rows to the view.
    buflen = snprintf(buf, bufsize, "%*s |%s|", time_len, "", pipe_delims);
    viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int week_idx = view->nrows-1;
    struct vrow *week_row = &view->rows[week_idx];

    if (!is_day_inline) viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int days_idx = view->nrows-1;
    struct vrow *days_row = &view->rows[days_idx];

    // Fill the weekday names and weekday days arrays.
    struct tm wday;
    Localtime_r(&view->as.act.start_ts, &wday);
    for (int i = 0; i < grid->cell_count; i++) {
        char wday_name[16];
        int name_len = strftime(wday_name, sizeof(wday_name), "%A", &wday);
        if (name_len > max_name_len) name_len = max_name_len;

        char wday_day[MAX_MDAY_LENGTH+1];
        int day_len = snprintf(wday_day, sizeof(wday_day), "%*d", 
                               MAX_MDAY_LENGTH, wday.tm_mday);
        if (day_len > MAX_MDAY_LENGTH) day_len = MAX_MDAY_LENGTH;

        // Check if we are in the current weekday.
        int is_curr_wday = week_curr.tm_yday == wday.tm_yday; 

        // Set the weekday name and day on the same rows.
        if (is_day_inline) {
            char wday_text[max_name_len+1+MAX_MDAY_LENGTH+1];
            int text_len = snprintf(wday_text, sizeof(wday_text), 
                                    "%.*s %.*s", 
                                    name_len, wday_name, 
                                    day_len, wday_day);
            vrowSetGridCellText(week_row, &grid->cells[i], 
                                wday_text, text_len,
                                (is_curr_wday) ? 
                                TEXT_STYLE_INV : TEXT_STYLE_DEFAULT,
                                view->as.act.body_off);
        } 
        // Set the weekday name and day in different rows.
        else {
            vrowSetGridCellText(week_row, &grid->cells[i], 
                                wday_name, name_len,
                                (is_curr_wday) ? 
                                TEXT_STYLE_INV : TEXT_STYLE_DEFAULT,
                                view->as.act.body_off);
            vrowSetGridCellText(days_row, &grid->cells[i], 
                                wday_day, day_len,
                                (is_curr_wday) ? 
                                TEXT_STYLE_INV : TEXT_STYLE_DEFAULT,
                                view->as.act.body_off);
        }

        wday.tm_mday += 1;
        Mktime(&wday);
    }

    view->as.act.header_rows = view->nrows;

    /* Split the activities into those that will go in all-day
     * slots and those that will go into regular time slots. */
    struct cell_act_list grid_acts = {0};
    daInit(&grid_acts);
    getGridActivities(grid, (Activity**)view->entities, view->entity_count,
                      &grid_acts);

    struct cell_act_list all_day_acts = {0};
    struct cell_act_list tslot_acts   = {0};
    daInit(&all_day_acts);
    daInit(&tslot_acts);
    classifyGridActivities(view, grid, &grid_acts, &all_day_acts, &tslot_acts);

    daFree(&grid_acts);

    /* All day slots */
    if (all_day_acts.count > 0) {
        buflen = snprintf(buf, bufsize, "%-*s +%*s+", 
                          time_len, "ALL DAY", view->as.act.body_len, "");
        viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);

        for (size_t j = 1; j < all_day_acts.count; j++) {
            buflen = snprintf(buf, bufsize, "%*s |%*s|", 
                              time_len, "", view->as.act.body_len, "");
            viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
        }
    }

    view->as.act.all_day_rows = view->nrows - view->as.act.header_rows;

    /* Body (time slots) */
    viewAppendTimeSlotRows(view); 

    /* Footer */
    if (!is_day_inline) {
        viewAppendEmpty(view, 1);
        viewCopyRow(view, view->nrows-1, &view->rows[days_idx]);
    }
    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[week_idx]);

    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[dashes_idx]);

    viewAppendEmpty(view, 1); // empty line

    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[dtstr_idx]);

    /* Build the activity bounding boxes */
    buildAllDayActivityBBoxes(view, &all_day_acts);
    buildTimeSlotActivityBBoxes(view, &tslot_acts);
    daFree(&all_day_acts);
    daFree(&tslot_acts);

    /* Place the activities */
    for (size_t i = 0; i < view->entity_count; i++) {
        placeActivityInView(view, i);
    }

    free(buf);
}

/* =============================== Month view =============================== */

/* Compute the start and end timestamp of the month from the current timestamp
 * 'ts'. 
 * The start and end timestamp are stored in the pointers provided as 
 * argument, if any. */
void getMonthBounds(time_t ts, time_t *start_ts, time_t *end_ts) {
    struct tm month;
    Localtime_r(&ts, &month);

    if (start_ts) {
        struct tm month_start = month;
        month_start.tm_mday = 1;

        time_t month_start_ts = Mktime(&month_start);
        getDayBounds(month_start_ts, start_ts, NULL);
    }
              
    if (end_ts) {
        struct tm month_end = month;
        // day 0 of the next month is the last day of the target month
        month_end.tm_mon += 1;
        month_end.tm_mday = 0;
        
        time_t month_end_ts = Mktime(&month_end);
        getDayBounds(month_end_ts, NULL, end_ts);
    }
}

/* Init the month view. */
void initMonthView(View *view, time_t ts) {
    time_t start_ts, end_ts;
    getMonthBounds(ts, &start_ts, &end_ts);
    getWeekBounds(start_ts, &start_ts, NULL);
    getWeekBounds(end_ts, NULL, &end_ts);

    view->as.act.act_type = ACTIVITY_VIEW_MONTH;

    view->as.act.tfmt = NULL;
    view->as.act.tslot_size = 0;

    view->as.act.header_rows  = 0;
    view->as.act.all_day_rows = 0;

    int ncols = Mycal.win_width;
    view->as.act.body_off  = 1;         // 1 '+' char
    view->as.act.body_len  = ncols - 2; // 2 '+' chars
    view->as.act.body_rows = 0;

    // Compute the number of days by which the week of the month view
    // should be splitted.
    int day_count = view->as.act.body_len / MAX_MDAY_LENGTH;
    int day_rest  = view->as.act.body_len % MAX_MDAY_LENGTH;

    int delim_width = 1;
    while (day_count > 0 && day_rest < delim_width*(day_count-1)) {
        // Remove a day
        day_count--;
        day_rest += MAX_MDAY_LENGTH;
    }

    if (day_count < 1) die_small_window();
    if (day_count > DAYS_IN_WEEK) day_count = DAYS_IN_WEEK;

    // Init the grid with the current week day at the center if the number
    // of days is smaller than 7. 
    // Compute the timestamp of the view.
    struct tm wday_curr, wday_start, wday_end;
    Localtime_r(&ts,       &wday_curr);
    Localtime_r(&start_ts, &wday_start);
    Localtime_r(&end_ts,   &wday_end);

    int left_days  = 0;
    int right_days = 0;
    if (day_count > 1) left_days  = ROUND_INT_DIV(day_count-1, 2);
    if (day_count > 1) right_days = (day_count-1) / 2;

    if (left_days > wday_curr.tm_wday) {
        right_days += left_days - wday_curr.tm_wday;
        left_days = wday_curr.tm_wday;
    }

    int max_right_days = DAYS_IN_WEEK-1 - wday_curr.tm_wday;
    if (right_days > max_right_days) {
        left_days += right_days - max_right_days;
        right_days = max_right_days;
    }

    wday_start.tm_mday += wday_curr.tm_wday - left_days;
    wday_end.tm_mday   -= (DAYS_IN_WEEK-1) - (wday_curr.tm_wday + right_days);
    start_ts = Mktime(&wday_start);
    end_ts   = Mktime(&wday_end);

    view->as.act.curr_ts  = ts;
    view->as.act.start_ts = start_ts;
    view->as.act.end_ts   = end_ts;
    view->as.act.day_gap  = DAYS_IN_WEEK - day_count;

    initGrid(&view->as.act.grid, day_count, view->as.act.body_len, 
             delim_width, start_ts);

    // Retreve activities for each grid row to add to the activity list.
    ActivityList act_list = {0};
    daInit(&act_list);
    ActivityList grid_acts = {0};
    daInit(&grid_acts);

    struct tm mday;
    Localtime_r(&view->as.act.start_ts, &mday);
    time_t mday_ts = Mktime(&mday);
    while (mday_ts <= view->as.act.end_ts) {

        initDailyGridCellTimes(&view->as.act.grid, mday_ts);
        getActivitiesInRange(Mycal.db, 
                             view->as.act.grid.cells[0].start_ts,
                             view->as.act.grid.cells[day_count-1].end_ts,
                             &grid_acts);

        for (size_t i = 0; i < grid_acts.count; i++) {
            // Check for duplicates before appending
            int exists = 0;
            for (size_t j = 0; j < act_list.count; j++) {
                if (act_list.items[j]->id == grid_acts.items[i]->id) {
                    exists = 1;
                    break;
                }
            }
            if (!exists) {
                daAdd(&act_list, grid_acts.items[i]);
            } else {
                freeActivity(grid_acts.items[i]); // free duplicate
            }
        }

        mday.tm_mday += day_count + view->as.act.day_gap;
        mday_ts = Mktime(&mday);
        grid_acts.count = 0;
    }

    initView(view, VIEW_ACTIVITY, ncols, 0, 
             (void**)act_list.items, act_list.count);

    daFree(&grid_acts);
}

/* Build the month view rows with the content of the month view. */
void populateMonthViewRows(View *view) {
    struct tm month_curr, month_start, month_end;
    Localtime_r(&view->as.act.curr_ts,  &month_curr);
    Localtime_r(&view->as.act.start_ts, &month_start);
    Localtime_r(&view->as.act.end_ts,   &month_end);

    size_t bufsize = view->ncols + 1;
    char  *buf = Malloc(bufsize);
    int    buflen;

    /* Header */
    char dtstr[64];
    int dtlen = getTimeStr(dtstr, sizeof(dtstr), 
                           FULL_DATETIME_FMT,
                           view->as.act.curr_ts);
    viewAppendRow(view, dtstr, dtlen, TEXT_STYLE_DEFAULT);
    int dtstr_idx = view->nrows-1;
    viewAppendEmpty(view, 1); // empty line

    struct row_grid *grid = &view->as.act.grid;

    // Build the weekday delimiter strings.
    char dashes[view->as.act.body_len+1];
    char pipe_delims[view->as.act.body_len+1];
    char plus_delims[view->as.act.body_len+1];
    memset(dashes, '-', view->as.act.body_len);
    memset(pipe_delims, ' ', view->as.act.body_len);
    memset(plus_delims, ' ', view->as.act.body_len);
    // set '+' or '|' in the first (n-1) grid cell edges 
    for (int i = 0; i < grid->cell_count-1; i++) {
        int off = grid->cells[i].offset;
        int index = off + grid->cells[i].width;
        dashes[index] = '+';
        pipe_delims[index] = '|';
        plus_delims[index] = '+';
    }
    dashes[view->as.act.body_len] = '\0';
    pipe_delims[view->as.act.body_len] = '\0';
    plus_delims[view->as.act.body_len] = '\0';

    buflen = snprintf(buf, bufsize, "+%s+", dashes);
    viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int dashes_idx = view->nrows-1;

    // Build the weekday header string.
    // Each weekday string is made of the name of the weekday.
    //
    // There is a minimum padding of 1 in the weekday cell (leave 1 space 
    // to the left and 1 space to right from the weekday grid cell).
    //
    // Each weekday name has the same length that is truncated if exceeds 
    // the avaible space.
    //
    // Finally a delimiter character is used to separate the weekday cells.
    //
    //  S | M | T | W | T | F | S 
    //  Su | Mo | Tu | We | Th | Fr | Sa 
    //  Sun | Mon | Tue | Wed | Thu | Fri | Sat 
    //  Sunday | Monday | Tuesday | Wednesday | ... 
    int min_cell_width = grid->cells[grid->cell_count-1].width;
    int max_name_len = min_cell_width-1-1; // 2 spaces
    if (max_name_len < MIN_WDAY_LENGTH) max_name_len = min_cell_width;

    // Append the sketch header rows to the view.
    buflen = snprintf(buf, bufsize, "|%s|", pipe_delims);
    viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int week_idx = view->nrows-1;
    struct vrow *week_row = &view->rows[week_idx];

    // Fill the the grid cells with the weekday names.
    struct tm wday;
    Localtime_r(&view->as.act.start_ts, &wday);
    for (int i = 0; i < grid->cell_count; i++) {
        char wday_name[16];
        int name_len = strftime(wday_name, sizeof(wday_name), "%A", &wday);
        if (name_len > max_name_len) name_len = max_name_len;

        // Set the weekday name at the center of the grid cell.
        vrowSetGridCellText(week_row, &grid->cells[i], 
                            wday_name, name_len,
                            TEXT_STYLE_DEFAULT,
                            view->as.act.body_off);

        wday.tm_mday += 1;
        Mktime(&wday);
    }

    view->as.act.header_rows = view->nrows;

    /* Body (month days) */
    struct tm mday;
    Localtime_r(&view->as.act.start_ts, &mday);
    time_t mday_ts = Mktime(&mday);
    while (mday_ts <= view->as.act.end_ts) {
        // Build and append the header rows for the month week.
        for (int i = 0; i < MDAY_HEADER_ROWS; i++) {
            buflen = snprintf(buf, bufsize, "+%s+", plus_delims);
            viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
            struct vrow *days_row = &view->rows[view->nrows-1];

            for (int j = 0; j < grid->cell_count; j++) {
                char mday_day[MAX_MDAY_LENGTH+1];
                int day_len = snprintf(mday_day, sizeof(mday_day), "%*d", 
                                       MAX_MDAY_LENGTH, mday.tm_mday);
                // Check if we are in the current weekday.
                int is_curr_wday = month_curr.tm_yday == mday.tm_yday; 

                // Set the weekday day at the center of the grid cell.
                int mday_col = vrowSetGridCellText(days_row, &grid->cells[j], 
                                                   mday_day, day_len,
                                                   TEXT_STYLE_DEFAULT,
                                                   view->as.act.body_off);
                if (is_curr_wday) {
                    int is_single_digit = (mday.tm_mday / 10) == 0;
                    if (is_single_digit) {
                        day_len  -= 1;
                        mday_col += 1;
                    }
                    vrowSet(days_row, mday_col, 
                            &days_row->chars[mday_col], day_len, 
                            TEXT_STYLE_INVERT);
                }

                mday.tm_mday += 1;
                mday_ts = Mktime(&mday);
            }
        }

        // Append the sketch body rows for the month week.
        for (int i = 0; i < MDAY_BODY_ROWS; i++) {
            buflen = snprintf(buf, bufsize, "|%s|", pipe_delims);
            viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
        }
        
        mday.tm_mday += view->as.act.day_gap;
        mday_ts = Mktime(&mday);
    }

    view->as.act.body_rows = view->nrows - view->as.act.header_rows;

    /* Footer */
    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[week_idx]);

    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[dashes_idx]);

    viewAppendEmpty(view, 1); // empty line

    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[dtstr_idx]);

    /* Build the activity bounding boxes */
    struct cell_act_list grid_acts   = {0};
    struct cell_act_list placed_acts = {0};
    daInit(&grid_acts);
    daInit(&placed_acts);

    /* For each day of the month, place the activity bounding boxes 
     * based on their starting cell and duration. 
     * The activities are placed starting from those that begin on 
     * earlier cells.
     * If two activities start in the same cell, place the activity 
     * that spans multiple cells first. 
     * If there is no more space to place activities, write the number
     * of remaining activities on the last body row.
     *
     *    Sun     Mon     Tue     Wed     Thu     Fri     Sat   
     * +-------+-------+-------+---------------+       +-------+
     * |  act  |  act  |  act  |      act      |       |  act  |
     * +-------+-------+-------+---------------+       +-------+
     * +-------+-------+-------+       +---------------+-------+
     * |+3 more|+2 more|  act  |       |      act      |+3 more|
     * +-------+-------+-------+       +---------------+-------+
     * */
    Localtime_r(&view->as.act.start_ts, &mday);
    mday_ts = Mktime(&mday);
    int week_count = 0;
    while (mday_ts <= view->as.act.end_ts) {
        // Get the activities of the month week.
        initDailyGridCellTimes(grid, mday_ts);
        getGridActivities(grid, (Activity**)view->entities, view->entity_count,
                          &grid_acts);
        
        // For each cell of the grid store the number of activity in 
        // that cell.
        int cell_act_count[grid->cell_count];
        memset(cell_act_count, 0, sizeof(cell_act_count));

        // For each cell of the grid and for each body row of the day, 
        // store the index of cell activity if any, -1 otherwise.
        int cell_act_idxs[grid->cell_count][MDAY_BODY_ROWS]; 
        for (int i = 0; i < grid->cell_count; i++) {
            for (int j = 0; j < MDAY_BODY_ROWS; j++) {
                cell_act_idxs[i][j] = -1;
            }
        }

        for (size_t a = 0; a < grid_acts.count; a++) {
            struct cell_act *cell_act = &grid_acts.items[a];
            int i = cell_act->cell_start_idx;

            for (int cell = 0; cell < cell_act->cell_count; cell++) {
                cell_act_count[cell+i] += 1;
            }

            /* Find a free row in cell 'i' of 'cell_act_idx'.
             * If we encounter an activity that starts from the same 
             * cell and whose duration is smaller, then we replace that
             * activity with the current one. 
             * The same process is repeated for all the rows of the day. 
             * By doing this we are actually shifting the activities,
             * keeping only the first once we are most interesed in. */
            int is_placed = 0;
            int j;
            for (j = 0; j < MDAY_BODY_ROWS; j++) {
                if (cell_act_idxs[i][j] == -1) {
                    is_placed = 1;
                } else {
                    int curr_idx = a;
                    struct cell_act *curr_cell = cell_act;
                    for (int k = j; k < MDAY_BODY_ROWS; k++) {
                        int prev_idx = cell_act_idxs[i][k];
                        struct cell_act *prev_cell =
                            &grid_acts.items[prev_idx];

                        // Replace existing activities that starts from 
                        // the same cell and whose end cell is smaller.
                        if (prev_cell->cell_start_idx == i &&
                            prev_cell->cell_count < curr_cell->cell_count) {
                            for (int cell = 0; 
                                 cell < curr_cell->cell_count; 
                                 cell++) {
                                cell_act_idxs[cell+i][k] = curr_idx;
                            }
                            is_placed = 1;

                            curr_idx = prev_idx;
                            curr_cell = prev_cell;
                        }

                        // If we don't swap the 'cell_act' with an existing
                        // one in row 'j', we move to the next row.
                        // Otherwise we continue swapping the remaining
                        // activities of the rows.
                        if (!is_placed) break;
                    }
                }

                if (is_placed) {
                    // Place the activity in 'cell_act_idxs' row and cells.
                    for (int cell = 0; cell < cell_act->cell_count; cell++) {
                        cell_act_idxs[cell+i][j] = a;
                    }
                    break;
                }
            }
        }

        // Fill the 'placed_acts' list with only the activities 
        // that should be placed in the current week of the month.
        // Store the row index for each placed activity as well.
        int act_row_idxs[grid_acts.count]; 
        int r = 0;
        for (int j = 0; j < MDAY_BODY_ROWS; j++) {
            int curr = -1;
            for (int i = 0; i < grid->cell_count; i++) {
                // New activity
                if (cell_act_idxs[i][j] != -1 &&
                    cell_act_idxs[i][j] != curr) {
                    curr = cell_act_idxs[i][j];

                    /* If we are on the last row of the cells and there are
                     * more activities than the avaible space, leave the
                     * cells row emtpy (we will write the number of the
                     * remaining activities later) and continue with the 
                     * next activities. */
                    if (j == MDAY_BODY_ROWS-1 && 
                        cell_act_count[i] > MDAY_BODY_ROWS) {
                        continue;
                    }
                    int index = cell_act_idxs[i][j];
                    struct cell_act cell_act = grid_acts.items[index];
                    daAdd(&placed_acts, cell_act);
                    act_row_idxs[r++] = j;
                }
            }
        }

        // Build the bounding boxes of the placed activities.
        buildAllDayActivityBBoxes(view, &placed_acts);

        // Adjust the y coordinate of the added bounding box (the last one).
        for (size_t i = 0; i < placed_acts.count; i++) {
            int y = view->as.act.header_rows
                + MDAY_HEADER_ROWS + act_row_idxs[i] 
                + ((MDAY_HEADER_ROWS + MDAY_BODY_ROWS) * week_count);

            int act_idx  = placed_acts.items[i].act_idx;
            int bbox_idx = view->bbox_lists[act_idx].count-1;
            view->bbox_lists[act_idx].items[bbox_idx].y = y;
        }

        // Write the number of the remaining activities on the last row 
        // of the cells.
        for (int i = 0; i < grid->cell_count; i++) {
            if (cell_act_count[i] > MDAY_BODY_ROWS) {
                int row_idx = 
                    view->as.act.header_rows 
                    + MDAY_HEADER_ROWS
                    + (MDAY_BODY_ROWS-1)
                    + ((MDAY_HEADER_ROWS + MDAY_BODY_ROWS) * week_count);
                struct vrow *row = &view->rows[row_idx];

                char text[16];
                int text_len = snprintf(text, sizeof(text), "+%d more", 
                        cell_act_count[i]-MDAY_BODY_ROWS+1);
                TextStyle tstyle = MAKE_TEXT_STYLE(COLOR_DEFAULT,
                                                   COLOR_DEFAULT,
                                                   TEXT_STYLE_ITALIC |
                                                   TEXT_STYLE_DIM);
                vrowSetGridCellText(row, &grid->cells[i], 
                                    text, text_len, tstyle,
                                    view->as.act.body_off);
            }
        }

        grid_acts.count   = 0;
        placed_acts.count = 0;

        week_count += 1;
        mday.tm_mday += grid->cell_count + view->as.act.day_gap;
        mday_ts = Mktime(&mday);
    }

    daFree(&grid_acts);
    daInit(&placed_acts);

    /* Remove the activities that have no bounding boxes set, so we can't 
     * select them during selectEntityFromUser like functions. */
    size_t index = 0;
    for (size_t i = 0; i < view->entity_count; i++) {
        if (view->bbox_lists[i].count > 0) {
            if (index != i) {
                view->entities[index] = view->entities[i];
                view->bbox_lists[index] = view->bbox_lists[i];
            }
            index++;
        }
    }
    view->entity_count = index;

    /* Place the activities */
    for (size_t i = 0; i < view->entity_count; i++) {
        placeActivityInView(view, i);
    }

    free(buf);
}

/* =============================== Year view ================================ */

/* Compute the start and end timestamp of the year from the current timestamp
 * 'ts'. 
 * The start and end timestamp are stored in the pointers provided as 
 * argument, if any. */
void getYearBounds(time_t ts, time_t *start_ts, time_t *end_ts) {
    struct tm year;
    Localtime_r(&ts, &year);

    if (start_ts) {
        struct tm year_start = year;
        year_start.tm_mday = 1;
        year_start.tm_mon  = 0;

        time_t year_start_ts = Mktime(&year_start);
        getDayBounds(year_start_ts, start_ts, NULL);
    }
              
    if (end_ts) {
        struct tm year_end = year;
        year_end.tm_mday = 31;
        year_end.tm_mon  = 11;

        time_t year_end_ts = Mktime(&year_end);
        getDayBounds(year_end_ts, NULL, end_ts);
    }
}

/* Init the year view. */
void initYearView(View *view, time_t ts) {
    time_t start_ts, end_ts;
    getYearBounds(ts, &start_ts, &end_ts);

    view->as.act.act_type = ACTIVITY_VIEW_YEAR;

    view->as.act.curr_ts  = ts;
    view->as.act.start_ts = start_ts;
    view->as.act.end_ts   = end_ts;

    view->as.act.tfmt = NULL;
    view->as.act.tslot_size = 0;

    view->as.act.header_rows  = 0;
    view->as.act.all_day_rows = 0;

    int ncols = Mycal.win_width;
    view->as.act.body_off  = 0;
    view->as.act.body_len  = ncols;
    view->as.act.body_rows = 0;

    // Compute the number of month by which the year view should be splitted.
    int day_delim_width = 1;
    int min_month_width = (MAX_MDAY_LENGTH * DAYS_IN_WEEK)
                          + (day_delim_width * (DAYS_IN_WEEK-1));
    int month_count = view->as.act.body_len / min_month_width;
    int month_rest  = view->as.act.body_len % min_month_width;

    int month_delim_width = 2;
    while (month_count > 0 && 
           month_rest < month_delim_width*(month_count-1)) {
        // Remove a month
        month_count--;
        month_rest += min_month_width;
    }

    // Clamp the number of month to range [1, 6] and choose only numbers
    // divisible by 2 or 3 to keep the rows simmetric. 
    if (month_count < 1)  die_small_window();
    if (month_count == 5) month_count = 4;
    if (month_count > 6)  month_count = 6;

    initGrid(&view->as.act.grid, month_count, 
             view->as.act.body_len, month_delim_width,
             view->as.act.start_ts);

    // Do not provide activities so we can't select them during 
    // selectEntityFromUser like functions.
    initView(view, VIEW_ACTIVITY, ncols, 0, NULL, 0);
}

/* Build the year view rows with the content of the year view. */
void populateYearViewRows(View *view) {
    struct tm year_curr, year_start, year_end;
    Localtime_r(&view->as.act.curr_ts,  &year_curr);
    Localtime_r(&view->as.act.start_ts, &year_start);
    Localtime_r(&view->as.act.end_ts,   &year_end);

    size_t bufsize = view->ncols + 1;
    char  *buf = Malloc(bufsize);
    int buflen;

    /* Header */
    char dtstr[64];
    int dtlen = getTimeStr(dtstr, sizeof(dtstr), 
                           FULL_DATETIME_FMT,
                           view->as.act.curr_ts);
    viewAppendRow(view, dtstr, dtlen, TEXT_STYLE_DEFAULT);
    int dtstr_idx = view->nrows-1;
    viewAppendEmpty(view, 1); // empty line

    view->as.act.header_rows = view->nrows;

    /* Body */
    struct row_grid *grid = &view->as.act.grid;

    // Build the emtpy row string
    char empty_row[view->as.act.body_len+1];
    memset(empty_row, ' ', view->as.act.body_len);
    empty_row[view->as.act.body_len] = '\0';

    const char *wday_names = "Su Mo Tu We Th Fr Sa";
    int wday_names_len = strlen(wday_names);
    int wdelim_len = 1;

    // Loop through all the days of the year.
    struct tm yday;
    Localtime_r(&view->as.act.start_ts, &yday);
    time_t yday_ts = Mktime(&yday);
    while (yday_ts <= view->as.act.end_ts) {
        int month_name_row_idx   = -1;
        int weekday_name_row_idx = -1;
        int days_row_start_idx   = -1;
        int days_row_count       = 0;

        // Loop through all the cells (months).
        for (int c = 0; c < grid->cell_count && 
                            yday_ts <= view->as.act.end_ts; c++) {
            // First cell (month), append the empty rows for 
            // the month and weekday names.
            if (c == 0) {
                viewAppendRow(view, empty_row, view->as.act.body_len,
                        TEXT_STYLE_DEFAULT);
                month_name_row_idx = view->nrows-1;
                viewAppendRow(view, empty_row, view->as.act.body_len,
                        TEXT_STYLE_DEFAULT);
                weekday_name_row_idx = view->nrows-1;
            }

            // Set the month name at the center of the grid cell.
            char month_name[16];
            strftime(month_name, sizeof(month_name), "%B", &yday);
            vrowSetGridCellText(&view->rows[month_name_row_idx], 
                                &grid->cells[c], 
                                month_name, strlen(month_name),
                                TEXT_STYLE_DEFAULT,
                                view->as.act.body_off);

            // Set the weekday names at the center of the grid cell.
            vrowSetGridCellText(&view->rows[weekday_name_row_idx], 
                                &grid->cells[c], 
                                wday_names, wday_names_len,
                                TEXT_STYLE_DEFAULT,
                                view->as.act.body_off);

            // Build the days rows.
            time_t month_start_ts, month_end_ts;
            getMonthBounds(yday_ts, &month_start_ts, &month_end_ts);

            time_t cal_month_start_ts, cal_month_end_ts;
            getWeekBounds(month_start_ts, &cal_month_start_ts, NULL);
            getWeekBounds(month_end_ts, NULL, &cal_month_end_ts);

            struct tm mday;
            Localtime_r(&cal_month_start_ts, &mday);
            time_t mday_ts = Mktime(&mday);

            // Loop through all the days of the month.
            int days_row = 0;
            while (mday_ts <= cal_month_end_ts) {
                buflen = 0;
                int curr_yday = -1;

                // Loop through all the days of the week.
                for (int i = 0; i < DAYS_IN_WEEK; i++) {
                    // This day is an actually day of this month.
                    // Not a day of the previous or next month.
                    if (mday_ts >= month_start_ts && mday_ts <= month_end_ts) {
                        buflen += snprintf(buf+buflen, bufsize-buflen, 
                                "%*d", MAX_MDAY_LENGTH, mday.tm_mday); 

                        // Check if we are in the current year day.
                        if (year_curr.tm_yday == mday.tm_yday) curr_yday = i;

                        yday.tm_mday += 1;
                    }
                    // This day is not a day of this month, but one of the
                    // previous or next months that fall in the same week.
                    //
                    //   Su Mo Tu We Th Fr Sa
                    //   26 27 28 29 30  1  2
                    //   ^^ ^^ ^^ ^^ ^^ 
                    else {
                        buflen += snprintf(buf+buflen, bufsize-buflen, 
                                "%*s", MAX_MDAY_LENGTH, ""); 
                    }

                    if (i < DAYS_IN_WEEK-1) {
                        buflen += snprintf(buf+buflen, bufsize-buflen, " ");
                    }

                    mday.tm_mday += 1;
                    mday_ts = Mktime(&mday);
                }

                // First cell (month), append the sketch rows for the month 
                // day.
                if (c == 0 || days_row >= days_row_count) {
                    viewAppendRow(view, empty_row, view->as.act.body_len,
                            TEXT_STYLE_DEFAULT);
                    if (days_row_start_idx == -1)
                        days_row_start_idx = view->nrows-1;
                    days_row_count++;
                }

                // Set the month day string at the center of the grid cell.
                struct vrow *row = &view->rows[days_row_start_idx+days_row];
                vrowSetGridCellText(row, 
                                    &grid->cells[c], 
                                    buf, buflen,
                                    TEXT_STYLE_DEFAULT,
                                    view->as.act.body_off);

                // Highlight the current year day.
                if (curr_yday != -1) {
                    int is_single_digit = (year_curr.tm_mday / 10) == 0;
                    int left_pad = (grid->cells[c].width - buflen) / 2;
                    if (left_pad < 0) left_pad = 0;
                    int off = grid->cells[c].offset + left_pad
                              + ((wdelim_len+MAX_MDAY_LENGTH) * curr_yday) 
                              + is_single_digit; 
                    vrowSet(row, off, row->chars+off, 
                            MAX_MDAY_LENGTH-is_single_digit, 
                            TEXT_STYLE_INV);
                }

                days_row++;
            }

            yday_ts = Mktime(&yday);
        }
        viewAppendEmpty(view, 1);
    }

    view->as.act.body_rows = view->nrows - view->as.act.header_rows;

    /* Footer */
    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[dtstr_idx]);

    free(buf);
}

/* ============================= Activity view ============================== */

/* Init the activity view. */
void initActivityView(View *view, time_t ts, int act_count) {
    view->as.act.act_type = ACTIVITY_VIEW_CUSTOM;

    view->as.act.curr_ts  = ts;
    view->as.act.start_ts = ts;
    view->as.act.end_ts   = ts;

    view->as.act.header_rows = 0;
    view->as.act.body_off    = 0;
    view->as.act.body_len    = Mycal.win_width;

    int ncols = Mycal.win_width;
    initView(view, VIEW_ACTIVITY, ncols, 0, NULL, 0);

    ActivityList act_list = {0};
    daInit(&act_list);
    getLastActivities(Mycal.db, ts, act_count, &act_list);

    initView(view, VIEW_ACTIVITY, ncols, 0, 
             (void**)act_list.items, act_list.count);
}

/* Build the activity view rows. */
void populateActivityViewRows(View *view) {
    size_t bufsize = view->ncols + 1;
    char  *buf = Malloc(bufsize);
    int buflen;

    /* Header */
    char ts_str[64];
    getTimeStr(ts_str, sizeof(ts_str), FULL_DATETIME_FMT,
               view->as.act.curr_ts);
    buflen = snprintf(buf, bufsize, "%s - %zu activities", 
                      ts_str, view->entity_count);
    viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int header_idx = view->nrows-1;
    viewAppendEmpty(view, 1); // empty line

    view->as.act.header_rows = view->nrows;

    /* Body (activity rows) */
    memset(buf, ' ', bufsize);
    buf[bufsize-1] = '\0';
    for (size_t i = 0; i < view->entity_count; i++) {
        buflen = 0;
        Activity *act = (Activity *)view->entities[i];
        int act_start_row = view->nrows;

        Color act_bgcolor = act->category ? act->category->color : UNCAT_COLOR;
        Color act_fgcolor = act_bgcolor == COLOR_WHITE ? COLOR_BLACK : 
                                                         COLOR_DEFAULT;

        TextStyle act_style = MAKE_TEXT_STYLE(act_fgcolor, 
                                              act_bgcolor,
                                              TEXT_STYLE_NONE);

        // Title
        TextStyle title_style;
        if (act->title) {
            title_style = MAKE_TEXT_STYLE(act_fgcolor, 
                                          act_bgcolor,
                                          TEXT_STYLE_BOLD);
            viewAppendRowWrap(view, act->title, strlen(act->title),
                              title_style);
        } else {
            title_style = MAKE_TEXT_STYLE(act_fgcolor, 
                                          act_bgcolor,
                                          TEXT_STYLE_BOLD |
                                          TEXT_STYLE_ITALIC);
            const char *title_str = "(EMPTY TITLE)";
            viewAppendRowWrap(view, title_str, strlen(title_str), act_style);
        }
        vrowAppend(&view->rows[view->nrows-1], buf,
                   view->as.act.body_len - view->rows[view->nrows-1].len,
                   title_style);

        // Date
        TextStyle dt_style = MAKE_TEXT_STYLE(act_fgcolor, 
                                             act_bgcolor,
                                             TEXT_STYLE_NONE);
        char date_str[64];
        int date_len = getTimeStr(date_str, sizeof(date_str),
                                  ACTIVITY_DATE_FMT, act->start_ts);

        viewAppendRowWrap(view, date_str, date_len, dt_style);
        vrowAppend(&view->rows[view->nrows-1], buf,
                   view->as.act.body_len - view->rows[view->nrows-1].len,
                   act_style);

        // Time
        char start_str[16], end_str[16];
        int start_str_len = getTimeStr(start_str, sizeof(start_str),
                                       ACTIVITY_TIME_FMT, act->start_ts);
        int end_str_len = getTimeStr(end_str, sizeof(end_str), 
                                     ACTIVITY_TIME_FMT, act->end_ts);
        char time_str[start_str_len+1+end_str_len+1];
        int time_len = snprintf(time_str, sizeof(time_str), "%s-%s", 
                                start_str, end_str);

        viewAppendRowWrap(view, time_str, time_len, dt_style);
        vrowAppend(&view->rows[view->nrows-1], buf,
                   view->as.act.body_len - view->rows[view->nrows-1].len,
                   act_style);

        // Category
        TextStyle category_style = MAKE_TEXT_STYLE(act_fgcolor,
                                                   act_bgcolor,
                                                   TEXT_STYLE_ITALIC);
        if (act->category && act->category->name) {
            viewAppendRowWrap(view, act->category->name, 
                              strlen(act->category->name), category_style);
        } else {
            const char *category_str = "(UNCATEGORIZED)";
            viewAppendRowWrap(view, category_str, strlen(category_str),
                              category_style);
        }
        vrowAppend(&view->rows[view->nrows-1], buf,
                   view->as.act.body_len - view->rows[view->nrows-1].len,
                   act_style);

        // Notes
        if (act->notes) {
            TextStyle notes_style = MAKE_TEXT_STYLE(act_fgcolor,
                                                    act_bgcolor,
                                                    TEXT_STYLE_ITALIC);
            viewAppendRowWrap(view, act->notes, strlen(act->notes), 
                              notes_style);
            vrowAppend(&view->rows[view->nrows-1], buf,
                    view->as.act.body_len - view->rows[view->nrows-1].len,
                    act_style);
        }

        // Add the activity bounding box.
        struct bounding_box bb = {
            .x      = view->as.act.body_off,
            .y      = act_start_row-1, 
            .width  = view->as.act.body_len,
            .height = view->nrows - act_start_row,
        };
        daAdd(&view->bbox_lists[i], bb);

        viewAppendEmpty(view, 1); // empty line
    }

    /* Footer */
    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[header_idx]);

    free(buf);
}

/* ============================= Category view ============================== */

/* Adds the categories and their bounding boxes to the view by recursively
 * traversing the category tree pointed by 'node'. 
 * 'depth' is the level of the node in the category tree. */
#define addCategoriesRecursive(view, node) \
    addCategoriesRecursiveImpl((view), (node), 0)

void addCategoriesRecursiveImpl(View *view, struct catnode *node, int depth) {
    if (node == NULL) return;

    int y = view->as.cat.header_rows + view->entity_count;
    struct bounding_box bb = {
        .x = 0,
        .y = y, 
        .width = strlen(node->cat->name), 
        .height = 1, 
    };
    viewAddCategory(view, view->entity_count, node->cat, &bb, 1, depth);

    for (size_t i = 0; i < node->child_count; i++) {
        addCategoriesRecursiveImpl(view, node->children[i], depth+1);
    }
}

/* Init the category view. */
void initCategoryView(View *view) {
    view->as.cat.header_rows = 2;
    view->as.cat.depths = NULL;

    initView(view, VIEW_CATEGORY, Mycal.win_width, 0, NULL, 0);

    CategoryNodeList trees = {0};
    daInit(&trees);
    getCategoryTrees(Mycal.db, &trees);

    /* Add categories by traversing the trees */
    for (size_t r = 0; r < trees.count; r++) {
        struct catnode *root = trees.items[r];
        addCategoriesRecursive(view, root);
    }

    daFreeEach(&trees, freeCategoryNode);
}

/* Build the category view rows. */
void populateCategoryViewRows(View *view) {
    size_t bufsize = view->ncols + 1;
    char  *buf = Malloc(bufsize);
    int buflen;

    /* Header */
    buflen = snprintf(buf, bufsize, "Categories %zu", view->entity_count);
    viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);
    int header_idx = view->nrows-1;
    viewAppendEmpty(view, 1); // empty line
    view->as.cat.header_rows = view->nrows;

    /* Body (category rows)
     * The categories are rendered as a tree structure.
     * Each category has a depth value that indicate the level of the node
     * in the category node tree.
     *
     * The ├ └ │ ─ non standard ASCII characters are used to draw the 
     * branches of the tree.
     *
     * root
     * ├── cat1
     * │   ├── subCat1
     * │   │   ├── subSubCat1
     * │   │   └── subSubCat2
     * │   └── subCat2
     * ├── cat2
     * │   └── subCat3
     * └── cat3
     *     └── subCat4
     *         └── subSubCat3
     *             └── subSubSubCat1
     */

    int max_depth = 0;
    for (size_t i = 0; i < view->entity_count; i++) {
        if (view->as.cat.depths[i] > max_depth) 
            max_depth = view->as.cat.depths[i];
    }
    
    // Keep track of whether an ancestor at a given depth
    // has more siblings further down in the entity array.
    int active_siblings[max_depth+1];
    memset(active_siblings, 0, sizeof(active_siblings));

    /* Build the category rows */
    for (size_t i = 0; i < view->entity_count; i++) {
        int depth = view->as.cat.depths[i];
        buflen = 0;

        // Look ahead to see if this node has a next sibling.
        // A sibling exists if we find another node with the exact 
        // same depth before we hit the end of the array.
        int has_next_sibling = 0;
        for (size_t j = i+1; j < view->entity_count; j++) {
            if (view->as.cat.depths[j] < depth) {
                break; // end of subtree
            } else if (view->as.cat.depths[j] == depth) {
                has_next_sibling = 1;
                break;
            }
        }

        active_siblings[depth] = has_next_sibling;

        // Draw the branch line for ancestors.
        // We start at k=1 because depth 0 (root) doesn't have a
        // branch line.
        for (int k = 1; k < depth; k++) {
            if (active_siblings[k]) {
                buflen += snprintf(buf+buflen, bufsize-buflen, "│   ");
            } else {
                buflen += snprintf(buf+buflen, bufsize-buflen, "    ");
            }
        }

        Category *cat = (Category *)view->entities[i];

        // Draw the branch line for current node.
        if (depth > 0) {
            if (active_siblings[depth]) { // intermediate child
                buflen += snprintf(buf+buflen, bufsize-buflen, "├── ");
            } else {                      // last child
                buflen += snprintf(buf+buflen, bufsize-buflen, "└── ");
            }
        }

        // Update the bounding box position of the category.
        for (size_t j = 0; j < view->bbox_lists[i].count; j++) {
            view->bbox_lists[i].items[j].x = buflen;
        }

        viewAppendRow(view, buf, buflen, TEXT_STYLE_DEFAULT);

        // Append category name.
        Color cat_fgcolor = cat->color == COLOR_WHITE ? COLOR_BLACK :
                                                        COLOR_DEFAULT;
        TextStyle cat_style = MAKE_TEXT_STYLE(cat_fgcolor,
                                              cat->color,
                                              TEXT_STYLE_BOLD);
        vrowAppend(&view->rows[view->nrows-1], cat->name, strlen(cat->name), 
                   cat_style);

        // Append the activity count.
        int act_count = countActivitiesForCategory(Mycal.db, cat->id);
        buflen = snprintf(buf, bufsize, " (%d)", act_count);
        TextStyle count_style = MAKE_TEXT_STYLE(COLOR_DEFAULT,
                                                COLOR_DEFAULT,
                                                TEXT_STYLE_ITALIC);
        vrowAppend(&view->rows[view->nrows-1], buf, buflen, count_style);
    }

    /* Footer */
    viewAppendEmpty(view, 1); // empty line
    viewAppendEmpty(view, 1);
    viewCopyRow(view, view->nrows-1, &view->rows[header_idx]);

    free(buf);
}

/* ================================== Form ================================== */

#define MAX_LOG_MESSAGES 16

/* Represent the buffer that contains messages with associated log level. */
struct log_buf {
    char msgs[MAX_LOG_MESSAGES][256];
    Dtparse_LogLevel levels[MAX_LOG_MESSAGES];
    int count;
};

/* Represent a field of the form. */
typedef struct {
    const char *name;   /* Name of the field */
    const char *prompt; /* Message to ask for user input */
    const char *buffer; /* Buffer to store the typed characters */
    size_t bufsize;     /* Maximum length for the input buffer */
    int start_row;      /* Start row index in 'Form' for this field */
} FormField;

/* Represent a generic form for prompting the user. */
typedef struct {
    const char *title;      /* Title of the form */
    FormField *fields;      /* Fields */
    int field_count;        /* Number of fields */
    struct log_buf logbuf;  /* Buffer to store the logs of the form */
} Form;

/* Holds all the UI rendering state and styles for a form.
 * The form UI is made up of three components:
 * 1. The form itself that is compiled by the user.
 * 2. The log referred to the input of the form.
 * 3. The status bar to indicate the actions to perform and let the user 
 *    input the text for the form fields. 
 */
typedef struct {
    TextStyle info_style;
    TextStyle warn_style;
    TextStyle err_style;
    TextStyle prompt_style;

    View     form_view;
    Renderer form_renderer;

    View     log_view;
    Renderer log_renderer;

    struct status_bar sbar;
} FormUI;

/* Add message to the log buffer. */
void addLog(struct log_buf *log, Dtparse_LogLevel level, const char *msg) {
    if (log->count >= MAX_LOG_MESSAGES) return;

    log->levels[log->count] = level;
    snprintf(log->msgs[log->count], sizeof(log->msgs[log->count]), "%s", msg);
    log->count++;
}

/* Save the diagnostic messages from dtparse to the log buffer ('ctx'). */
void getDtparseLog(Dtparse_LogLevel level, int line, int col, 
                   const char *msg, void *ctx) 
{
    struct log_buf *log = (struct log_buf *)ctx;

    char buf[256];
    int pos = 0;

    if (line != -1 && col != -1) {
        pos += snprintf(buf+pos, sizeof(buf) - pos, "[%d:%d] ", line, col);
    }

    if (level == DTPARSE_LOG_INFO) {
        pos += snprintf(buf+pos, sizeof(buf) - pos, "INFO ");
    } else if (level == DTPARSE_LOG_WARNING) {
        pos += snprintf(buf+pos, sizeof(buf) - pos, "WARNING ");
    } else if (level == DTPARSE_LOG_ERROR) {
        pos += snprintf(buf+pos, sizeof(buf) - pos, "ERROR ");
    }

    snprintf(buf+pos, sizeof(buf) - pos, "%s", msg);

    addLog(log, level, buf);
}

/* Display the man page using a pager (like less) */
static void showDtparseManPage(void) {
    // Open a pipe to 'less'
    FILE *pager = popen("less", "w");
    if (pager) {
        fputs(dtparse_getManPage(), pager);
        pclose(pager);
    } else {
        fprintf(stderr, "`less` command is not available."
                " Try with `%s --help-datetime`\n", Mycal.progname);
    }
}

/* Initialize the form. */
void formInit(Form *form, const char *title, 
              FormField *fields, int field_count)
{
    form->title        = title;
    form->fields       = fields;
    form->field_count  = field_count;
    form->logbuf.count = 0;
}

/* Clear the logs of the form. */
void formClearLogs(Form *form) {
    form->logbuf.count = 0;
}

/* Initialize the form UI content at (x,y) position of the screen. */
void formInitUI(FormUI *ui, int x, int y) {
    ui->info_style   = FORM_INFO_STYLE;
    ui->warn_style   = FORM_WARN_STYLE;
    ui->err_style    = FORM_ERR_STYLE;
    ui->prompt_style = FORM_PROMPT_STYLE;

    int form_cols = (Mycal.win_width > MAX_FORM_WIDTH) ? 
        MAX_FORM_WIDTH : Mycal.win_width;
    ui->form_view = (View){
        .type = VIEW_CUSTOM,
        .ncols = form_cols,
        .nrows = 0,
    };

    ui->form_renderer = (Renderer){
        .x = x,
        .y = y,
        .rowoff = 0,    
        .coloff = 0,    
        .screenrows = 0, // sets during rendering (formRender function)
        .screencols = Mycal.win_width,
        .text_align = RENDERER_ALIGN_CENTER_BLOCK,
    };

    ui->log_view = (View){
        .type = VIEW_CUSTOM,
        .ncols = Mycal.win_width,
        .nrows = 0,
    };

    ui->log_renderer = (Renderer){
        .x = x,          // sets during rendering (formRender function)
        .y = y,          // sets during rendering (formRender function)
        .rowoff = 0,    
        .coloff = 0,    
        .screenrows = 0, // sets during rendering (formRender function)
        .screencols = Mycal.win_width,
        .text_align = RENDERER_ALIGN_START,
    };

    ui->sbar = (struct status_bar){
        .screencols = Mycal.win_width,
        .prompt       = NULL,
        .prompt_len   = 0,
        .input        = NULL,
        .input_len    = 0,
        .status       = NULL,
        .status_len   = 0,
        .cursor_pos   = 0,
        .input_scroll = NULL,
    };
}

/* Free the memory allocated by the form UI. */
void formFreeUI(FormUI *ui) {
    freeView(&ui->form_view);
    freeView(&ui->log_view);
}

/* Builds the fields of the form component. */
void formBuildFields(Form *form, FormUI *ui) {
    size_t bufsize = ui->form_view.ncols + 1;
    char  *buf = Malloc(bufsize);

    /* Build header */
    char dashes[ui->form_view.ncols+1];
    memset(dashes, '=', sizeof(dashes)-1);
    dashes[sizeof(dashes)-1] = '\0';

    int avail_dashes = ui->form_view.ncols-1-strlen(form->title)-1;
    if (avail_dashes < 0) avail_dashes = 0;
    int left_dash_count  = avail_dashes/2;
    int right_dash_count = avail_dashes-left_dash_count;

    int headlen = snprintf(buf, bufsize, "%.*s %s %.*s", 
                           left_dash_count,  dashes, 
                           form->title, 
                           right_dash_count, dashes);
    if (headlen >= (int)bufsize) headlen = bufsize-1;
    viewAppendRow(&ui->form_view, buf, headlen, TEXT_STYLE_DEFAULT);

    /* Build fields */
    for (int i = 0; i < form->field_count; i++) {
        FormField *field = &form->fields[i];
        const char *name_str   = field->name   ? field->name   : "";
        const char *buffer_str = field->buffer ? field->buffer : "";

        int field_len = snprintf(NULL, 0, "%s: %s", name_str, buffer_str);
        char *field_buf = Malloc(field_len + 1);
        snprintf(field_buf, field_len+1, "%s: %s", name_str, buffer_str);

        field->start_row = ui->form_view.nrows;
        viewAppendRowWrap(&ui->form_view, field_buf, field_len,
                          TEXT_STYLE_DEFAULT);
        free(field_buf);
    }

    /* Build footer */
    memset(buf, '=', headlen);
    buf[headlen] = '\0';
    viewAppendRow(&ui->form_view, buf, headlen, TEXT_STYLE_DEFAULT);

    free(buf);
}

/* Builds logs of the form component. 
 * 'input' is a NULL terminated string that contains the input of the user. */
void formBuildLogs(Form *form, FormUI *ui, const char *input) {
    /* Build logs */
    if (form->logbuf.count > 0) {
        int buflen = snprintf(NULL, 0, "Input: `%s`", input);
        char *buf  = Malloc(buflen + 1);
        snprintf(buf, buflen+1, "Input: `%s`", input);
        viewAppendRowWrap(&ui->log_view, buf, buflen, TEXT_STYLE_DEFAULT);
        free(buf);

        for (int i = 0; i < form->logbuf.count; i++) {
            const char *msg = form->logbuf.msgs[i];
            int msg_len = strlen(msg);
            TextStyle style = TEXT_STYLE_DEFAULT;

            switch (form->logbuf.levels[i]) {
                case DTPARSE_LOG_INFO:
                    style = ui->info_style;
                    break;
                case DTPARSE_LOG_WARNING:
                    style = ui->warn_style;
                    break;
                case DTPARSE_LOG_ERROR:
                    style = ui->err_style;
                    break;
            }

            viewAppendRowWrap(&ui->log_view, msg, msg_len, style);
        }
    }
}

/* Set the 'field' as current field in the form UI . */
void selectField(FormUI *ui, FormField *field) {
    ui->form_view.rows[field->start_row].chars[2] = '*';
}

/* Render the UI of the form. */
void formRender(FormUI *ui) {
    /* Render fields */
    int start_y = ui->form_renderer.y;
    int avail_height = Mycal.win_height - start_y - 1; // -1 status bar
    int form_height = ui->form_view.nrows;
    if (form_height > avail_height) form_height = avail_height;

    avail_height -= form_height;

    ui->form_renderer.screenrows = form_height;
    viewRenderRows(&ui->form_view, &ui->form_renderer);

    /* Render logs */
    int log_height = ui->log_view.nrows;
    if (log_height > avail_height) log_height = avail_height;

    avail_height -= log_height;

    ui->log_renderer.y = start_y + form_height + avail_height;
    ui->log_renderer.screenrows = log_height;

    for (int i = 0; i < avail_height; i++) {
        printf("\x1b[K\n"); // clear line and add empty line
    }
    viewRenderRows(&ui->log_view, &ui->log_renderer);
}

/* Rebuild and render the UI of the form. */
void formRebuildAndRender(Form *form, FormUI *ui, const char *input) {
    /* Clear previous frame */
    clearView(&ui->form_view);
    clearView(&ui->log_view);

    formBuildFields(form, ui);
    formBuildLogs(form, ui, input);

    formRender(ui);
}

/* Render the status bar for the 'field'. */
void formRenderStatusBar(struct status_bar *sbar, TextStyle style,
                         FormField *field, 
                         const char *input, size_t input_len)
{
    int chars_left = field->bufsize-1 - input_len;

    char status[32];
    int status_len = snprintf(status, sizeof(status), " %d/%zu", 
                              chars_left, field->bufsize-1);

    sbar->prompt     = field->prompt;
    sbar->prompt_len = field->prompt ? strlen(field->prompt) : 0;
    sbar->input      = input;
    sbar->input_len  = input_len;
    sbar->status     = status;
    sbar->status_len = status_len;

    int cursor_col = renderStatusBar(sbar, style);
    printf("\x1b[%dG", cursor_col); // set cursor at active input column 
    fflush(stdout);
}

/* Process a form field interactively. 
 * Writes at most 'bufsize'-1 characters in the buffer by reading user inputs.
 * The input is dynamically rendererd in the UI status bar.
 * Returns 1 if the field was completed successfully, 0 if canceld, 
 * return the pressed UP_ARROW or DOWN_ARROW key otherwise. */
int processField(FormUI *ui, FormField *field, char *buf, size_t bufsize) {
    int res = 0;

    int buflen = strlen(buf);
    int cursor_pos = buflen; // cursor index in 'buf'
    int input_scroll = 0;    // offset for scrolling in 'buf'
    ui->sbar.input_scroll = &input_scroll;

    setTextStyle(ui->prompt_style);
    enableRawMode();

    /* Render status bar */
    printf("\x1b[G"); // set cursor at starting screen column
    ui->sbar.cursor_pos = cursor_pos;
    formRenderStatusBar(&ui->sbar, ui->prompt_style, field, buf, buflen);

    /* Build the input string */
    while (1) {
        int c = readKey();
        switch (c) {
        case CTRL_C:
        case CTRL_Q:
        case ESC:
            res = 0;
            goto eoi_field;
        case ENTER:
        case '\n':
            res = 1;
            goto eoi_field;
        case KEY_ARROW_LEFT:
            if (cursor_pos > 0) cursor_pos--;
            break;
        case KEY_ARROW_RIGHT:
            if (cursor_pos < buflen) cursor_pos++;
            break;
        case KEY_ARROW_UP:
        case KEY_ARROW_DOWN:
            res = c;
            goto eoi_field;
        case BACKSPACE:
        case CTRL_H:
            if (cursor_pos > 0) {
                memmove(&buf[cursor_pos-1], 
                        &buf[cursor_pos], 
                        (buflen-cursor_pos) * sizeof(char));
                cursor_pos--;
                buflen--;
                buf[buflen] = '\0';
            }
            break;
        default:
            if (c >= 32 && c <= 126) {
                if (buflen < (int)field->bufsize-1 && 
                    buflen < (int)bufsize-1) {
                    memmove(&buf[cursor_pos+1], 
                            &buf[cursor_pos], 
                            (buflen-cursor_pos) * sizeof(char));
                    buf[cursor_pos] = (char)c;
                    cursor_pos++;
                    buflen++;
                    buf[buflen] = '\0';
                }
            }
            break;
        }

        /* Render status bar */
        printf("\x1b[G"); // set cursor at starting screen column
        ui->sbar.cursor_pos = cursor_pos;
        formRenderStatusBar(&ui->sbar, ui->prompt_style, field, buf, buflen);
    }

eoi_field: // end of input field
    disableRawMode();
    setTextStyle(TEXT_STYLE_DEFAULT);
    ui->sbar.input_scroll = NULL;
    return res;
}

/* ======================== User activity management ======================== */

/* Return the string that best fits the status bar (the first one that 
 * doesn't overflow).
 * The 'n' strings are provided as variadic arguments, sorted by their 
 * length from the longest to the shortest. 
 * The function checks the status bar prompt for each of this strings until
 * there is one that does not overlow.
 * If no strings are provided, it returns NULL. */
const char *fitToStatusBar(struct status_bar *sbar, int n, ...) {
    const char *str = NULL;

    va_list args;
    va_start(args, n);
    for (int i = 0; i < n; i++) {
        str = va_arg(args, const char *);
        sbar->prompt = str;
        sbar->prompt_len = strlen(str);
        if (!isStatusBarOverflow(sbar)) break;
        else str = NULL;
    }
    
    va_end(args);
    return str;
}

/* Render the activity. */
void renderActivity(Activity *act) {
    char start_str[START_STR_SIZE] = {0};
    char end_str[END_STR_SIZE]     = {0};

    if (act->start_ts != (time_t)-1) {
        getTimeStr(start_str, sizeof(start_str), 
                   FULL_DATETIME_FMT, act->start_ts);
    }

    if (act->end_ts != (time_t)-1) {
        getTimeStr(end_str, sizeof(end_str), 
                   FULL_DATETIME_FMT, act->end_ts);
    }

    Form form;
    FormUI form_ui;
    FormField fields[] = {
        { " Title",      NULL, (char *)act->title, TITLE_SIZE,        0 },
        { " Start Time", NULL, start_str,          sizeof(start_str), 0 },
        { " End Time",   NULL, end_str,            sizeof(end_str),   0 },
        { " Notes",      NULL, (char *)act->notes, NOTES_SIZE,        0 },
        { " Category",   NULL, (act->category) ? 
            (char *)act->category->name : NULL, CATEGORY_NAME_SIZE, 0 },
    };

    int field_count = sizeof(fields)/sizeof(fields[0]);
    formInit(&form, "Activity", fields, field_count);
    formInitUI(&form_ui, 0, 0);

    formRebuildAndRender(&form, &form_ui, "");
    formFreeUI(&form_ui);
}

/* Render the category. */
void renderCategory(Category *cat) {
    Category *parent = getCategoryById(Mycal.db, cat->parent_id);

    char color_buf[32];
    int len = 0;
    TextStyle cat_style = MAKE_TEXT_STYLE(COLOR_DEFAULT, 
                                          cat->color, 
                                          TEXT_STYLE_NONE);
    len += getTextStyleStr(color_buf, sizeof(color_buf), cat_style);
    len += snprintf(color_buf+len, sizeof(color_buf)-len, "    ");
    len += getTextStyleStr(color_buf+len, sizeof(color_buf)-len,
                           TEXT_STYLE_DEFAULT);
    color_buf[sizeof(color_buf)-1] = '\0';

    Form form;
    FormUI form_ui;
    FormField fields[] = {
        { " Name",   NULL, (char *)cat->name, CATEGORY_NAME_SIZE, 0 },
        { " Color",  NULL, color_buf,         sizeof(color_buf),  0 },
        { " Parent", NULL, (parent) ? (char *)parent->name : NULL, 
                           sizeof(CATEGORY_NAME_SIZE), 0 },
    };

    int field_count = sizeof(fields)/sizeof(fields[0]);
    formInit(&form, "Category", fields, field_count);
    formInitUI(&form_ui, 0, 0);

    formRebuildAndRender(&form, &form_ui, "");
    formFreeUI(&form_ui);
    if (parent) freeCategory(parent);
}

/* Update the the bounding box style by applying the text style 'flags'.
 * 'orig_styles' is a 1D array that contains the text style for each 
 * character of the bounding box. */
void viewUpdateBBoxStyle(View *view, struct bounding_box *bb, 
                         TextStyle *orig_styles, int flags) {
    for (int y = 0; y < bb->height; y++) {
        for (int x = 0; x < bb->width; x++) {
            struct vrow *row = &view->rows[bb->y + y];
            TextStyle base_style = orig_styles[y * bb->width + x];
            TextStyle new_style  = MAKE_TEXT_STYLE(
                                       GET_FG(base_style), 
                                       GET_BG(base_style),
                                       GET_FLAGS(base_style) | flags);
            vrowSet(row, bb->x+x, row->chars+bb->x+x, 1, new_style);
        }
    }
}

/* Interactively select an entity from the view.
 * The function renders the view and lets the user choose an entity by
 * scrolling through the available ones.
 * The selection process starts at entity index 'start_idx' (if it is not -1), 
 * starts at index 0 otherwise.
 * Return the selected entity if any, NULL otherwise. 
 * The 'prompt' message is rendered in the status bar to guide the user on
 * the actions he can perform. */
void *selectEntityFromUser(View *view, Renderer *r, int start_idx, 
                           const char *prompt) 
{
    void *e = NULL;

    if (view->entities == NULL || view->entity_count == 0) return NULL;

    /* Build the prompt message for the status bar.
     * The same prompt message is provided in different length and is 
     * choose the longest one that does not overflow. */
    struct status_bar sbar = {
        .screencols   = r->screencols,
        .prompt       = NULL,
        .prompt_len   = 0,
        .input        = NULL,
        .input_len    = 0,
        .status       = NULL,
        .status_len   = 0,
        .cursor_pos   = 0,
        .input_scroll = NULL,
    };

    sbar.status_len = snprintf(NULL, 0, " %zu/%zu", 
                               view->entity_count, view->entity_count);

    int prompt_len = strlen(prompt);
    char prompt_msgs[3][256];

    snprintf(prompt_msgs[0], sizeof(prompt_msgs[0]), 
            "%s%sPress q to quit. Use ARROWS to move. Press ENTER to select.",
            prompt, (prompt_len > 0) ? " " : "");
    snprintf(prompt_msgs[1], sizeof(prompt_msgs[1]), 
            "%s%sq quit, ARROWS move, ENTER select.", 
            prompt, (prompt_len > 0) ? " " : "");
    snprintf(prompt_msgs[2], sizeof(prompt_msgs[2]), 
            "%s%sUse q, ARROWS and ENTER.", 
            prompt, (prompt_len > 0) ? " " : "");

    const char *prompt_msg = fitToStatusBar(&sbar, 3, 
                                            prompt_msgs[0], 
                                            prompt_msgs[1], 
                                            prompt_msgs[2]);
    if (prompt_msg == NULL) prompt_msg = prompt_msgs[2];
    sbar.prompt     = prompt_msg;
    sbar.prompt_len = strlen(sbar.prompt);

    enableRawMode();

    int prev_idx = -1;
    int selected_idx = 0;
    if (start_idx > 0 && (size_t)start_idx < view->entity_count) 
        selected_idx = start_idx;

    int blink = 0;
    TextStyle *orig_styles = NULL;
    size_t orig_size = 0;

    while (1) {
        /* Render the selected entity on changes */
        if (selected_idx != prev_idx) {
            if (prev_idx != -1) {
                // Restore the previous entity to its original style.
                int style_idx = 0;
                for (size_t b = 0; b < view->bbox_lists[prev_idx].count; b++) {
                    struct bounding_box prev_box = 
                        view->bbox_lists[prev_idx].items[b];
                    viewUpdateBBoxStyle(view, &prev_box, 
                                        orig_styles+style_idx,
                                        TEXT_STYLE_NONE);
                    style_idx += prev_box.width * prev_box.height;
                }
            }

            /* Store original styles for all the characters in the 
             * bounding boxes. */
            size_t bbox_count = view->bbox_lists[selected_idx].count;

            int total_styles = 0;
            for (size_t b = 0; b < bbox_count; b++) {
                struct bounding_box bb =
                    view->bbox_lists[selected_idx].items[b];
                total_styles += bb.width * bb.height;
            }

            size_t new_size = total_styles * sizeof(TextStyle);
            if (orig_size < new_size) {
                orig_styles = Realloc(orig_styles, new_size);
                orig_size = new_size;
            }

            int style_idx = 0;
            for (size_t b = 0; b < bbox_count; b++) {
                struct bounding_box bb =
                    view->bbox_lists[selected_idx].items[b];
                for (int y = 0; y < bb.height; y++) {
                    for (int x = 0; x < bb.width; x++) {
                        struct vrow *row = &view->rows[bb.y + y];
                        TextStyle base_style = row->tstyle[bb.x + x];
                        orig_styles[style_idx++] = base_style;
                    }
                }
            }

            /* Place entity at the center of the screen.
             * Consider only the first bounding box. */
            assert(bbox_count > 0);
            struct bounding_box first_bb =
                view->bbox_lists[selected_idx].items[0];

            // Vertical alignment
            if (first_bb.height < r->screenrows) {
                r->rowoff = first_bb.y - (r->screenrows/2) 
                            + (first_bb.height/2);
            } else {
                r->rowoff = first_bb.y-(r->screenrows/2);
            }

            if (r->rowoff + r->screenrows > view->nrows) 
                r->rowoff = view->nrows - r->screenrows;
            if (r->rowoff < 0) r->rowoff = 0;

            // Horizontal alignment
            if (first_bb.width < r->screencols) {
                r->coloff = first_bb.x - (r->screencols/2) 
                            + (first_bb.width/2);
            } else  {
                r->coloff = first_bb.x - (r->screencols/2);
            }

            if (r->coloff + r->screencols > view->ncols)
                r->coloff = view->ncols - r->screencols;
            if (r->coloff < 0) r->coloff = 0;

            prev_idx = selected_idx;
            blink = 1;
        }

        /* Render selected entity.
         * Note that the ANSI blinking effect only applies to the text.
         * Since we want the background to blink too, we need to manually
         * create the blink effect by switching between the base and inverse
         * styles. */
        int style_idx = 0;
        for (size_t b = 0; b < view->bbox_lists[selected_idx].count; b++) {
            struct bounding_box bb = view->bbox_lists[selected_idx].items[b];
            viewUpdateBBoxStyle(view, &bb, orig_styles+style_idx,
                                blink ? TEXT_STYLE_INVERT : TEXT_STYLE_NONE);
            style_idx += bb.width * bb.height;
        }

        viewRenderRows(view, r);

        /* Render status bar */
        TextStyle status_style = TEXT_STYLE_INV;
        char status[32];
        sbar.status_len = snprintf(status, sizeof(status), " %d/%zu", 
                                   selected_idx+1, view->entity_count);
        sbar.status = status;
        renderStatusBar(&sbar, status_style);

        /* Wait for input with timeout */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = BLINK_MS * 1000;

        int ready = select(STDIN_FILENO+1, &readfds, NULL, NULL, &tv);
        
        /* Handle user input */
        if (ready > 0) {
            int c = readKey();
            switch (c) {
            case CTRL_C:
            case CTRL_Q:
                exit(0);
            case 'q':
                e = NULL;
                goto selection_end;
            case ENTER:
            case '\n':
                e = view->entities[selected_idx];
                goto selection_end;
            case KEY_ARROW_UP:
            case KEY_ARROW_LEFT:
                if (selected_idx > 0) selected_idx--;
                break;
            case KEY_ARROW_DOWN:
            case KEY_ARROW_RIGHT:
                if ((size_t)selected_idx < view->entity_count-1) 
                    selected_idx++;
                break;
            }
        } else if (ready == 0) {
            blink = !blink;
        }
    }

selection_end:
    /* Restore the last selected entity to its original style. */
    int style_idx = 0;
    for (size_t b = 0; b < view->bbox_lists[selected_idx].count; b++) {
        struct bounding_box bb = view->bbox_lists[selected_idx].items[b];
        viewUpdateBBoxStyle(view, &bb, orig_styles+style_idx, TEXT_STYLE_NONE);
        style_idx += bb.width * bb.height;
    }

    viewRenderRows(view, r);
    if (orig_styles) free(orig_styles);
    disableRawMode();
    return e;
}

/* Build and return the category by prompting the user with a form.
 * If an original category is provided, it is used as default values for 
 * the form fields. */
Category *makeCategoryFromUser(Category *orig_cat, const char *form_title) {
    Category *cat = NULL;

    char name[CATEGORY_NAME_SIZE]        = {0};
    char parent_name[CATEGORY_NAME_SIZE] = {0};

    Color color      = 0;
    size_t parent_id = 0;

    int has_category = orig_cat != NULL;

    /* Initialize the fields */
    if (has_category) {
        strncpy(name, orig_cat->name, sizeof(name)-1);

        Category *orig_parent = getCategoryById(Mycal.db, orig_cat->parent_id);
        if (orig_parent) {
            strncpy(parent_name, orig_parent->name, sizeof(parent_name)-1);
            freeCategory(orig_parent);
        }
    }

    /* Build the prompts.
     * Here we use an input_len of 1 to tell the status bar 
     * that we are intented to add the input later. 
     * (see getStatusBarLengths function) */
    int input_len = 1;
    struct status_bar sbar = {
        .screencols   = Mycal.win_width,
        .prompt       = NULL,
        .prompt_len   = 0,
        .input        = NULL,
        .input_len    = input_len,
        .status       = NULL,
        .status_len   = 0,
        .cursor_pos   = 0,
        .input_scroll = NULL,
    };

    sbar.status_len = snprintf(NULL, 0, " %d/%d",
                               CATEGORY_NAME_SIZE-1, 
                               CATEGORY_NAME_SIZE-1);

    /* We try with long, medium and short prompts for properly display 
     * on different window sizes without overflow. */
    char prompt_names[3][256];

    snprintf(prompt_names[0], sizeof(prompt_names[0]), 
             "Enter Category Name: ");
    snprintf(prompt_names[1], sizeof(prompt_names[1]), 
             "Enter Name: ");
    snprintf(prompt_names[2], sizeof(prompt_names[2]), "Name: ");

    const char *prompt_name = fitToStatusBar(&sbar, 3, 
                                             prompt_names[0], 
                                             prompt_names[1], 
                                             prompt_names[2]);
    if (prompt_name == NULL) prompt_name = prompt_names[2];

    /* Build the form */
    Form form;
    FormUI form_ui;
    FormField fields[] = {
        { " ( ) Name",   prompt_name, name,        sizeof(name),        0 },
        { " ( ) Parent", NULL,        parent_name, sizeof(parent_name), 0 },
    };

    int field_count = sizeof(fields)/sizeof(fields[0]);
    formInit(&form, form_title, fields, field_count);
    formInitUI(&form_ui, 0, 0);

    char input[512] = {0};
    int step = 0; // track the current level in the form
    int max_step = 0;
    while (1) {
        if (step == 2) break;
        if (step > max_step) max_step = step;

        /* Render form */
        clearView(&form_ui.form_view);
        clearView(&form_ui.log_view);

        formBuildFields(&form, &form_ui);
        formBuildLogs(&form, &form_ui, input);
        selectField(&form_ui, &form.fields[step]);
        formRender(&form_ui); 
        int field_input_len = strlen(form.fields[step].buffer);
        form_ui.sbar.cursor_pos = field_input_len;
        formRenderStatusBar(&form_ui.sbar, form_ui.prompt_style,
                            &form.fields[step],
                            form.fields[step].buffer, field_input_len);

        int last_field_step = 0;
        if (step <= last_field_step) {
            // Copy the original input to start typing from it.
            strncpy(input, fields[step].buffer, fields[step].bufsize-1);
            input[fields[step].bufsize-1] = '\0';

mkcat_cnt_input:
            int c = processField(&form_ui, &form.fields[step],
                                 input, sizeof(input));
            if (!c) {
                cat = NULL;
                goto cleanup_mkcat;
            }

            if (c == KEY_ARROW_UP) {
                if (step > 0) {
                    formClearLogs(&form);
                    step--;
                    continue;
                }
                goto mkcat_cnt_input;
            } else if (c == KEY_ARROW_DOWN) {
                if (step <= last_field_step &&
                    (step < max_step || has_category)) {
                    formClearLogs(&form);
                    /* always check the step conditions when skipping a 
                     * field down */
                } else {
                    goto mkcat_cnt_input;
                }
            }

            char *r;
            if ((r = strchr(input, '\n'))) *r = '\0'; // remove new line
            input_len = strlen(input);
        }

        /* Parse the input.
         * Write successful input into form field. */

        /* Name */
        if (step == 0) {
            formClearLogs(&form);

            // Empty name
            if (input_len == 0) {
                addLog(&form.logbuf, DTPARSE_LOG_ERROR, 
                        "ERROR: Name cannot be empty.");
            }
            // New name 
            else {
                // Check if exists a category with the same name.
                size_t exclude_id = (has_category) ? orig_cat->id : 0;
                if (categoryNameExists(Mycal.db, input, exclude_id)) {
                    addLog(&form.logbuf, DTPARSE_LOG_ERROR,
                            "ERROR: Category name already exists. Please use a different name.");
                } else {
                    strncpy(name, input, sizeof(name)-1);
                    name[sizeof(name)-1] = '\0';
                    step++;
                }
            }
        }
        /* Parent category */
        else if (step == 1) {
            View pview;
            initCategoryView(&pview);

            // add NONE category
            int y = pview.as.cat.header_rows;
            Category *none = makeCategory("NONE", UNCAT_COLOR, 0);
            none->id = CATEGORY_ID_NONE;
            struct bounding_box none_bb = {
                .x      = 0,
                .y      = y,
                .width  = strlen(none->name),
                .height = 1, 
            };
            viewAddCategory(&pview, 0, none, &none_bb, 1, 0);

            // Remove the 'orig_cat' category. We don't want to choose 
            // self category as parent.
            if (has_category) {
                Category *same_cat = viewRemoveCategory(&pview, orig_cat->id);
                if (same_cat) freeCategory(same_cat);
            }

            populateCategoryViewRows(&pview);

            int rend_rows = Mycal.win_height - form_ui.form_renderer.y -
                form_ui.form_renderer.screenrows - 1; // -1 for status bar
            Renderer r = {
                .x = form_ui.form_renderer.x,
                .y = form_ui.form_renderer.y 
                     + form_ui.form_renderer.screenrows,
                .rowoff     = 0,
                .coloff     = 0,
                .screenrows = rend_rows,
                .screencols = Mycal.win_width,
                .vert_scroll = 1,
                .horz_scroll = 1,
                .text_align = RENDERER_ALIGN_START,
            };

            // Make the selection points to the original category if any.
            int select_idx = 0;
            if (has_category) {
                for (size_t i = 0; i < pview.entity_count; i++) {
                    if (orig_cat->parent_id == 
                        ((Category *)pview.entities[i])->id) {
                        select_idx = i;
                        break;
                    }
                }
            }

            Category *parent = selectEntityFromUser(&pview, &r, select_idx, 
                    "Select the parent category.");

            // Orig parent
            if (parent == NULL && has_category) {
                parent_id = orig_cat->parent_id;
                color = orig_cat->color;
            }
            // NONE parent
            else if (parent == NULL || parent->id == CATEGORY_ID_NONE) {
                parent_id = 0;
                color = getUnusedColor();
                parent_name[0] = '\0';
            } 
            // Chosen parent
            else {
                parent_id = parent->id;
                color = parent->color;
                strncpy(parent_name, parent->name, sizeof(parent_name)-1);
                parent_name[sizeof(parent_name)-1] = '\0';
            }

            freeView(&pview);
            step++;
        }
    }

    /* Render one last time to update the last form field. */
    formRebuildAndRender(&form, &form_ui, input); 

    cat = makeCategory(name, color, parent_id);

cleanup_mkcat:
    formFreeUI(&form_ui);
    return cat;
}

/* Build and return the activity by prompting the user with a form. 
 * If an original activity is provided, it is used as default values for 
 * the form fields. */
Activity *makeActivityFromUser(Activity *orig_act, const char *form_title) {
    Activity *act = NULL;
    Category *cat = NULL;

    char title[TITLE_SIZE]            = {0};
    char start_str[START_STR_SIZE]    = {0};
    char end_str[END_STR_SIZE]        = {0};
    char notes[NOTES_SIZE]            = {0};
    char cat_name[CATEGORY_NAME_SIZE] = {0};

    time_t start_ts = -1;
    time_t end_ts   = -1;

    int has_activity = orig_act != NULL;

    /* Initialize the fields */
    if (has_activity) {
        strncpy(title, orig_act->title, sizeof(title)-1);
        getTimeStr(start_str, sizeof(start_str), 
                   FULL_DATETIME_FMT, orig_act->start_ts);
        getTimeStr(end_str, sizeof(end_str), 
                   FULL_DATETIME_FMT, orig_act->end_ts);
        if (orig_act->notes) 
            strncpy(notes, orig_act->notes, sizeof(notes)-1);
        if (orig_act->category && orig_act->category->name) 
            strncpy(cat_name, orig_act->category->name, sizeof(cat_name)-1);
    }

    /* Build the prompts.
     * Here we use an input_len of 1 to tell the status bar 
     * that we are intented to add the input later. 
     * (see getStatusBarLengths function) */
    int input_len = 1;
    struct status_bar sbar = {
        .screencols   = Mycal.win_width,
        .prompt       = NULL,
        .prompt_len   = 0,
        .input        = NULL,
        .input_len    = input_len,
        .status       = NULL,
        .status_len   = 0,
        .cursor_pos   = 0,
        .input_scroll = NULL,
    };

    /* We try with long, medium and short prompts for properly display 
     * on different window sizes without overflow. */
    char prompt_titles[3][256];
    char prompt_start_strings[4][256];
    char prompt_end_strings[4][256];
    char prompt_notes[3][256];

    char *sbar_fmt = " %d/%d";

    // Title prompt
    sbar.status_len = snprintf(NULL, 0, sbar_fmt, TITLE_SIZE-1, TITLE_SIZE-1);
    snprintf(prompt_titles[0], sizeof(prompt_titles[0]), 
             "Enter Activity Title: ");
    snprintf(prompt_titles[1], sizeof(prompt_titles[1]), "Enter Title: ");
    snprintf(prompt_titles[2], sizeof(prompt_titles[2]), "Title: ");

    const char *prompt_title = fitToStatusBar(&sbar, 3, 
                                             prompt_titles[0], 
                                             prompt_titles[1], 
                                             prompt_titles[2]);
    if (prompt_title == NULL) prompt_title = prompt_titles[2];

    // Start time string prompt
    sbar.status_len = snprintf(NULL, 0, sbar_fmt, 
                              START_STR_SIZE-1, START_STR_SIZE-1);
    snprintf(prompt_start_strings[0], sizeof(prompt_start_strings[0]), 
             "Enter Activity Start Time, type `man dtparse` for details: ");
    snprintf(prompt_start_strings[1], sizeof(prompt_start_strings[1]), 
             "Enter Start Time, `man dtparse` for details: ");
    snprintf(prompt_start_strings[2], sizeof(prompt_start_strings[2]), 
             "Start Time, try `man dtparse`: ");
    snprintf(prompt_start_strings[3], sizeof(prompt_start_strings[3]), 
             "Start Time: ");

    const char *prompt_start_str = fitToStatusBar(&sbar, 4, 
                                                  prompt_start_strings[0], 
                                                  prompt_start_strings[1], 
                                                  prompt_start_strings[2],
                                                  prompt_start_strings[3]);
    if (prompt_start_str == NULL) prompt_start_str = prompt_start_strings[3];

    // End time string prompt
    sbar.status_len = snprintf(NULL, 0, sbar_fmt,
                               END_STR_SIZE-1, END_STR_SIZE-1);

    snprintf(prompt_end_strings[0], sizeof(prompt_end_strings[0]), 
             "Enter Activity End Time, type `man dtparse` for details: ");
    snprintf(prompt_end_strings[1], sizeof(prompt_end_strings[1]), 
             "Enter End Time, `man dtparse` for details: ");
    snprintf(prompt_end_strings[2], sizeof(prompt_end_strings[2]), 
                        "End Time, try `man dtparse`: ");
    snprintf(prompt_end_strings[3], sizeof(prompt_end_strings[3]), 
                        "End Time: ");

    const char *prompt_end_str = fitToStatusBar(&sbar, 4, 
                                                prompt_end_strings[0], 
                                                prompt_end_strings[1], 
                                                prompt_end_strings[2],
                                                prompt_end_strings[3]);
    if (prompt_end_str == NULL) prompt_end_str = prompt_end_strings[3];

    // Notes prompt
    sbar.status_len = snprintf(NULL, 0, sbar_fmt, NOTES_SIZE-1, NOTES_SIZE-1);

    snprintf(prompt_notes[0], sizeof(prompt_notes[0]), "Enter Activity Notes: ");
    snprintf(prompt_notes[1], sizeof(prompt_notes[1]), "Enter Notes: ");
    snprintf(prompt_notes[2], sizeof(prompt_notes[2]), "Notes: ");

    const char *prompt_note = fitToStatusBar(&sbar, 3, 
                                             prompt_notes[0], 
                                             prompt_notes[1], 
                                             prompt_notes[2]);
    if (prompt_note == NULL) prompt_note = prompt_notes[2];


    /* Build the form */
    Form form;
    FormUI form_ui;
    FormField fields[] = {
        { " ( ) Title",      prompt_title,     title,     sizeof(title),    0 },
        { " ( ) Start Time", prompt_start_str, start_str, sizeof(start_str),
            0 },
        { " ( ) End Time",   prompt_end_str,   end_str,   sizeof(end_str),  0 },
        { " ( ) Notes",      prompt_note,      notes,     sizeof(notes),    0 },
        { " ( ) Category",   NULL,             cat_name,  sizeof(cat_name), 0 },
    };

    int field_count = sizeof(fields)/sizeof(fields[0]);

    formInit(&form, form_title, fields, field_count);
    formInitUI(&form_ui, 0, 0);

    char input[512] = {0};
    char prev_input[512] = {0};

    int step = 0; // track the current level in the form
    int max_step = 0;
    while (1) {
        if (step == 5) break;
        if (step > max_step) max_step = step;

        /* Render form */
        clearView(&form_ui.form_view);
        clearView(&form_ui.log_view);

        formBuildFields(&form, &form_ui);
        formBuildLogs(&form, &form_ui, input);
        selectField(&form_ui, &form.fields[step]);
        formRender(&form_ui); 
        int field_input_len = strlen(form.fields[step].buffer);
        form_ui.sbar.cursor_pos = field_input_len;
        formRenderStatusBar(&form_ui.sbar, form_ui.prompt_style, 
                            &form.fields[step],
                            form.fields[step].buffer, field_input_len);

        int last_field_step = 3;
        if (step <= last_field_step) {
            strncpy(prev_input, input, sizeof(prev_input)-1);
            prev_input[sizeof(prev_input)-1] = '\0';

            // Copy the original input to start typing from it.
            strncpy(input, fields[step].buffer, fields[step].bufsize-1);
            input[fields[step].bufsize-1] = '\0';

mkact_cnt_input:
            int c = processField(&form_ui, &form.fields[step],
                                 input, sizeof(input));
            if (!c) {
                cat = NULL;
                goto cleanup_mkact;
            }

            if (c == KEY_ARROW_UP) {
                if (step > 0) {
                    formClearLogs(&form);
                    step--;
                    continue;
                }
                goto mkact_cnt_input;
            } else if (c == KEY_ARROW_DOWN) {
                if (step <= last_field_step && 
                    (step < max_step || has_activity)) {
                    formClearLogs(&form);
                    /* always check the step conditions when skipping a 
                     * field down */
                } else {
                    goto mkact_cnt_input;
                }
            }

            char *r;
            if ((r = strchr(input, '\n'))) *r = '\0'; // remove new line
            input_len = strlen(input);
        }

        /* Parse the input.
         * Write successful inputs into form fields. */

        /* Title */
        if (step == 0) {
            formClearLogs(&form);

            // Empty title
            if (input_len == 0) {
                addLog(&form.logbuf, DTPARSE_LOG_ERROR, 
                        "ERROR: Title cannot be empty.");
            }
            // New title
            else {
                strncpy(title, input, sizeof(title)-1);
                title[sizeof(title)-1] = '\0';
                step++;
            }
        } 
        /* Start time */
        else if (step == 1) {
            // Show dtparse man page
            if (strcmp(input, "man dtparse") == 0) {
                strncpy(input, prev_input, sizeof(input));
                showDtparseManPage();
            } 
            // Empty start time
            else if (input_len == 0) {
                start_ts = -1;
                formClearLogs(&form);

                addLog(&form.logbuf, DTPARSE_LOG_ERROR, 
                       "ERROR: Start time cannot be empty.");
            }
            // New start time
            else {
                formClearLogs(&form);
                start_ts = dtparse_parse(input, getDtparseLog, &form.logbuf); 
                if (start_ts != (time_t)-1) {
                    getTimeStr(start_str, sizeof(start_str),
                               FULL_DATETIME_FMT, start_ts);
                    step++;
                }
            }
        } 
        /* End time */
        else if (step == 2) {
            // Show dtparse man page
            if (strcmp(input, "man dtparse") == 0) {
                strncpy(input, prev_input, sizeof(input));
                showDtparseManPage();
            }
            // Empty end time
            else if (input_len == 0) {
                end_ts = -1;
                formClearLogs(&form);

                addLog(&form.logbuf, DTPARSE_LOG_ERROR, 
                        "ERROR: End time cannot be empty.");
            }
            // New end time
            else {
                formClearLogs(&form);
                end_ts = dtparse_parse(input, getDtparseLog, &form.logbuf); 
            }

            if (end_ts != (time_t)-1) {
                if (end_ts <= start_ts) {
                    getTimeStr(input, sizeof(input),
                               FULL_DATETIME_FMT, end_ts);
                    addLog(&form.logbuf, DTPARSE_LOG_ERROR, 
                            "ERROR: End time must be after start time.");
                    end_ts = -1;
                } else {
                    if (!(input_len == 0 && has_activity)) {
                        getTimeStr(end_str, sizeof(end_str),
                                   FULL_DATETIME_FMT, end_ts);
                    }

                    step++;
                }
            }
        }
        /* Notes */
        else if (step == 3) {
            formClearLogs(&form);

            strncpy(notes, input, sizeof(notes)-1);
            notes[sizeof(notes)-1] = '\0';

            step++;
        }
        /* Category */
        else if (step == 4) {
            View cat_view;
            initCategoryView(&cat_view);

            // Add NONE and NEW as categories
            int y;
            y = cat_view.as.cat.header_rows;
            Category *none = makeCategory("NONE", UNCAT_COLOR, 0);
            none->id = CATEGORY_ID_NONE;
            struct bounding_box none_bb = {
                .x      = 0,
                .y      = y,
                .width  = strlen(none->name),
                .height = 1, 
            };
            viewAddCategory(&cat_view, 0, none, &none_bb, 1, 0);

            y = cat_view.as.cat.header_rows + 1;
            Category *new = makeCategory("NEW", COLOR_DEFAULT, 0);
            new->id = CATEGORY_ID_NEW;
            struct bounding_box new_bb = {
                .x      = 0,
                .y      = y,
                .width  = strlen(new->name),
                .height = 1, 
            };
            viewAddCategory(&cat_view, 1, new, &new_bb, 1, 0);

            populateCategoryViewRows(&cat_view);

            int rend_rows = Mycal.win_height - form_ui.form_renderer.y 
                - form_ui.form_renderer.screenrows - 1; // -1 for status bar
            Renderer r = {
                .x = form_ui.form_renderer.x,
                .y = form_ui.form_renderer.y 
                     + form_ui.form_renderer.screenrows,
                .rowoff     = 0,
                .coloff     = 0,
                .screenrows = rend_rows,
                .screencols = Mycal.win_width,
                .vert_scroll = 1,
                .horz_scroll = 1,
                .text_align = RENDERER_ALIGN_START,
            };

            // Make the selection points to the original category if any.
            int select_idx = 0;
            if (has_activity && orig_act->category) {
                for (size_t i = 0; i < cat_view.entity_count; i++) {
                    if (orig_act->category->id == 
                        ((Category *)cat_view.entities[i])->id) {
                        select_idx = i;
                        break;
                    }
                }
            }

            Category *selected = selectEntityFromUser(&cat_view, &r,
                                                      select_idx, 
                                                      "Select the category.");

            if (selected == NULL && has_activity && orig_act->category) {
                // Use same category as the orig activity.
                // Clone category to prevent use-after-free.
                if (cat) freeCategory(cat);
                Category *clone = makeCategory(orig_act->category->name,
                                               orig_act->category->color,
                                               orig_act->category->parent_id);
                clone->id = orig_act->category->id;
                cat = clone;

                strncpy(cat_name, cat->name, sizeof(cat_name)-1);
                cat_name[sizeof(cat_name)-1] = '\0';
            } else if (selected) {
                if (cat) freeCategory(cat);
                // NONE category
                if (selected->id == CATEGORY_ID_NONE) {
                    cat = NULL;
                    cat_name[0] = '\0';
                } 
                // NEW category
                else if (selected->id == CATEGORY_ID_NEW) {
                    cat = makeCategoryFromUser(NULL, "Add a new category");
                    if (cat) {
                        if (insertCategory(Mycal.db, cat)) {
                            cat->id = sqlite3_last_insert_rowid(Mycal.db);
                        }
                        strncpy(cat_name, cat->name, sizeof(cat_name)-1);
                        cat_name[sizeof(cat_name)-1] = '\0';
                    }
                } 
                // Chosen category
                else {
                    // Clone category to prevent use-after-free.
                    Category *clone = makeCategory(selected->name, 
                                                   selected->color, 
                                                   selected->parent_id);
                    clone->id = selected->id;
                    cat = clone;

                    strncpy(cat_name, cat->name, sizeof(cat_name)-1);
                    cat_name[sizeof(cat_name)-1] = '\0';
                }
            }

            freeView(&cat_view);
            step++;
        }
    }

    // Render last time for update the last form field.
    formRebuildAndRender(&form, &form_ui, input); 

    act = makeActivity(title, start_ts, end_ts, notes, cat);

cleanup_mkact:
    formFreeUI(&form_ui);
    return act;
}

/* Prompt the user with the 'prompt' message and wait for a y/n response.
 * Return 1 if the user answers yes, return 0 if he answers no, continue
 * asking otherwise. */
int askUserConfirm(const char *prompt) {
    int res = 0;
    int prompt_len = strlen(prompt);

    char buf[2] = {0};
    int buflen  = 0;

    struct status_bar sbar = {
        .screencols   = Mycal.win_width,
        .prompt       = prompt,
        .prompt_len   = prompt_len,
        .input        = buf,
        .input_len    = buflen,
        .status       = NULL,
        .status_len   = 0,
        .cursor_pos   = 0,
        .input_scroll = NULL,
    };

    const char *help = "Invalid input. Please type 'y' for yes or 'n' for no: ";
    int help_len = strlen(help);

    int chars_left = sizeof(buf)-1;
    int cursor_pos = buflen;
    int input_scroll = 0;
    sbar.input_scroll = &input_scroll;

    char status[32];
    sbar.status_len = snprintf(status, sizeof(status), " %d/%zu", 
                               chars_left, sizeof(buf)-1);
    sbar.status = status;

    TextStyle status_style = TEXT_STYLE_INV;
    printf("\x1b[G"); // set cursor at starting screen column
    sbar.cursor_pos = buflen;
    int cursor_col = renderStatusBar(&sbar, status_style);
    printf("\x1b[%dG", cursor_col); // set cursor at end of input
    fflush(stdout);

    setTextStyle(status_style);
    enableRawMode();

    while (1) {
ask_start:
        int c = readKey();
        switch (c) {
        case CTRL_C:
        case CTRL_Q:
            res = 0;
            goto ask_end;
        case ENTER:
        case '\n':
            if (buf[0] == 'y' || buf[0] == 'Y') {
                res = 1;
                goto ask_end;
            } else if (buf[0] == 'n' || buf[0] == 'N') {
                res = 0;
                goto ask_end;
            } else {
                buflen = 0;
                buf[0] = '\0';
                sbar.prompt     = help;
                sbar.prompt_len = help_len;
                sbar.input_len  = buflen;

                cursor_pos = 0;
                printf("\x1b[G"); // set cursor at starting screen column

                chars_left = sizeof(buf)-1 - buflen;
                sbar.status_len = snprintf(status, sizeof(status), " %d/%zu", 
                                           chars_left, sizeof(buf)-1);
                sbar.cursor_pos = buflen;
                sbar.input_scroll = NULL;
                cursor_col = renderStatusBar(&sbar, status_style);
                printf("\x1b[%dG", cursor_col); // set cursor at end of input
                fflush(stdout);

                goto ask_start;
            }
            break;
        case KEY_ARROW_LEFT:
            if (cursor_pos > 0) cursor_pos--;
            break;
        case KEY_ARROW_RIGHT:
            if (cursor_pos < buflen) cursor_pos++;
            break;
        case BACKSPACE:
        case CTRL_H:
            if (cursor_pos > 0) {
                memmove(&buf[cursor_pos-1], 
                        &buf[cursor_pos], 
                        (buflen-cursor_pos) * sizeof(char));
                cursor_pos--;
                buflen--;
                buf[buflen] = '\0';
            }
            break;
        default:
            if (c >= 32 && c <= 126) {
                if ((size_t)buflen < sizeof(buf)-1) {
                    memmove(&buf[cursor_pos+1], 
                            &buf[cursor_pos], 
                            (buflen-cursor_pos) * sizeof(char));
                    buf[cursor_pos] = (char)c;
                    cursor_pos++;
                    buflen++;
                    buf[buflen] = '\0';
                }
            }
            break;
        }

        /* Render status bar */
        sbar.prompt     = prompt;
        sbar.prompt_len = prompt_len;
        sbar.input_len  = buflen;

        chars_left = sizeof(buf)-1 - buflen;
        sbar.status_len = snprintf(status, sizeof(status), " %d/%zu", 
                                   chars_left, sizeof(buf)-1);

        printf("\x1b[G"); // set cursor at starting screen column
        sbar.cursor_pos = cursor_pos;
        sbar.input_scroll = &input_scroll;
        cursor_col = renderStatusBar(&sbar, status_style);
        printf("\x1b[%dG", cursor_col); // set cursor at end of input
        fflush(stdout);
    }

ask_end:
    disableRawMode();
    setTextStyle(TEXT_STYLE_DEFAULT);
    return res;
}

/* ================================== Help ================================== */

/* Print the usage of the program. */
void showHelp(const char *progname) {
    printf("Usage: %s [FILE.db] [OPTIONS...]\n"
           "\n"
           "   --add-activity            Add a new activity\n"
           "   --del-activity            Delete an existing activity\n"
           "   --edit-activity           Edit an existing activity\n"
           "\n"
           "   --add-category            Add a new category\n"
           "   --del-category            Delete an existing category\n"
           "   --edit-category           Edit an existing category\n"
           "\n"
           "   -d[DT], --show-day[=DT]   Show daily view   (default: today)\n"
           "   -w[DT], --show-week[=DT]  Show weekly view  (default: current week)\n"
           "   -m[DT], --show-month[=DT] Show monthly view (default: current month)\n"
           "   -y[DT], --show-year[=DT]  Show yearly view  (default: current year)\n"
           "\n"
           "   -a[DT] [NUM], --show-acts[=DT] [NUM]\n"
           "       Show last NUM activities before DT (default: 10)\n"
           "   -c, --show-cats           Show categories\n"
           "\n"
           "   --help-datetime           Show manual page for datetime string formatting\n"
           "   -h, --help                Just show this help\n"
           "\n"
           "DT (datetime string) is used to indicate time values and should be quoted (e.g. `-d\"2026-5-6\"` or `--show-day=\"2026-5-6\"). For more information about datetime string formatting try `%s --help-datetime`.\n",
           progname, progname);
}

/* ================================== Main ================================== */

/* Represent a log buffer with CLI argument referred to it. */
struct cli_log_buf {
    struct log_buf *logbuf;
    const char *arg;
    int argi;
};

/* Save the error messages from dtparse to the CLI log buffer ('ctx'). */
void storeDtparseCLIErrors(Dtparse_LogLevel level, int line, int col, 
                           const char *msg, void *ctx) 
{
    if (level != DTPARSE_LOG_ERROR) return;

    struct cli_log_buf *cli_log = (struct cli_log_buf *)ctx;

    char buf[256];
    int pos = 0;

    pos += snprintf(buf+pos, sizeof(buf)-pos, 
                    "At argument %d `%s`, from dtparse: ", 
                    cli_log->argi+1, cli_log->arg);

    if (line != -1 && col != -1) {
        pos += snprintf(buf+pos, sizeof(buf) - pos, "[%d:%d] ", line, col);
    }

    pos += snprintf(buf+pos, sizeof(buf) - pos, "ERROR %s", msg);

    addLog(cli_log->logbuf, level, buf);
}

/* Return the rest of the string if it starts with the given 'prefix', 
 * return NULL otherwise. */
const char *stripPrefix(const char *str, const char *prefix) {
    int prefix_len = strlen(prefix);
    if (strncmp(str, prefix, prefix_len) == 0) {
        return str+prefix_len;
    }

    return NULL;
}

/* Initialize the 'view' with the first view type argument found, if any.
 * Initialize as day view otherwise. 
 * Return the number of argument parsed. */
int viewInitFirstArg(View *view, char **argv, int argc) {
    int is_init = 0;
    int i;
    for (i = 0; i < argc; i++) {
        int more_args = (i+1) < argc;

        const char *dt_str;
        time_t ts = Mycal.now;
        // Day view (short)
        if ((dt_str = stripPrefix(argv[i], "-d"))) {
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initDayView(view, ts);
            populateDayViewRows(view);
            is_init = 1;
            break;
        }
        // Day view (long)
        else if ((dt_str = stripPrefix(argv[i], "--show-day"))) {
            if (dt_str[0] == '=') dt_str++;
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initDayView(view, ts);
            populateDayViewRows(view);
            is_init = 1;
            break;
        }
        // Week view (short)
        else if ((dt_str = stripPrefix(argv[i], "-w"))) {
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initWeekView(view, ts);
            populateWeekViewRows(view);
            is_init = 1;
            break;
        }
        // Week view (long)
        else if ((dt_str = stripPrefix(argv[i], "--show-week"))) {
            if (dt_str[0] == '=') dt_str++;
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initWeekView(view, ts);
            populateWeekViewRows(view);
            is_init = 1;
            break;
        } 
        // Month view (short)
        else if ((dt_str = stripPrefix(argv[i], "-m"))) {
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initMonthView(view, ts);
            populateMonthViewRows(view);
            is_init = 1;
            break;
        }
        // Month view (long)
        else if ((dt_str = stripPrefix(argv[i], "--show-month"))) {
            if (dt_str[0] == '=') dt_str++;
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initMonthView(view, ts);
            populateMonthViewRows(view);
            is_init = 1;
            break;
        } 
        // Year view (short)
        else if ((dt_str = stripPrefix(argv[i], "-y"))) {
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initYearView(view, ts);
            populateYearViewRows(view);
            is_init = 1;
            break;
        }
        // Year view (long)
        else if ((dt_str = stripPrefix(argv[i], "--show-year"))) {
            if (dt_str[0] == '=') dt_str++;
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            initYearView(view, ts);
            populateYearViewRows(view);
            is_init = 1;
            break;
        } 
        // Activity view (short)
        else if ((dt_str = stripPrefix(argv[i], "-a"))) {
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            // Parse optional NUM argument
            int act_count = DEFAULT_LAST_ACTIVITY_COUNT;
            if (more_args && stripPrefix(argv[i+1], "-") == NULL) {
                i++;
                act_count = atoi(argv[i]);
                if (act_count <= 0) act_count = DEFAULT_LAST_ACTIVITY_COUNT;
            }

            initActivityView(view, ts, act_count);
            populateActivityViewRows(view);
            is_init = 1;
            break;
        }
        // Activity view (long)
        else if ((dt_str = stripPrefix(argv[i], "--show-acts"))) {
            if (dt_str[0] == '=') dt_str++;
            if (strlen(dt_str) > 0) ts = dtparse_parse(dt_str, NULL, NULL); 

            // Parse optional NUM argument
            int act_count = DEFAULT_LAST_ACTIVITY_COUNT;
            if (more_args && stripPrefix(argv[i+1], "-") == NULL) {
                i++;
                act_count = atoi(argv[i]);
                if (act_count <= 0) act_count = DEFAULT_LAST_ACTIVITY_COUNT;
            }

            initActivityView(view, ts, act_count);
            populateActivityViewRows(view);
            is_init = 1;
            break;
        } 
    }

    // Day view as default
    if (!is_init) {
        initDayView(view, Mycal.now);
        populateDayViewRows(view);
    }

    return i;
}

int main(int argc, char *argv[]) {
    srand(time(NULL)); // for getUnusedColor()

    int argi = 0;
    Mycal.progname = argv[argi++];
    Mycal.now = time(NULL);
    if (Mycal.now == -1) die("time");

    /* Get terminal window sizes */
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) die("ioctl");
    Mycal.win_width  = ws.ws_col;
    Mycal.win_height = ws.ws_row;

    /* Load database */
    char *db_file = "mycal.db";
    if (argi < argc) {
        int file_len = strlen(argv[argi]);
        if (file_len >= 3 && strcmp(argv[argi]+file_len-3, ".db") == 0)
            db_file = argv[argi++]; // endswith ".db"
    }
    
    if (!initDB(db_file, &Mycal.db)) {
        sqlite3_close(Mycal.db);
        return 1;
    }

    /* Show day view as default */
    if (argi == argc) { 
        View view;
        initDayView(&view, Mycal.now);
        populateDayViewRows(&view);

        Renderer r = {
            .x = -1,
            .y = -1,
            .rowoff     = 0,
            .coloff     = 0,
            .screenrows = view.nrows,
            .screencols = Mycal.win_width,
            .text_align = RENDERER_ALIGN_START,
        };

        viewRenderRows(&view, &r);
        freeView(&view);
    }

    /* Parse args.
     *
     * For options like `--del-activity`, `--edit-activity`, we can provide 
     * a specific activity view to work with (e.g. -d -w -m -y).
     * In case of multiple of this options or multiple views we only use 
     * the FIRST one.
     * Instead, if we are simply showing the view on the screen we print 
     * each of them in order.
     * If the program is invoked with no additional arguments, the default 
     * behavior is to show the day view. 
     *
     * Alongside all the --show-(view) options, we can provide an optional 
     * datetime string to indicate a specific date to use for the view.
     * This datetime string should always be quoted.
     * For short forms (such as -d -w -m -y), we append the datetime string
     * after the option (-d"2026-5-6", no space between -d and datetime
     * string).
     * For long forms (--show-(view)), we add datetime string appended to
     * the option with an '=' in between (--show-day="2026-5-6"). 
     *
     * For these short form options that are some subtle decisions to discuss.
     * First note that when we pass quoted arguments, the shell REMOVE the 
     * quotes and treat the enclosed text as a single contiguous string. 
     * So:
     * - we cannot let the short option to be separated by its datetime
     *   string, because otherwise we would not be able to distinguish when 
     *   the next argument is a datetime string or program command 
     *   (e.g. -d "-w" or -d -w).
     * - we can't provide more than one short option in the same argument,
     *   because again we would not be able to distinguish whether the next 
     *   argument is a datetime string or a program command 
     *   (e.g. -d"wed" or -dw"ed"). 
     * For these reasons we can only provide a single short argument with 
     * its optional datetime string at a time. */
    int op_once = 0;   // 1 if there is an operation that runs and quits
    int need_view = 0; // 1 if the operation needs the view

    int show_help = 0;               // 1 to show the program help
    int show_help_datetime = 0;      // 1 to show the datetime help
    struct log_buf dtstr_errs = {0}; // log buffer for the datetime string
    int dtstr_err = 0;               // 1 if the datetime string has an error
    int invalid_arg = -1;            // index of the invalid argument in 'argv'
                                     // array if any, -1 otherwise
    const char *dt_str;
    for (int i = argi; i < argc; i++) {
        int more_args = (i+1) < argc;

        if ((strcmp(argv[i], "--add-activity") == 0) ||
            (strcmp(argv[i], "--add-category") == 0) ||
            (strcmp(argv[i], "--del-category") == 0) ||
            (strcmp(argv[i], "--edit-category") == 0)) {
            op_once = 1;
        } else if ((strcmp(argv[i], "--del-activity") == 0) ||
                   (strcmp(argv[i], "--edit-activity") == 0)) {
            // There is not another operation before.
            if (!op_once) {
                op_once   = 1;
                need_view = 1;
            }
        } else if ((dt_str = stripPrefix(argv[i], "-d")) || 
                   (dt_str = stripPrefix(argv[i], "-w")) ||
                   (dt_str = stripPrefix(argv[i], "-m")) ||
                   (dt_str = stripPrefix(argv[i], "-y")) ||
                   (dt_str = stripPrefix(argv[i], "-a"))) {
            if (strlen(dt_str) > 0) {
                struct cli_log_buf cli_log = {
                    .logbuf = &dtstr_errs,
                    .arg    = dt_str,
                    .argi   = i,
                };
                if (dtparse_parse(dt_str, 
                                  storeDtparseCLIErrors, 
                                  &cli_log) == -1)
                    dtstr_err = 1;
            }

            // Skip optional NUM argument after `-a` option
            if (stripPrefix(argv[i], "-a") && 
                more_args && stripPrefix(argv[i+1], "-") == NULL) {
                i++;
            }
        } else if (((dt_str = stripPrefix(argv[i], "--show-day"))   ||
                    (dt_str = stripPrefix(argv[i], "--show-week"))  ||
                    (dt_str = stripPrefix(argv[i], "--show-month")) ||
                    (dt_str = stripPrefix(argv[i], "--show-year"))  ||
                    (dt_str = stripPrefix(argv[i], "--show-acts"))) &&
                   (dt_str[0] == '=' || dt_str[0] == '\0')) {
            if (dt_str[0] == '=') {
                dt_str++; // skip the '='
                if (strlen(dt_str) > 0) {
                    struct cli_log_buf cli_log = {
                        .logbuf = &dtstr_errs,
                        .arg    = dt_str,
                        .argi   = i,
                    };
                    if (dtparse_parse(dt_str, 
                                      storeDtparseCLIErrors, 
                                      &cli_log) == -1)
                        dtstr_err = 1;
                }
            }

            // Skip optional NUM argument after `-show-acts` option
            if (stripPrefix(argv[i], "--show-acts") && 
                more_args && stripPrefix(argv[i+1], "-") == NULL) {
                i++;
            }
        } else if ((strcmp(argv[i], "-c") == 0) || 
                   (strcmp(argv[i], "--show-categories") == 0)) {
            ; // nothing
        }
        /* Show datetime man page */
        else if (strcmp(argv[i], "--help-datetime") == 0) {
            show_help_datetime = 1;
        }
        /* Show help */
        else if ((strcmp(argv[i], "-h") == 0) || 
                 (strcmp(argv[i], "--help") == 0)) {
            show_help = 1;
        } 
        /* Invalid option */
        else {
            invalid_arg = i;
        }
    }
    
    /* Show help and quit */
    if (show_help) {
        showHelp(argv[0]);
        sqlite3_close(Mycal.db);
        exit(0);
    } 

    /* Show datetime man page and quit */
    if (show_help_datetime) {
        dtparse_showManPage();
        sqlite3_close(Mycal.db);
        exit(0);
    }

    /* Show datetime string error and quit */
    if (dtstr_err) {
        for (int l = 0; l < dtstr_errs.count; l++) {
            printf("%s\n", dtstr_errs.msgs[l]);
            //break; // show only the first error
        }
        sqlite3_close(Mycal.db);
        exit(1);
    }

    /* Show invalid option and quit */
    if (invalid_arg != -1) {
        fprintf(stderr, "%s: unrecognized option `%s`\n",
                argv[0], argv[invalid_arg]);
        fprintf(stderr, "Try `%s --help` for more information.\n",
                argv[0]);
        sqlite3_close(Mycal.db);
        exit(1);
    }

    /* Run */
    View view;
    view.type = VIEW_NONE;
    for (int i = argi; i < argc; i++) {
        /* Add activity */
        if (strcmp(argv[i], "--add-activity") == 0) {
            printf("\x1b[2J"); // clear the screen
            Activity *act = makeActivityFromUser(NULL, "Add a new activity");

            if (act) {
                struct status_bar sbar = {
                    .screencols   = Mycal.win_width,
                    .prompt       = NULL,
                    .prompt_len   = 0,
                    .input        = NULL,
                    .input_len    = 1,
                    .status       = NULL,
                    .status_len   = 0,
                    .cursor_pos   = 0,
                    .input_scroll = NULL,
                };
                const char *ask_msgs[] = {
                    "Do you want to add this activity? [y/n] ",
                    "Want to add this activity? [y/n] ",
                    "Add activity? [y/n] ",
                };
                const char *ask_msg = fitToStatusBar(&sbar, 3,
                        ask_msgs[0],
                        ask_msgs[1],
                        ask_msgs[2]);
                if (ask_msg == NULL) ask_msg = ask_msgs[2];
                    
                if (askUserConfirm(ask_msg)) {
                    printf("\n");
                    if (insertActivity(Mycal.db, act)) {
                        printf("Activity added");
                    } else {
                        printf("Failed to add activity. Please try again");
                    }
                }

                freeActivity(act);
            }

            printf("\n");
            break;
        }
        /* Delete activity */
        else if (strcmp(argv[i], "--del-activity") == 0) {
            if (view.type == VIEW_NONE) {
                viewInitFirstArg(&view, &argv[i+1], argc - (i+1));
            }

            if (view.entity_count == 0) {
                if (view.as.act.act_type == ACTIVITY_VIEW_YEAR) {
                    printf("Yearly view doesn't show activites. Try with "
                           "another view\n");
                } else {
                    printf("There are no activities to select in the current "
                           "%s view\n", 
                           getActivityViewString(view.as.act.act_type));
                }
                freeView(&view);
                break;
            }

            Renderer r = {
                .x = 0,
                .y = 0,
                .rowoff = 0,
                .coloff = 0,
                .screenrows = Mycal.win_height-1,
                .screencols = Mycal.win_width,
                .vert_scroll = 1,
                .horz_scroll = 0,
                .text_align = RENDERER_ALIGN_START,
            };

            printf("\x1b[2J"); // clear the screen
            Activity *act = selectEntityFromUser(&view, &r, 0,
                    "Select the activity.");

            if (act) {
                renderActivity(act);

                struct status_bar sbar = {
                    .screencols   = Mycal.win_width,
                    .prompt       = NULL,
                    .prompt_len   = 0,
                    .input        = NULL,
                    .input_len    = 1,
                    .status       = NULL,
                    .status_len   = 0,
                    .cursor_pos   = 0,
                    .input_scroll = NULL,
                };
                const char *ask_msgs[] = {
                    "Do you want to delete this activity? [y/n] ",
                    "Want to delete this activity? [y/n] ",
                    "Delete activity? [y/n] ",
                };
                const char *ask_msg = fitToStatusBar(&sbar, 3,
                        ask_msgs[0],
                        ask_msgs[1],
                        ask_msgs[2]);
                if (ask_msg == NULL) ask_msg = ask_msgs[2];

                if (askUserConfirm(ask_msg)) {
                    printf("\n");
                    if (deleteActivity(Mycal.db, act->id)) {
                        printf("Activity deleted");
                    } else {
                        printf("Failed to delete activity. Please try again");
                    }
                }
            }

            printf("\n");
            freeView(&view);
            break;
        }
        /* Edit activity */
        else if (strcmp(argv[i], "--edit-activity") == 0) {
            if (view.type == VIEW_NONE) {
                viewInitFirstArg(&view, &argv[i+1], argc - (i+1));
            }

            if (view.entity_count == 0) {
                if (view.as.act.act_type == ACTIVITY_VIEW_YEAR) {
                    printf("Yearly view doesn't show activites. Try with "
                           "another view\n");
                } else {
                    printf("There are no activities to select in the current "
                           "%s view\n", 
                           getActivityViewString(view.as.act.act_type));
                }
                freeView(&view);
                break;
            }

            Renderer r = {
                .x = 0,
                .y = 0,
                .rowoff = 0,
                .coloff = 0,
                .screenrows = Mycal.win_height-1,
                .screencols = Mycal.win_width,
                .vert_scroll = 1,
                .horz_scroll = 0,
                .text_align = RENDERER_ALIGN_START,
            };

            printf("\x1b[2J"); // clear the screen
            Activity *act = selectEntityFromUser(&view, &r, 0,
                    "Select the activity.");

            if (act) {
                Activity *new = makeActivityFromUser(act, "Edit the activity");

                if (new) {
                    struct status_bar sbar = {
                        .screencols   = Mycal.win_width,
                        .prompt       = NULL,
                        .prompt_len   = 0,
                        .input        = NULL,
                        .input_len    = 1,
                        .status       = NULL,
                        .status_len   = 0,
                        .cursor_pos   = 0,
                        .input_scroll = NULL,
                    };
                    const char *ask_msgs[] = {
                        "Do you want to save the changes for this activity? [y/n] ",
                        "Save changes for this activity? [y/n] ",
                        "Save changes? [y/n] ",
                    };
                    const char *ask_msg = fitToStatusBar(&sbar, 3,
                            ask_msgs[0],
                            ask_msgs[1],
                            ask_msgs[2]);
                    if (ask_msg == NULL) ask_msg = ask_msgs[2];

                    if (askUserConfirm(ask_msg)) {
                        printf("\n");
                        if (editActivity(Mycal.db, act->id, new)) {
                            printf("Activity updated");
                        } else {
                            printf("Failed to update activity. Please try again");
                        }
                    }

                    freeActivity(new); 
                }
            }

            printf("\n");
            freeView(&view);
            break;
        }
        /* Add category */
        else if (strcmp(argv[i], "--add-category") == 0) {
            printf("\x1b[2J"); // clear the screen
            Category *cat = makeCategoryFromUser(NULL, "Add a new category");

            if (cat) {
                struct status_bar sbar = {
                    .screencols   = Mycal.win_width,
                    .prompt       = NULL,
                    .prompt_len   = 0,
                    .input        = NULL,
                    .input_len    = 1,
                    .status       = NULL,
                    .status_len   = 0,
                    .cursor_pos   = 0,
                    .input_scroll = NULL,
                };
                const char *ask_msgs[] = {
                    "Do you want to add this category? [y/n] ",
                    "Want to add this category? [y/n] ",
                    "Add category? [y/n] ",
                };
                const char *ask_msg = fitToStatusBar(&sbar, 3,
                        ask_msgs[0],
                        ask_msgs[1],
                        ask_msgs[2]);
                if (ask_msg == NULL) ask_msg = ask_msgs[2];

                if (askUserConfirm(ask_msg)) {
                    printf("\n");
                    if (insertCategory(Mycal.db, cat)) {
                        printf("Category added");
                    } else {
                        printf("Failed to add category. Please try again");
                    }
                }

                freeCategory(cat);
            }

            printf("\n");
            break;
        }
        /* Delete category */
        else if (strcmp(argv[i], "--del-category") == 0) {
            initCategoryView(&view);
            populateCategoryViewRows(&view);

            if (view.entity_count == 0) {
                printf("There are no categories to select\n");
                freeView(&view);
                break;
            }

            Renderer r = {
                .x = 0,
                .y = 0,
                .rowoff = 0,
                .coloff = 0,
                .screenrows = Mycal.win_height-1,
                .screencols = Mycal.win_width,
                .vert_scroll = 1,
                .horz_scroll = 1,
                .text_align = RENDERER_ALIGN_START,
            };

            printf("\x1b[2J"); // clear the screen
            Category *cat = selectEntityFromUser(&view, &r, 0,
                    "Select the category.");

            if (cat) {
                renderCategory(cat);

                struct status_bar sbar = {
                    .screencols   = Mycal.win_width,
                    .prompt       = NULL,
                    .prompt_len   = 0,
                    .input        = NULL,
                    .input_len    = 1,
                    .status       = NULL,
                    .status_len   = 0,
                    .cursor_pos   = 0,
                    .input_scroll = NULL,
                };
                const char *ask_msgs[] = {
                    "Do you want to delete this category? [y/n] ",
                    "Want to delete this category? [y/n] ",
                    "Delete category? [y/n] ",
                };
                const char *ask_msg = fitToStatusBar(&sbar, 3,
                        ask_msgs[0],
                        ask_msgs[1],
                        ask_msgs[2]);
                if (ask_msg == NULL) ask_msg = ask_msgs[2];

                if (askUserConfirm(ask_msg)) {
                    // Check if the category has children
                    CategoryList child_list = {0};
                    daInit(&child_list);
                    getCategoriesByParentId(Mycal.db, cat->id, &child_list);

                    int cascade = 0;

                    if (child_list.count > 0) {
                        const char *ask_msgs[] = {
                            "Also want to delete all its subcategories? [y/n] ",
                            "Cascade delete its subcategories too? [y/n] ",
                            "Delete subcategories too? [y/n] ",
                        };
                        const char *ask_msg = fitToStatusBar(&sbar, 3,
                                ask_msgs[0],
                                ask_msgs[1],
                                ask_msgs[2]);
                        if (ask_msg == NULL) ask_msg = ask_msgs[2];

                        if (askUserConfirm(ask_msg)) {
                            cascade = 1;
                        }
                    }

                    daFreeEach(&child_list, freeCategory);
                    printf("\n");

                    if (deleteCategory(Mycal.db, cat->id, cascade)) {
                        if (cascade)
                            printf("Category and sub-categories deleted");
                        else
                            printf("Category deleted");
                    } else {
                        printf("Failed to delete category. Please try again");
                    }
                }
            }

            printf("\n");
            freeView(&view);
            break;
        }
        /* Edit category */
        else if (strcmp(argv[i], "--edit-category") == 0) {
            initCategoryView(&view);
            populateCategoryViewRows(&view);

            if (view.entity_count == 0) {
                printf("There are no categories to select\n");
                freeView(&view);
                break;
            }

            Renderer r = {
                .x = 0,
                .y = 0,
                .rowoff = 0,
                .coloff = 0,
                .screenrows = Mycal.win_height-1,
                .screencols = Mycal.win_width,
                .vert_scroll = 1,
                .horz_scroll = 1,
                .text_align = RENDERER_ALIGN_START,
            };

            printf("\x1b[2J"); // clear the screen
            Category *cat = selectEntityFromUser(&view, &r, 0,
                    "Select the category.");
            
            if (cat) {
                Category *new = makeCategoryFromUser(cat, "Edit the category");

                if (new) {
                    /* - if the parent of the new category is one of the
                     *   descendant of the orig category, then reparent the
                     *   orig children to the orig parent. 
                     * - if the user choose that the orig children should not
                     *   follow the new category in changing their parent, 
                     *   then reparent the orig children to the orig parent.
                     * In both cases the operations are actually committed 
                     * only if the user accepts the updates of the category. */
                    int is_descendant = 0;
                    Category *c = getCategoryById(Mycal.db, new->parent_id);
                    // Traverse the new tree from bottom to top to search
                    // the orig category.
                    while (c) {
                        if (c->id == cat->id) {
                            is_descendant = 1;
                            freeCategory(c);
                            break;
                        }
                        size_t p_id = c->parent_id;
                        freeCategory(c);
                        c = getCategoryById(Mycal.db, p_id);
                    }

                    int reparent_children = 0;

                    CategoryList child_list = {0};
                    daInit(&child_list);
                    getCategoriesByParentId(Mycal.db, cat->id, &child_list);

                    struct status_bar sbar = {
                        .screencols   = Mycal.win_width,
                        .prompt       = NULL,
                        .prompt_len   = 0,
                        .input        = NULL,
                        .input_len    = 1,
                        .status       = NULL,
                        .status_len   = 0,
                        .cursor_pos   = 0,
                        .input_scroll = NULL,
                    };

                    if (is_descendant) {
                        // Reparent the orig children to the orig parent.
                        reparent_children = 1;
                    } else {
                        // We are in another category tree.
                        // Ask to user if he wants to reparent the children.
                        if (child_list.count > 0) {
                            char ask_msgs[3][128];
                            snprintf(ask_msgs[0], sizeof(ask_msgs[0]), 
                                    "Do you want to move its %zu children too? [y/n] ",
                                    child_list.count);
                            snprintf(ask_msgs[1], sizeof(ask_msgs[1]), 
                                    "Move its %zu children too? [y/n] ",
                                    child_list.count);
                            snprintf(ask_msgs[2], sizeof(ask_msgs[2]), 
                                    "Move childrens too? [y/n] ");

                            const char *ask_msg = fitToStatusBar(&sbar, 3,
                                    ask_msgs[0],
                                    ask_msgs[1],
                                    ask_msgs[2]);
                            if (ask_msg == NULL) ask_msg = ask_msgs[2];

                            if (askUserConfirm(ask_msg)) {
                                ; // Default behavior
                            } else {
                                // Reparent orig children to orig parent
                                reparent_children = 1;
                            }
                        }
                    }

                    const char *ask_msgs[] = {
                        "Do you want to save the changes for this category? [y/n] ",
                        "Save the changes for this category? [y/n] ",
                        "Save changes? [y/n] ",
                    };
                    const char *ask_msg = fitToStatusBar(&sbar, 3,
                            ask_msgs[0],
                            ask_msgs[1],
                            ask_msgs[2]);
                    if (ask_msg == NULL) ask_msg = ask_msgs[2];

                    if (askUserConfirm(ask_msg)) {
                        printf("\n");

                        // Reparent children
                        if (reparent_children) {
                            for (size_t i = 0; i < child_list.count; i++) {
                                Category *child = child_list.items[i];
                                child->parent_id = cat->parent_id;
                                editCategory(Mycal.db, child->id, child);
                                if (!editCategory(Mycal.db, child->id, child))
                                    fprintf(stderr, "Failed to update child "
                                            "%zu category\n", i);
                            }
                        }

                        if (editCategory(Mycal.db, cat->id, new)) {
                            printf("Category updated");
                        } else {
                            printf("Failed to update category. Please try again");
                        }
                    }

                    daFreeEach(&child_list, freeCategory);
                    freeCategory(new); 
                }
            }

            printf("\n");
            freeView(&view);
            break;
        }
        /* Show categories */
        else if (strcmp(argv[i], "-c") == 0 ||
                 strcmp(argv[i], "--show-categories") == 0) {
            if (!op_once) {
                initCategoryView(&view);
                populateCategoryViewRows(&view);

                Renderer r = {
                    .x = -1,
                    .y = -1,
                    .rowoff     = 0,
                    .coloff     = 0,
                    .screenrows = view.nrows,
                    .screencols = Mycal.win_width,
                    .text_align = RENDERER_ALIGN_START,
                };

                viewRenderRows(&view, &r);
                freeView(&view);
            }
        }
        /* Show view */
        else {
            if (!op_once || (need_view && view.type == VIEW_NONE)) {
                i += viewInitFirstArg(&view, &argv[i], argc-i);
            }

            if (!op_once) {
                Renderer r = {
                    .x = -1,
                    .y = -1,
                    .rowoff     = 0,
                    .coloff     = 0,
                    .screenrows = view.nrows,
                    .screencols = Mycal.win_width,
                    .text_align = RENDERER_ALIGN_START,
                };

                viewRenderRows(&view, &r);
                freeView(&view);
            }
        }
    }

    sqlite3_close(Mycal.db);
    return 0;
}
