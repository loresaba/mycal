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


/* Do this:
 *   #define DTPARSE_IMPLEMENTATION
 *   before you include this file in *one* C or C++ file to create the
 *   implementation.
 *
 *   i.e. it should look like this:
 *   #include ...
 *   #define DTPARSE_IMPLEMENTATION
 *   #include "dtparse.h" 
 *
 * Do this:
 *   #define DTPARSE_STATIC
 *   before you include this file if you want the library's functions 
 *   to be private to the file they are compiled in (internal linkage).
 *
 *   i.e. it should look like this:
 *   #include ...
 *   #define DTPARSE_IMPLEMENTATION
 *   #define DTPARSE_STATIC
 *   #include "dtparse.h"
 */

#ifndef DTPARSE_H
#define DTPARSE_H

#ifdef DTPARSE_STATIC
#define DTPARSEDEF static
#else
#define DTPARSEDEF extern
#endif

#include <time.h>

/* =============================== Public API =============================== */

/* Represent all the diagnostic message levels. */
typedef enum {
    DTPARSE_LOG_INFO,
    DTPARSE_LOG_WARNING,
    DTPARSE_LOG_ERROR,
} Dtparse_LogLevel;

/* Callback invoked for every diagnostic message during parsing. 
 * line and col indicate the position of the token in the source code. 
 * -1 is an undefined token position.
 * msg is a null terminated string, valid only for the duration of this
 * function.
 * ctx is the user data pointer supplied to dtparse_parse() function. */
typedef void (*Dtparse_LogFn)(Dtparse_LogLevel level,
                              int line, int col,
                              const char *msg, void *ctx);

/* Parse the source datetime string according to the format specified in the
 * man page (see dtparse_showManPage() function). 
 * Return the corresponding timestamp of the parsed string on success, -1
 * otherwise. 
 * log_fn is the function invoked during the report of diagnostic messages, you
 * can use a NULL log_fn to silence the output.
 * ctx is an opaque pointer that is passed directly to your log_fn callback
 * whenever it is invoked. This allows you to safely pass custom data to your
 * logging function. Pass NULL if it is not needed. */
DTPARSEDEF time_t dtparse_parse(const char *source, 
                                Dtparse_LogFn log_fn, 
                                void *ctx);

/* Return the man page string. */
DTPARSEDEF const char *dtparse_getManPage(void);

/* Print the datetime format manual. */
DTPARSEDEF void dtparse_showManPage(void);

#endif // DTPARSE_H

/* ########## Implementation ########## */
#ifdef DTPARSE_IMPLEMENTATION

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DTPARSE_TAB_WIDTH 8
#define DTPARSE_TOKEN_BUFFER_SIZE 8
#define DTPARSE_MSG_BUF_SIZE 1024

/* Datetime constants */

#define DTPARSE_YEAR_BASE 1900

#define DTPARSE_SECOND_MIN 0
#define DTPARSE_SECOND_MAX 59

#define DTPARSE_MINUTE_MIN 0
#define DTPARSE_MINUTE_MAX 59

#define DTPARSE_12_HOUR_MIN 1
#define DTPARSE_12_HOUR_MAX 12

#define DTPARSE_24_HOUR_MIN 0
#define DTPARSE_24_HOUR_MAX 23

#define DTPARSE_DAY_MIN 1
// #define DTPARSE_DAY_MAX 31 <- relative to the month

#define DTPARSE_MONTH_MIN 0
#define DTPARSE_MONTH_MAX 11

#define DTPARSE_SECONDS_IN_MINUTE 60
#define DTPARSE_MINUTES_IN_HOUR   60
#define DTPARSE_HOURS_IN_DAY      24
#define DTPARSE_HALF_HOURS_IN_DAY 12
#define DTPARSE_DAYS_IN_WEEK       7
#define DTPARSE_MONTHS_IN_YEAR    12

#define DTPARSE_NAMED_DATE_FORMAT_STRING "%d %B %Y"

/* Floored integer division, always rounds toward -inf.
 * Differs from C's '/' only when 'a' is negative and 'b' positive. */
static inline int dtparse_fdiv(int a, int b) {
    return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

/* Floored modulo, result is always in [0, b) for b > 0.
 * Equivalent to (a) - (b) * dtparse_fdiv(a, b). */
static inline int dtparse_fmod(int a, int b) {
    return a % b + (a % b != 0 && (a < 0) != (b < 0)) * b;
}

/* ================================== Data ================================== */

/* Represent all the types of supported tokens. */
typedef enum {
    // data values
    DTPARSE_TOKEN_NUMBER,
    DTPARSE_TOKEN_STRING,
    // separator and operator
    DTPARSE_TOKEN_PLUS,
    DTPARSE_TOKEN_MINUS,
    DTPARSE_TOKEN_COLON,
    DTPARSE_TOKEN_COMMA,
    // control
    DTPARSE_TOKEN_ERROR,
    DTPARSE_TOKEN_EOF,
} Dtparse_TokenType;

/* Represent a token. */
typedef struct {
    Dtparse_TokenType type;
    const char *str; // Normal tokens own pointer into the source string.
                     // Error tokens point to a static error message literal.
    size_t len;
    int line;
    int col;
} Dtparse_Token;

/* Represent the scanner. */
typedef struct {
    const char *start;
    const char *curr;
    int line;
    int col;
} Dtparse_Scanner;

/* Represent a circular, fixed-size buffer queue of tokens. */
typedef struct {
    Dtparse_Token tokens[DTPARSE_TOKEN_BUFFER_SIZE];
    int head;
    int tail;
    int count;
} Dtparse_TokenBuffer;

/* Represent the parser. */
typedef struct {
    Dtparse_Scanner *scanner;
    Dtparse_Token curr_tok;
    Dtparse_TokenBuffer tokbuf;
    struct tm dt;
    int had_error;
    Dtparse_LogFn log_fn;
    void *log_ctx;
} Dtparse_Parser;

/* ================================= Debug ================================== */

#ifdef DTPARSE_DEBUG
/* Print token information. */
static void dtparse_printToken(Dtparse_Token *tok) {
    printf("%d:%d: ", tok->line, tok->col);
    printf("[");
    switch (tok->type) {
    case DTPARSE_TOKEN_NUMBER: printf("DTPARSE_TOKEN_NUMBER"); break;
    case DTPARSE_TOKEN_STRING: printf("DTPARSE_TOKEN_STRING"); break;
    case DTPARSE_TOKEN_PLUS:   printf("DTPARSE_TOKEN_PLUS");   break;
    case DTPARSE_TOKEN_MINUS:  printf("DTPARSE_TOKEN_MINUS");  break;
    case DTPARSE_TOKEN_COLON:  printf("DTPARSE_TOKEN_COLON");  break; 
    case DTPARSE_TOKEN_COMMA:  printf("DTPARSE_TOKEN_COMMA");  break; 
    case DTPARSE_TOKEN_ERROR:  printf("DTPARSE_TOKEN_ERROR");  break; 
    case DTPARSE_TOKEN_EOF:    printf("DTPARSE_TOKEN_EOF");    break;
    default: printf("UNKNOWN"); break;
    }
    printf("] ");

    printf("%.*s\n", (int)tok->len, tok->str);
}
#endif // DTPARSE_DEBUG

/* ================================ Scanner ================================= */

/* Initialize and return the token with type using the scanner knowledge. */
static Dtparse_Token dtparse_makeToken(Dtparse_TokenType type, 
                                       Dtparse_Scanner *scanner) {
    Dtparse_Token token;

    token.type = type;
    token.str = scanner->start;
    token.len = (size_t)(scanner->curr - scanner->start);
    token.line = scanner->line;
    token.col = scanner->col - (int)token.len;

    return token;
}

/* Initialize and return the error token with the error message. */
static Dtparse_Token dtparse_makeErrorToken(Dtparse_Scanner *scanner, 
                                            const char *msg)
{
    Dtparse_Token token;

    token.type = DTPARSE_TOKEN_ERROR;
    token.str  = msg;
    token.len  = (size_t)strlen(msg);
    token.line = scanner->line;
    token.col  = scanner->col - (int)(scanner->curr - scanner->start);

    return token;
}

/* Initialize the scanner. */
static void dtparse_initScanner(Dtparse_Scanner *scanner, const char *source) 
{
    scanner->start = source;
    scanner->curr  = source;
    scanner->line  = 1;
    scanner->col   = 1;
}

/* Skip all the whitespaces pointed by the scanner. */
static void dtparse_skipWhitespace(Dtparse_Scanner *scanner) {
    while (isspace((unsigned char)*scanner->curr)) {
        switch (*scanner->curr) {
        case ' ':  scanner->col++; break; 
        case '\f':
        case '\v': scanner->col++; break;
        case '\n': scanner->line++; scanner->col = 1; break;
        case '\r': scanner->col = 1; break;
        case '\t': 
            scanner->col += DTPARSE_TAB_WIDTH - 
                            ((scanner->col-1) % DTPARSE_TAB_WIDTH); 
            break;
        }

        scanner->curr++;
    }
}

/* Return the next token number pointed by the scanner.
 * Here a number is simply an integer (a sequence of [0-9] digits). */
static Dtparse_Token dtparse_scanNumber(Dtparse_Scanner *scanner) {
    while (isdigit((unsigned char)*scanner->curr)) {
        scanner->curr++;
        scanner->col++;
    }

    return dtparse_makeToken(DTPARSE_TOKEN_NUMBER, scanner);
}

/* Return the next token string pointed by the scanner.
 * Here a string is simply a sequence of alphabetical characters. */
static Dtparse_Token dtparse_scanString(Dtparse_Scanner *scanner) {
    while (isalpha((unsigned char)*scanner->curr)) {
        scanner->curr++;
        scanner->col++;
    }

    return dtparse_makeToken(DTPARSE_TOKEN_STRING, scanner);
}

/* Return the next token pointed by the scanner. */
static Dtparse_Token dtparse_scanToken(Dtparse_Scanner *scanner) {
    dtparse_skipWhitespace(scanner);

    scanner->start = scanner->curr;

    if (isdigit((unsigned char)*scanner->curr)) 
        return dtparse_scanNumber(scanner);
    if (isalpha((unsigned char)*scanner->curr)) 
        return dtparse_scanString(scanner);
    if (*scanner->curr == '\0') 
        return dtparse_makeToken(DTPARSE_TOKEN_EOF, scanner);

    char c = *scanner->curr++;
    scanner->col++;

    switch (c) {
        case '-': return dtparse_makeToken(DTPARSE_TOKEN_MINUS, scanner);
        case '+': return dtparse_makeToken(DTPARSE_TOKEN_PLUS,  scanner);
        case ':': return dtparse_makeToken(DTPARSE_TOKEN_COLON, scanner);
        case ',': return dtparse_makeToken(DTPARSE_TOKEN_COMMA, scanner);
    }

    return dtparse_makeErrorToken(scanner, "Unexpected character");
}

/* ============================== Token buffer ============================== */

/* Initialize the token buffer. */
static void dtparse_initTokenBuffer(Dtparse_TokenBuffer *tokbuf) {
    tokbuf->head  = 0;
    tokbuf->tail  = 0;
    tokbuf->count = 0;
}

/* Return 1 if the token buffer is full, 0 otherwise. */
static int dtparse_isTokenBufferFull(Dtparse_TokenBuffer *tokbuf) {
    return (tokbuf->count >= DTPARSE_TOKEN_BUFFER_SIZE);
}

/* Add token to the token buffer. Return 1 on success, 0 otherwise. */
static int dtparse_enqueueToken(Dtparse_TokenBuffer *tokbuf, 
                                Dtparse_Token token) 
{
    if (dtparse_isTokenBufferFull(tokbuf)) return 0;

    tokbuf->tokens[tokbuf->tail] = token;
    tokbuf->tail = (tokbuf->tail + 1) % DTPARSE_TOKEN_BUFFER_SIZE;
    tokbuf->count++;
    return 1;
}

/* Remove a token from the token buffer. 
 * The removed token, if any, is stored into token variable.
 * Return 1 on success, 0 otherwise. */
static int dtparse_dequeueToken(Dtparse_TokenBuffer *tokbuf, 
                                Dtparse_Token *token) 
{
    if (tokbuf->count == 0) return 0;

    *token = tokbuf->tokens[tokbuf->head];
    tokbuf->head = (tokbuf->head + 1) % DTPARSE_TOKEN_BUFFER_SIZE;
    tokbuf->count--;
    return 1;
}

/* Get the i-th token of token buffer. 
 * The selected token, if any, is stored into token variable.
 * Return 1 on success, 0 otherwise. */
static int dtparse_peekToken(Dtparse_TokenBuffer *tokbuf, 
                             int i, Dtparse_Token *token) 
{
    if (i < 0 || i >= tokbuf->count) return 0;

    *token = tokbuf->tokens[(tokbuf->head+i) % DTPARSE_TOKEN_BUFFER_SIZE];

    return 1;
}

/* ================================= Parser ================================= */

/* Initialize the parser. */
static void dtparse_initParser(Dtparse_Parser *parser, 
                               Dtparse_Scanner *scanner,
                               Dtparse_LogFn log_fn, void *ctx) 
{
    parser->scanner = scanner;
    parser->curr_tok = dtparse_scanToken(parser->scanner);
    dtparse_initTokenBuffer(&parser->tokbuf);
    parser->had_error = 0;
    parser->log_fn    = log_fn;
    parser->log_ctx   = ctx;
}

/* Peek ahead 'offset' tokens without consuming them.
 * It also saves the scanned tokens in the parser buffer to avoid later
 * recomputations. */
static Dtparse_Token dtparse_peek(Dtparse_Parser *parser, int offset) {
    if (offset == 0) return parser->curr_tok;

    Dtparse_Token token;

    // Get available tokens from the buffer.
    int cnt = (offset < parser->tokbuf.count) ? offset : parser->tokbuf.count;
    if (cnt > 0) {
        dtparse_peekToken(&parser->tokbuf, cnt-1, &token);
        offset -= cnt;
    }

    // Scan the remaining tokens and add them to the buffer as long as there 
    // is room.
    while (offset > 0 && !dtparse_isTokenBufferFull(&parser->tokbuf)) {
        token = dtparse_scanToken(parser->scanner);
        dtparse_enqueueToken(&parser->tokbuf, token);
        offset--;
    }

    // Buffer full. Scan the rest tokens with a temporary scanner to avoid
    // consuming from the real scanner.
    Dtparse_Scanner tmp_scanner = *(parser->scanner);
    while (offset > 0) {
        token = dtparse_scanToken(&tmp_scanner);
        offset--;
    }

    return token;
}

/* Advance to the next token. */
static void dtparse_advance(Dtparse_Parser *parser) {
    if (dtparse_dequeueToken(&parser->tokbuf, &parser->curr_tok)) return;

    parser->curr_tok = dtparse_scanToken(parser->scanner);
}

/* Return 1 if the current token has the expected type, 0 otherwise. */
static int dtparse_checkCurr(Dtparse_Parser *parser, Dtparse_TokenType type) {
    return parser->curr_tok.type == type;
}

/* Return 1 if the next 'offset' token has the expected type, 0 otherwise. */
static int dtparse_checkNext(Dtparse_Parser *parser, 
                             int offset, Dtparse_TokenType type) 
{
    Dtparse_Token token = dtparse_peek(parser, offset);
    return token.type == type;
}

/* ================================= Report ================================= */

/* Report the message generated by the token.
 * It builds the diagnostic message using format string and variadic arguments, 
 * then it invokes the user callback.
 * The constructed message follows the pattern: at '<TOKEN>': <MESSAGE> 
 * If the token is NULL, the final diagnostic will contain only the
 * <MESSAGE>.*/
static void dtparse_report(Dtparse_Parser *p, Dtparse_LogLevel level, 
                           Dtparse_Token *token, const char *fmt, ...) {
    if (p->had_error) return; // report one error at a time to avoid 
                              // cascades of messages
    if (level == DTPARSE_LOG_ERROR) {
        p->had_error = 1;
    }
    if (!p->log_fn) return;

    char buf[DTPARSE_MSG_BUF_SIZE];
    int pos = 0;

    if (token != NULL) {
        if (token->type == DTPARSE_TOKEN_EOF) {
            pos += snprintf(buf+pos, sizeof(buf)-pos, "at end: ");
        } else if (token->type == DTPARSE_TOKEN_ERROR) {
            pos += snprintf(buf+pos, sizeof(buf)-pos, "%s: ", token->str);
        } else {
            pos += snprintf(buf+pos, sizeof(buf)-pos, 
                    "at '%.*s': ", (int)token->len, token->str);
        }
    }

    va_list args;
    va_start(args, fmt);
    pos += vsnprintf(buf+pos, sizeof(buf)-pos, fmt, args);
    va_end(args);

    if (token != NULL)
        p->log_fn(level, token->line, token->col, buf, p->log_ctx);
    else 
        p->log_fn(level, -1, -1, buf, p->log_ctx);
}

/* ================================ Helpers ================================= */

/* Return 1 if the year is a leap year, 0 otherwise. */
static int dtparse_isLeapYear(int year) {
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

/* Return the number of the month associated to the name. 
 * Return the month number [0-11] (January = 0) on success, return -1
 * otherwise. */
static int dtparse_getMonthFromName(const char *name) {
    static const char *month_names[] = {
        "January",   "February", "March",    "April", 
        "May",       "June",     "July",     "August", 
        "September", "October",  "November", "December"
    };

    static const char *short_month_names[] = {
        "Jan", "Feb", "Mar", "Apr", 
        "May", "Jun", "Jul", "Aug", 
        "Sep", "Oct", "Nov", "Dec"
    };

    for (int i = 0; i < DTPARSE_MONTHS_IN_YEAR; i++) {
        if (strcasecmp(name, month_names[i]) == 0 ||
            strcasecmp(name, short_month_names[i]) == 0)
            return i;
    }

    return -1;
}

/* Return the maximum number of days in the provided month and year, if any,
 * -1 otherwise. 
 * For consistency with the libc tm structure: month is between [0,11] 
 * (January = 0) and year is the full year - DTPARSE_YEAR_BASE.  */
static int dtparse_daysInMonth(int month, int year) {
    if (month < DTPARSE_MONTH_MIN || month > DTPARSE_MONTH_MAX) return -1;

    static const int days_in_month[] = { 31, 28, 31, 30, 
                                         31, 30, 31, 31, 
                                         30, 31, 30, 31 };

    if (month == 1 && dtparse_isLeapYear(year + DTPARSE_YEAR_BASE)) 
        return days_in_month[month] + 1;
    return days_in_month[month];
}

/* Return the number of the weekday associated to the name. 
 * Return the weekday number [0-6] (Sunday = 0) on success, return -1
 * otherwise. */
static int dtparse_getWeekdayFromName(const char *name) {
    static const char *weekday_names[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", 
        "Thursday", "Friday", "Saturday"
    };

    static const char *short_weekday_names[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };

    for (int i = 0; i < DTPARSE_DAYS_IN_WEEK; i++) {
        if (strcasecmp(name, weekday_names[i]) == 0 ||
            strcasecmp(name, short_weekday_names[i]) == 0) 
            return i;
    }

    return -1;
}

/* Return the weekday [0-6] (Sunday = 0) calculated from the provided date.
 * Return -1 on failure. */
static int dtparse_getWeekdayFromDate(struct tm *date) {
    struct tm tmp = *date;
    if ((mktime(&tmp)) == (time_t)-1) return -1;
    return tmp.tm_wday;
}

/* Return the string associated with the weekday [0-6] (Sunday = 0), NULL
 * otherwise. */
static const char *dtparse_getWeekdayName(int wday) {
    static const char *weekday_names[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", 
        "Thursday", "Friday", "Saturday"
    };

    if (wday < 0 || wday > 6) return NULL;
    return weekday_names[wday];
}

/* Return 1 if the token is a correct unit modifier, 0 otherwise. 
 * Note that a valid unit modifier can be made of multiple single modifier
 * units (e.g. 'mdHM' is a valid modifier).
 * See dtparse_showManPage() function for more details. */
static int dtparse_isModUnit(Dtparse_Token *token) {
    if (token->type != DTPARSE_TOKEN_STRING) return 0;

    for (size_t i = 0; i < token->len; i++) {
        char c = token->str[i];
        switch (c) {
        case 'y':
        case 'm':
        case 'w':
        case 'd':
        case 'H':
        case 'M': 
        case 'S':
            continue;
        default: return 0;
        }
    }

    return 1;
}

/* Return 1 if the next tokens to parse is a modifier, 0 otherwise. 
 * See dtparse_showManPage() function for more details. */
static int dtparse_isMod(Dtparse_Parser *p) {
    if (!dtparse_checkCurr(p, DTPARSE_TOKEN_MINUS) &&
        !dtparse_checkCurr(p, DTPARSE_TOKEN_PLUS)) return 0;

    // Check if the next token is an unit.
    Dtparse_Token next = dtparse_peek(p, 1);
    if (dtparse_isModUnit(&next)) return 1;
    
    // Check if the next token is a number and the one after is an unit.
    Dtparse_Token next2 = dtparse_peek(p, 2);
    return (next.type == DTPARSE_TOKEN_NUMBER && dtparse_isModUnit(&next2));
}

/* Copy at most bufsize-1 character from the current parsed token to the
 * buffer. */
static void dtparse_tokenToStr(Dtparse_Parser *parser, 
                               char *buf, size_t bufsize) 
{
    size_t len = (parser->curr_tok.len < (bufsize-1)) ? parser->curr_tok.len
                                                      : bufsize - 1;
    memcpy(buf, parser->curr_tok.str, len);
    buf[len] = '\0';
}

/* Return the value of the number pointed by the parser. */
static int dtparse_parseNumber(Dtparse_Parser *parser) {
    int res = 0;
    for (size_t i = 0; i < parser->curr_tok.len; i++) {
        int digit = parser->curr_tok.str[i] - '0';
        if (res > (INT_MAX - digit) / 10) {
            dtparse_report(parser, DTPARSE_LOG_ERROR, &parser->curr_tok,
                           "Number too large. The maximum is %d.", INT_MAX);
            return INT_MAX; // overflow guard
        }
        res = res * 10 + digit;
    }

    return res;
}

/* Normalize a date by bringing year, month and day into their valid ranges.
 * It handles day and month overflows taking leap years into account.
 * The function modifies the original date values via pointers. */
static void dtparse_normalizeDate(int *year, int *month, int *day) {
    // Normalize month first because day depends on it. 
    *year += dtparse_fdiv(*month, DTPARSE_MONTHS_IN_YEAR);
    *month = dtparse_fmod(*month, DTPARSE_MONTHS_IN_YEAR);

    // The number of days varies depending on the month they are in.
    while (1) {
        int mday = dtparse_daysInMonth(*month, *year);

        if (*day >= DTPARSE_DAY_MIN && *day <= mday) break;

        if (*day < DTPARSE_DAY_MIN) {
            (*month)--;
            if (*month < DTPARSE_MONTH_MIN) {
                *month += DTPARSE_MONTHS_IN_YEAR;
                (*year)--;
            }
            *day += dtparse_daysInMonth(*month, *year);
        } else {
            *day -= mday;
            (*month)++;
            if (*month > DTPARSE_MONTH_MAX) {
                *month -= DTPARSE_MONTHS_IN_YEAR;
                (*year)++;
            }
        }
    }
}

/* ============================= Parse datetime ============================= */

/* Parse date. 
 * See dtparse_showManPage() function for more details. */
static void dtparse_parseDate(Dtparse_Parser *p) {
    int year = -1, month = -1, weekday = -1, day = -1;
    Dtparse_Token week_tok = {0}, day_tok = {0};

    /* Parse 'yyyy-mm-dd' */
    if ((dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER) && 
         dtparse_checkNext(p, 1, DTPARSE_TOKEN_MINUS)) || 
        (dtparse_checkCurr(p, DTPARSE_TOKEN_MINUS) && !dtparse_isMod(p))) {

        for (int i = 0; i < 3; i++) {
            if (dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER)) {
                if (dtparse_checkNext(p, 1, DTPARSE_TOKEN_COLON)) // it's time
                        break;

                if (i == 0) {        // year
                    year = dtparse_parseNumber(p);
                    dtparse_advance(p);
                } else if (i == 1) { // month
                    month = dtparse_parseNumber(p);
                    month--; // user uses 1-12, libc tm struct uses 0-11 

                    if (month < DTPARSE_MONTH_MIN || 
                        month > DTPARSE_MONTH_MAX) {
                        dtparse_report(p, DTPARSE_LOG_ERROR, &p->curr_tok, 
                                     "Month number must be between [%d,%d].",
                                     DTPARSE_MONTH_MIN+1, DTPARSE_MONTH_MAX+1);
                        break;
                    }

                    dtparse_advance(p);
                } else if (i == 2) { // day
                    day = dtparse_parseNumber(p);
                    day_tok = p->curr_tok;
                    dtparse_advance(p);
                }
            }

            if (i == 2) break; // dd has no '-' hanging

            if (!dtparse_checkCurr(p, DTPARSE_TOKEN_MINUS)) {
                char *date_comp_str = (i == 0) ? "year"  : 
                                      (i == 1) ? "month" : 
                                                 "day";
                dtparse_report(p, DTPARSE_LOG_ERROR, &p->curr_tok, 
                        "Expected '-' after %s (yyyy-mm-dd).",
                        date_comp_str);
                goto apply_date;
            }

            dtparse_advance(p); // consume '-'
        }
    }
    else if (dtparse_checkCurr(p, DTPARSE_TOKEN_STRING) ||
             dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER)) {
        char buf[32];
        dtparse_tokenToStr(p, buf, sizeof(buf));

        /* Parse 'today' 'tomorrow' 'yesterday' */
        int is_keyword = 1;
        if (strcasecmp(buf, "today") == 0) {
            day_tok = p->curr_tok;
        } else if (strcasecmp(buf, "tomorrow") == 0) {
            p->dt.tm_mday++;
            day_tok = p->curr_tok;
        } else if (strcasecmp(buf, "yesterday") == 0) {
            p->dt.tm_mday--;
            day_tok = p->curr_tok;
        } else {
            is_keyword = 0;
        }

        if (is_keyword) {
            dtparse_normalizeDate(&p->dt.tm_year,
                                  &p->dt.tm_mon, 
                                  &p->dt.tm_mday);
            day = p->dt.tm_mday;
            dtparse_advance(p); // consume keyword
        }
        /* Parse '[Weekday,] [dd] [Month] [yyyy]' */
        else {
            dtparse_tokenToStr(p, buf, sizeof(buf));
            weekday = dtparse_getWeekdayFromName(buf);

            if (weekday != -1) {
                week_tok = p->curr_tok;
                dtparse_advance(p); // consume weekday

                if (dtparse_checkCurr(p, DTPARSE_TOKEN_COMMA)) 
                    dtparse_advance(p); // consume ','
            }

            if (dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER)) {
                if (dtparse_checkNext(p, 1, DTPARSE_TOKEN_COLON)) // it's time
                    goto apply_date;

                day = dtparse_parseNumber(p);
                day_tok = p->curr_tok;
                dtparse_advance(p); // consume day
            }

            if (dtparse_checkCurr(p, DTPARSE_TOKEN_STRING)) {
                dtparse_tokenToStr(p, buf, sizeof(buf));

                if (strcasecmp(buf, "now") == 0) goto apply_date;

                month = dtparse_getMonthFromName(buf);

                if (month == -1) {
                    dtparse_report(p, DTPARSE_LOG_ERROR, &p->curr_tok, 
                         "%s is not a valid %s", 
                         buf, 
                         (weekday == -1) ? "name for either weekday or month." 
                                         : "month name.");
                    goto apply_date;
                }

                dtparse_advance(p); // consume month
            }

            if (dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER)) {
                if (dtparse_checkNext(p, 1, DTPARSE_TOKEN_COLON)) // it's time
                    goto apply_date;

                year = dtparse_parseNumber(p);
                dtparse_advance(p); // consume year
            }
        }

    }

apply_date:
    if (year != -1) {
        p->dt.tm_year = year - DTPARSE_YEAR_BASE;
    }

    if (month != -1) {
        p->dt.tm_mon = month;
    }

    if (weekday != -1 && day == -1) {
        int day_offset = weekday - p->dt.tm_wday;
        p->dt.tm_mday += day_offset;
        dtparse_normalizeDate(&p->dt.tm_year, &p->dt.tm_mon, &p->dt.tm_mday);
    }

    if (day != -1) {
        int maxday = dtparse_daysInMonth(p->dt.tm_mon, p->dt.tm_year);
        if (day < DTPARSE_DAY_MIN || day > maxday) {
            char month_name[16];
            strftime(month_name, sizeof(month_name), "%B", &p->dt);

            dtparse_report(p, DTPARSE_LOG_ERROR, &day_tok, 
                           "%s days must be between [%d,%d].", 
                           month_name, DTPARSE_DAY_MIN, maxday);
        } else {
            p->dt.tm_mday = day;
        }
    }

    if (weekday != -1 && day != -1) {
        // Check if target date has different weekday from the one specified.
        int wday = dtparse_getWeekdayFromDate(&p->dt);
        if (weekday != wday) {
            char date_buf[64];

            const char *false_wday_name = dtparse_getWeekdayName(weekday);
            const char *true_wday_name  = dtparse_getWeekdayName(wday);

            strftime(date_buf, sizeof(date_buf), 
                     DTPARSE_NAMED_DATE_FORMAT_STRING, &p->dt);

            dtparse_report(p, DTPARSE_LOG_WARNING, &week_tok, 
                           "%s is %s not %s. Weekday ignored.", 
                           date_buf, true_wday_name, false_wday_name);
        }
    }
}

/* Parse time.
 * See dtparse_showManPage() function for more details. */
static void dtparse_parseTime(Dtparse_Parser *p) {
    int hour = -1, min = -1, sec = -1;
    int is_12_hour = 0, is_am = 0;
    Dtparse_Token hour_tok = {0}, min_tok = {0}, sec_tok = {0};

    /* Parse 'HH:MM[:SS][AM/PM]' */
    if (dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER) &&
        dtparse_checkNext(p, 1, DTPARSE_TOKEN_COLON)) {

        sec = 0; // reset seconds

        hour = dtparse_parseNumber(p);
        hour_tok = p->curr_tok;
        dtparse_advance(p); // consume hour
        dtparse_advance(p); // consume ':'

        if (!dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER)) {
            dtparse_report(p, DTPARSE_LOG_ERROR, &p->curr_tok, 
                           "Expected minutes after ':' in time (HH:MM).");
            goto apply_time;
        }

        min = dtparse_parseNumber(p);
        min_tok = p->curr_tok;
        dtparse_advance(p); // consume minutes

        if (dtparse_checkCurr(p, DTPARSE_TOKEN_COLON)) {
            dtparse_advance(p); // consume ':'
            if (!dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER)) {
                dtparse_report(p, DTPARSE_LOG_ERROR, &p->curr_tok, 
                        "Expected seconds after ':' in time (HH:MM:SS).");
                goto apply_time;
            }

            sec = dtparse_parseNumber(p);
            sec_tok = p->curr_tok;
            dtparse_advance(p); // consume seconds
        }

        if (dtparse_checkCurr(p, DTPARSE_TOKEN_STRING)) {
            char buf[32];
            dtparse_tokenToStr(p, buf, sizeof(buf));
            if (strcasecmp(buf, "am") == 0) {
                is_12_hour  = 1;
                is_am = 1;
                dtparse_advance(p); // consume "am"
            } else if (strcasecmp(buf, "pm") == 0) {
                is_12_hour  = 1;
                is_am = 0;
                dtparse_advance(p); // consume "pm"
            }
        }
    }
    /* Parse 'now' */
    else if (dtparse_checkCurr(p, DTPARSE_TOKEN_STRING)) {
        char buf[32];
        dtparse_tokenToStr(p, buf, sizeof(buf));

        if (strcasecmp(buf, "now") == 0) dtparse_advance(p); // consume 'now'
    }

apply_time:
    if (hour != -1) {
        if (is_12_hour) { // 12-hour clock
            if (hour < DTPARSE_12_HOUR_MIN || hour > DTPARSE_12_HOUR_MAX) {
                dtparse_report(p, DTPARSE_LOG_ERROR, &hour_tok, 
                               "Hour must be between [%d,%d] AM/PM.",
                               DTPARSE_12_HOUR_MIN, DTPARSE_12_HOUR_MAX);
            } else {
                if (is_am) 
                    p->dt.tm_hour = (hour == 12) ? 0 : hour;
                else
                    p->dt.tm_hour = (hour == 12) ? 
                                    12 : hour + DTPARSE_HALF_HOURS_IN_DAY;
            }
        } else {          // 24 hour clock
            if (hour < DTPARSE_24_HOUR_MIN || hour > DTPARSE_24_HOUR_MAX) {
                dtparse_report(p, DTPARSE_LOG_ERROR, &hour_tok, 
                               "Hour must be between [%d,%d].",
                               DTPARSE_24_HOUR_MIN, DTPARSE_24_HOUR_MAX);
            } else {
                p->dt.tm_hour = hour;
            }
        }
    }

    if (min != -1) {
        if (min < DTPARSE_MINUTE_MIN || min > DTPARSE_MINUTE_MAX) {
            dtparse_report(p, DTPARSE_LOG_ERROR, &min_tok, 
                           "Minutes must be between [%d,%d].",
                           DTPARSE_MINUTE_MIN, DTPARSE_MINUTE_MAX);
        } else {
            p->dt.tm_min = min;
        }
    }

    if (sec != -1) {
        if (sec < DTPARSE_SECOND_MIN || sec > DTPARSE_SECOND_MAX) {
            dtparse_report(p, DTPARSE_LOG_ERROR, &sec_tok, 
                           "Seconds must be between [%d,%d].",
                           DTPARSE_SECOND_MIN, DTPARSE_SECOND_MAX);
        } else {
            p->dt.tm_sec = sec;
        }
    }
}

/* Parse modifiers.
 * See dtparse_showManPage() function for more details. */
static void dtparse_parseModifiers(Dtparse_Parser *p) {
    /* Parse '(+|-)[NUM]UNIT[UNIT...]' */
    while (!dtparse_checkCurr(p, DTPARSE_TOKEN_EOF)) {
        if (!dtparse_checkCurr(p, DTPARSE_TOKEN_MINUS) &&
            !dtparse_checkCurr(p, DTPARSE_TOKEN_PLUS)) {
            dtparse_report(p, DTPARSE_LOG_ERROR, &p->curr_tok, 
                    "Expected modifier or EOF.");
            return;
        }

        int sign = dtparse_checkCurr(p, DTPARSE_TOKEN_PLUS) ? 1 : -1;
        dtparse_advance(p); // consume '-' or '+'

        do {
            int has_number = 0;

            int delta = 1;
            if (dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER)) {
                has_number = 1;
                delta = dtparse_parseNumber(p);
                dtparse_advance(p); // consume number
            }

            delta *= sign;

            if (!dtparse_isModUnit(&p->curr_tok)) {
                dtparse_report(p, DTPARSE_LOG_ERROR, &p->curr_tok,
                               "Expected %smodifier unit " 
                               "('y' 'm' 'w' 'd' 'H' 'M' 'S').",
                               has_number ? "" : "number or ");
                return;
            }

            for (size_t i = 0; i < p->curr_tok.len; i++) {
                if (i > 0) delta = 1 * sign; // no number is provided after
                                             // the first character

                char c = p->curr_tok.str[i];
                int touch = 0;

                // Fall-through the time units from smallest to largest
                // (second - year) wrapping the current time unit to its
                // valid range and passing the remaining quantity to the
                // next units.
                // At each step we set the value of the smaller unit and then
                // we pass the excess to the following larger one. In this way
                // we ensure that at the end all fields of datetime do not
                // overflow.
                switch (c) {
                case 'S':
                    if (!touch) { 
                        p->dt.tm_sec += delta; 
                        touch = 1; 
                        if (delta != 0) {
                            dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                           abs(delta) == 1 ? "%s one second."
                                                           : "%s %d seconds.", 
                                           (delta > 0) ? "Added" 
                                                       : "Subtracted",
                                           abs(delta));
                        }
                    }
                    p->dt.tm_min += dtparse_fdiv(p->dt.tm_sec, 
                                                 DTPARSE_SECONDS_IN_MINUTE);
                    p->dt.tm_sec  = dtparse_fmod(p->dt.tm_sec, 
                                                 DTPARSE_SECONDS_IN_MINUTE);
                    /* fallthrough */
                case 'M': 
                    if (!touch) { 
                        p->dt.tm_min += delta; 
                        touch = 1; 
                        if (delta != 0) {
                            dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                           abs(delta) == 1 ? "%s one minute."
                                                           : "%s %d minutes.", 
                                           (delta > 0) ? "Added" 
                                                       : "Subtracted",
                                           abs(delta));
                        }
                    }
                    p->dt.tm_hour += dtparse_fdiv(p->dt.tm_min, 
                                                  DTPARSE_MINUTES_IN_HOUR);
                    p->dt.tm_min   = dtparse_fmod(p->dt.tm_min, 
                                                  DTPARSE_MINUTES_IN_HOUR);
                    /* fallthrough */
                case 'H':
                    if (!touch) { 
                        p->dt.tm_hour += delta; 
                        touch = 1; 
                        if (delta != 0) {
                            dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                           abs(delta) == 1 ? "%s one hour."
                                                           : "%s %d hours.", 
                                           (delta > 0) ? "Added" 
                                                       : "Subtracted",
                                           abs(delta));
                        }
                    }
                    p->dt.tm_mday += dtparse_fdiv(p->dt.tm_hour, 
                                                  DTPARSE_HOURS_IN_DAY);
                    p->dt.tm_hour  = dtparse_fmod(p->dt.tm_hour, 
                                                  DTPARSE_HOURS_IN_DAY);
                    /* fallthrough */
                case 'w':
                    if (!touch) {
                        p->dt.tm_mday += (delta * DTPARSE_DAYS_IN_WEEK); 
                        touch = 1;
                        if (delta != 0) {
                            dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                           abs(delta) == 1 ? "%s one week."
                                                           : "%s %d weeks.", 
                                           (delta > 0) ? "Added" 
                                                       : "Subtracted",
                                           abs(delta));
                        }
                    }
                    /* fallthrough */
                case 'd':
                    if (!touch) {
                        p->dt.tm_mday += delta; 
                        touch = 1; 
                        if (delta != 0) {
                            dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                           abs(delta) == 1 ? "%s one day."
                                                           : "%s %d days.", 
                                           (delta > 0) ? "Added" 
                                                       : "Subtracted",
                                           abs(delta));
                            
                        }
                    }

                    dtparse_normalizeDate(&p->dt.tm_year,
                                          &p->dt.tm_mon, 
                                          &p->dt.tm_mday);
                    break;
                case 'm':
                    if (!touch) {
                        p->dt.tm_mon += delta; 
                        touch = 1; 
                        if (delta != 0) {
                            dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                           abs(delta) == 1 ? "%s one month."
                                                           : "%s %d months.", 
                                           (delta > 0) ? "Added" 
                                                       : "Subtracted",
                                           abs(delta));
                        }
                    }

                    p->dt.tm_year += dtparse_fdiv(p->dt.tm_mon, 
                                                  DTPARSE_MONTHS_IN_YEAR);
                    p->dt.tm_mon   = dtparse_fmod(p->dt.tm_mon, 
                                                  DTPARSE_MONTHS_IN_YEAR);
                    /* fallthrough */
                case 'y': {
                    if (!touch) {
                        p->dt.tm_year += delta; 
                        touch = 1; 
                        if (delta != 0) {
                            dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                           abs(delta) == 1 ? "%s one year."
                                                           : "%s %d years.", 
                                           (delta > 0) ? "Added" 
                                                       : "Subtracted",
                                           abs(delta));
                        }
                    }

                    int target_mday = dtparse_daysInMonth(p->dt.tm_mon, 
                                                          p->dt.tm_year);

                    // target month clamps days
                    if (p->dt.tm_mday > target_mday) {
                        int old_mday = p->dt.tm_mday;
                        p->dt.tm_mday = target_mday;

                        char month_name[16];
                        strftime(month_name, sizeof(month_name), "%B", &p->dt);
                        dtparse_report(p, DTPARSE_LOG_INFO, &p->curr_tok,
                                       "Days clamped from %d to %d in %s.",
                                       old_mday, target_mday, month_name);
                    }

                    break;
                    }
                }
            }

            dtparse_advance(p); // consume unit

        } while (dtparse_checkCurr(p, DTPARSE_TOKEN_NUMBER) ||
                 dtparse_checkCurr(p, DTPARSE_TOKEN_STRING));
    }
}

/* Parse datetime.
 * See dtparse_showManPage() function for more details. */
static void dtparse_parseDatetime(Dtparse_Parser *p) {
    dtparse_parseDate(p);
    dtparse_parseTime(p);
    dtparse_parseModifiers(p);
}

DTPARSEDEF time_t dtparse_parse(const char *source, 
                                Dtparse_LogFn log_fn, void *ctx) 
{
    Dtparse_Scanner scanner;
    dtparse_initScanner(&scanner, source);

    Dtparse_Parser parser;
    dtparse_initParser(&parser, &scanner, log_fn, ctx);

    time_t now = time(NULL);
    localtime_r(&now, &parser.dt);
    parser.dt.tm_isdst = -1; // let system figure out DST

    dtparse_parseDatetime(&parser);

    if (!dtparse_checkCurr(&parser, DTPARSE_TOKEN_EOF)) {
        dtparse_report(&parser, DTPARSE_LOG_ERROR, &parser.curr_tok, 
                       "Expected end of input.");
    }

    if (parser.had_error) return (time_t)-1;
    return mktime(&parser.dt);
}

/* ============================ Help and manuals ============================ */

static const char *DTPARSE_DATETIME_MAN_PAGE =
"DTPARSE-DATETIME\n\n"
"NAME\n"
"       dtparse-datetime - Datetime string format for the dtparse library\n\n"
"SYNOPSIS\n"
"       [BASE_TIME] [MODIFIERS...]\n\n"
"DESCRIPTION\n"
"       The dtparse datetime parser evaluates user input sequentially from left\n"
"       to right, resolving to a Unix timestamp. If no base time is provided,\n"
"       the parser uses the current system time as the base. Any missing\n"
"       component (year, month, day, time) will default to its current system\n"
"       value.\n\n"
"BASE TIME\n"
"       You can specify a date, a time, or a combination of both.\n\n"
"       1. ISO Date Format: yyyy-mm-dd\n"
"         The parser supports the ISO date format with hanging - characters to\n"
"         indicate missing components.\n\n"
"           yyyy-mm-dd : Exact year, month and day.\n"
"           yyyy-mm-   : Exact year and month.\n"
"           yyyy--dd   : Exact year and day.\n"
"           yyyy--     : Exact year only.\n"
"           -mm-dd     : Exact month and day.\n"
"           -mm-       : Exact month only.\n"
"           --dd       : Exact day only.\n\n"
"       2. Named Date Format: [Weekday,] [dd] [Month] [yyyy]\n"
"         The parser supports a highly flexible named date format where every\n"
"         component is entirely optional.\n\n"
"           Weekday : The day of the week in short (Sun, Mon, ...) or long\n"
"                     (Sunday, Monday, ...) form.\n"
"           dd      : Exact day number.\n"
"           Month   : The month name in short (Jan, Feb, ...) or long (January,\n"
"                     February, ...) form.\n"
"           yyyy    : Exact year number.\n\n"
"         Weekday evaluates to a day in the current week window (from Sunday to\n"
"         Saturday). For example, if today is Wednesday and you type `Sunday`,\n"
"         it evaluates to three days ago. If you type `Friday`, it evaluates to\n"
"         two days from now. If an exact day (dd) is also provided, the weekday\n"
"         is ignored.\n\n"
"       3. Time Format: HH:MM[:SS][AM|PM]\n\n"
"           HH:MM    : Exact hour and minutes (seconds default to 0).\n"
"           HH:MM:SS : Exact hour, minutes and seconds.\n\n"
"         An optional AM or PM can be appended for 12-hour clock times.\n\n"
"       4. Semantic Keywords:\n\n"
"           today     : Current day.\n"
"           tomorrow  : The day after the current day.\n"
"           yesterday : The day before the current day.\n"
"           now       : Current time.\n\n"
"         `today`, `tomorrow` and `yesterday` keywords can be used instead of\n"
"         date. `now` keyword can be used instead of time.\n\n"
"       VALUE RANGES:\n"
"           dd (day)    : [1-31]\n"
"           mm (month)  : [1-12]\n"
"           Month       : Short (Jan, Feb, ...), or Long (January, ...)\n"
"           Weekday     : Short (Sun, Mon, ...), or Long (Sunday, ...)\n"
"           HH (hour)   : [0-23] or [1-12 AM/PM]\n"
"           MM (minute) : [0-59]\n"
"           SS (second) : [0-59]\n\n"
"         These ranges apply only to the base time. Modifiers can freely exceed\n"
"         these limits (e.g., +90M is perfectly valid).\n\n"
"MODIFIERS (DELTAS)\n"
"       Modifiers apply shifts to the base time.\n\n"
"       Format: (+|-)[NUM]UNITS[[NUM]UNITS...]\n"
"       Units:\n"
"         y (year), m (month), w (week), d (day)\n"
"         H (hour), M (minute), S (second)\n\n"
"       If NUM is omitted, it defaults to 1 (e.g., +d means add one day).\n\n"
"       Modifiers can be chained under the same sign. If you concatenate units\n"
"       without repeating the number, the first unit takes [NUM] and subsequent\n"
"       units default to 1 (e.g., +3d7H adds 3 days and 7 hours; +3dHM adds\n"
"       3 days, 1 hour, and 1 minute).\n\n"
"CORNER CASES\n"
"       When using the month modifier (m), if the target month has fewer days\n"
"       than the current day value, the parser clamps the date to the last day\n"
"       of the target month (e.g., adding +1m to 31 October clamps to 30\n"
"       November). Note that this clamping is permanent for subsequent steps if\n"
"       modifiers are chained (e.g., 31 Jan +1m +1m evaluates as 31 Jan ->\n"
"       28 Feb -> 28 March).\n"
"       This clamping also applies to leap years when using the year modifier\n"
"       (y); for example, adding +1y to 29 February clamps to 28 February of\n"
"       the following non-leap year.\n\n"
"       Because modifiers are evaluated strictly left-to-right, non-uniform\n"
"       units (like months and years) can produce different results depending\n"
"       on the order.\n"
"       For example, if today is 30 January, +1m +2d means 30 Jan + 1 month\n"
"       (Clamps to 28 Feb) + 2 days = 2 March. However, +2d +1m means 30 Jan +\n"
"       2 days (1 Feb) + 1 month = 1 March.\n\n"
"EXAMPLES\n"
"      today 15:00 +2H -> Today's date at 17:00.\n"
"      tomorrow +90M   -> Tomorrow's date at current time plus 90 minutes.\n"
"      2024-02-29      -> 29 February, 2024, at current time.\n"
"      -10-31 12:00    -> 31 October of the current year at 12:00.\n\n"
"      22 May -> 22 May of the current year at the current time.\n"
"      Friday, 25 October 2026 -> 25 October, 2026. (If 25 Oct is not a Friday,\n"
"                                 'Friday' is ignored).\n\n"
"      31 Jan +1m +1m    -> 28 March. (Adding a month to 31 Jan clamps to 28\n"
"                           Feb; adding another month to 28 Feb yields 28\n"
"                           March).\n"
"      2024-02-29 +1y    -> 28 February, 2025. (Clamp leap days to the 28th of\n"
"                           non-leap years).\n"
"      Mon 25 March 2026 -> 25 March, 2026. (25 March 2026 is actually a\n"
"                           Wednesday; Monday is ignored).\n";

DTPARSEDEF const char *dtparse_getManPage(void) {
    return DTPARSE_DATETIME_MAN_PAGE;
}

DTPARSEDEF void dtparse_showManPage(void) {
    fputs(DTPARSE_DATETIME_MAN_PAGE, stdout);
}

#endif // DTPARSE_IMPLEMENTATION
