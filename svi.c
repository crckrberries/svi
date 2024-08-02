/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * For more information, please refer to <http://unlicense.org/>
 */

/*
 * TODO (from highest to lowest priority):
 * - test on another platform (most likely BSD)
 * - command mode
 * - writing to file
 * - opening files
 * - UTF-8 support
 * - maybe try to handle OOM more gracefully instead of exiting instantly
 */

/*
 * ===========================================================================
 * configurable macros
 */

/*
 * ===================
 * editing buffer
 */

/* how many rows to initially allocate for the buffer, cant be 0 */
#define INITIAL_BUFFER_ROWS 32

/* how many rows to add to the buffer's size when it's too small, cant be 0 */
#define BUF_SIZE_INCREMENT  16

/* how many columns to initially allocate for each row, cant be 0 or 1 */
#define INITIAL_ROW_SIZE    128

/* how many columns to add to a row's size when it's too small, cant be 0 */
#define ROW_SIZE_INCREMENT  64

/*
 * ===================
 * terminal
 */

/* if getting the terminal's size fails, the following size is used instead */
#define FALLBACK_WIDTH  80
#define FALLBACK_HEIGHT 24

/*
 * how long to wait for the terminal's response when getting the size with
 * the fallback method. cant be higher than 1000
 */
#define RESIZE_FALLBACK_MS 1000

/*
 * ===========================================================================
 * includes
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/ioctl.h>
#include <sys/select.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/*
 * ===========================================================================
 * macros and types
 */

/* terminal */
#define COLOR_DEFAULT NULL
#define COLOR_RESET   "\033[0m"
#define COLOR_BLACK   "\033[30m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

#define TERM_CLEAR() write(STDOUT_FILENO, "\033[2J\033[;H", 8)

/* utility */
#define ROUNDUPTO(x, multiple) (((x + multiple - 1) / multiple) * multiple)

/* enums */
enum event_type {
#if defined(SIGWINCH)
	TERM_EVENT_RESIZE,
#endif /* SIGWINCH */
	TERM_EVENT_KEY
};

enum key {
	KEY_CHAR,
	KEY_ESC,
	KEY_ARROW_UP,
	KEY_ARROW_DOWN,
	KEY_ARROW_RIGHT,
	KEY_ARROW_LEFT,
	KEY_ENTER,
	KEY_BACKSPACE,
	KEY_DELETE
};

/* structs */
struct term_event {
	enum event_type type;
	enum key key;
	char ch;
};

struct str {
	char *s;
	size_t len, size;
};

struct buf {
	struct str **b;
	size_t size;
};

/*
 * ===========================================================================
 * function declarations
 */

/* memory allocation */
static void *ecalloc(size_t nmemb, size_t size);
static void *emalloc(size_t size);
static void *erealloc(void *ptr, size_t size);
static void *ereallocarray(void *ptr, size_t nmemb, size_t size);

/* errors */
static void die(const char *fmt, ...);

/* terminal */
static void readkey(struct term_event *ev);
static void term_event_wait(struct term_event *ev);
static void term_init(void);
static void term_print(int x, int y, const char *color, const char *str);
static void term_set_cursor(int x, int y);
static void term_shutdown(void);
static int term_size(int size[2]);
static int try_read_chr(char *c);
#if defined(SIGWINCH)
static void winch(int unused);
#endif /* SIGWINCH */

/* strings */
static void str_insertchar(struct str *str, char c, size_t index);
static void str_removechar(struct str *str, size_t index);

/* buffer management */
static void buf_char_insert(struct buf *buf, size_t elem, char c,
		size_t index);
static void buf_char_remove(struct buf *buf, size_t elem, size_t index);
static void buf_create(struct buf *buf, size_t size);
static size_t buf_elem_len(struct buf *buf, size_t elem);
static void buf_free(const struct buf *buf);
static void buf_resize(struct buf *buf, size_t size);

/* main program loop */
static void run(void);

/*
 * ===========================================================================
 * global variables
 */
static const char *argv0 = NULL;

/* terminal state */
static int term_initialized = 0;
static struct termios tio, oldtio;
static int old_stdin_flags;
#if defined(SIGWINCH)
static volatile sig_atomic_t win_resized = 0;
static sigset_t oldmask;
#endif /* SIGWINCH */

/*
 * ===========================================================================
 * memory allocation
 */
static void *
ecalloc(size_t nmemb, size_t size)
{
	void *ret = calloc(nmemb, size);
	if (!ret)
		die("calloc: out of memory");
	return ret;
}

static void *
emalloc(size_t size)
{
	void *ret = malloc(size);
	if (!ret)
		die("malloc: out of memory");
	return ret;
}

static void *
erealloc(void *ptr, size_t size)
{
	void *ret = realloc(ptr, size);
	if (!ret)
		die("realloc: out of memory");
	return ret;
}

static void *
ereallocarray(void *ptr, size_t nmemb, size_t size)
{
	/* overflow checking taken from musl's calloc implementation */
	if (size > 0 && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		die("reallocarray: out of memory");
	}
	return erealloc(ptr, nmemb * size);
}

/*
 * ===========================================================================
 * errors
 */
static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (term_initialized)
		term_shutdown();
	if (fmt) {
		if (argv0)
			fprintf(stderr, "%s: ", argv0);
		vfprintf(stderr, fmt, ap);
		if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
			fprintf(stderr, " %s\n", strerror(errno));
		else
			putc('\n', stderr);
	}
	va_end(ap);
	exit(1);
}

/*
 * ===========================================================================
 * terminal
 */
static void
readkey(struct term_event *ev)
{
	char c;
	for (;;) {
		read(STDIN_FILENO, &c, 1);
		switch (c) {
		case '\033':
			if (try_read_chr(&c)) {
				if (c == '[' && try_read_chr(&c)) {
					switch (c) {
					case '3':
						if (try_read_chr(&c) &&
								c == '~') {
							ev->key = KEY_DELETE;
							return;
						}
						break;
					case 'A':
						ev->key = KEY_ARROW_UP;
						return;
					case 'B':
						ev->key = KEY_ARROW_DOWN;
						return;
					case 'C':
						ev->key = KEY_ARROW_RIGHT;
						return;
					case 'D':
						ev->key = KEY_ARROW_LEFT;
						return;
					}
				}
			} else {
				ev->key = KEY_ESC;
				return;
			}
			break;
		case '\r':
			ev->key = KEY_ENTER;
			return;
		case 127:
			ev->key = KEY_BACKSPACE;
			return;
		default:
			/* currently only ASCII is supported */
			if (c < 0x7f) {
				ev->key = KEY_CHAR;
				ev->ch = c;
				return;
			}
			break;
		}
	}
}

static void
term_event_wait(struct term_event *ev)
{
	fd_set rfds;
	int rv;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
#if defined(SIGWINCH)
	/* do pselect() to wait for SIGWINCH or data on stdin */
	rv = pselect(1, &rfds, NULL, NULL, NULL, &oldmask);
	if (rv < 0) {
		if (errno == EINTR && win_resized) {
			/* got SIGWINCH */
			win_resized = 0;
			ev->type = TERM_EVENT_RESIZE;
			return;
		} else {
			die("pselect:");
		}
	} else if (rv) {
		/* data available on stdin */
		ev->type = TERM_EVENT_KEY;
		readkey(ev);
	} else {
		/* ... can this even happen? */
		die("pselect: timeout");
	}
#else
	/* do select() to wait for data on stdin */
	rv = select(1, &rfds, NULL, NULL, NULL);
	if (rv < 0) {
		die("select:");
	} else if (rv) {
		/* data available on stdin */
		ev->type = TERM_EVENT_KEY;
		readkey(ev);
	} else {
		/* ... can this even happen? */
		die("select: timeout");
	}
#endif /* SIGWINCH */
}

static void
term_init(void)
{
#if defined(SIGWINCH)
	struct sigaction sa;
	sigset_t mask;
#endif /* SIGWINCH */

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		die("stdin and stdout must be a terminal");
	if (tcgetattr(STDIN_FILENO, &tio) < 0)
		die("tcgetattr:");
	memcpy(&oldtio, &tio, sizeof(struct termios));

	/* set raw mode */
	tio.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP
			| INLCR | IGNCR | ICRNL | IXON);
	tio.c_oflag &= ~(tcflag_t)OPOST;
	tio.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
	tio.c_cflag |= (tcflag_t)CS8;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) < 0)
		die("tcsetattr:");

	/* set non-blocking mode for stdin */
	if ((old_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0)) < 0)
		die("fcntl:");
	if (fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags | O_NONBLOCK) < 0)
		die("fcntl:");

#if defined(SIGWINCH)
	/* set handler for SIGWINCH */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = winch;
	if (sigaction(SIGWINCH, &sa, NULL) < 0)
		die("sigaction:");

	/* block SIGWINCH */
	sigemptyset(&mask);
	sigaddset(&mask, SIGWINCH);
	if (sigprocmask(SIG_BLOCK, &mask, &oldmask) < 0)
		die("sigprocmask:");
#endif /* SIGWINCH */

	TERM_CLEAR();
	term_initialized = 1;
}

static void
term_print(int x, int y, const char *color, const char *str)
{
	if (x < 0 || y < 0)
		return;
	printf("\033[%d;%dH\033[2K", y + 1, x + 1);
	if (color)
		fputs(color, stdout);
	fputs(str, stdout);
	if (color)
		fputs(COLOR_RESET, stdout);
	fflush(stdout);
}

static void
term_set_cursor(int x, int y)
{
	if (x < 0 || y < 0)
		return;
	printf("\033[%d;%dH", y + 1, x + 1);
	fflush(stdout);
}

static void
term_shutdown(void)
{
	/* avoid infinite recursion with die() if something fails */
	term_initialized = 0;

	if (tcsetattr(STDIN_FILENO, TCSANOW, &oldtio) < 0)
		die("tcsetattr:");
	if (fcntl(STDIN_FILENO, F_SETFL, old_stdin_flags) < 0)
		die("fcntl:");

	TERM_CLEAR();
}

static int
term_size(int size[2])
{
	fd_set rfds;
	struct timeval timeout;

	char buf[16];
	char c = 0;
	size_t i = 0;

#if defined(TIOCGWINSZ)
	/* if we have TIOCGWINSZ, find the window size with it */
	struct winsize sz;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &sz) == 0 &&
			sz.ws_col && sz.ws_row) {
		size[0] = sz.ws_col;
		size[1] = sz.ws_row;
		return 0;
	}
#endif /* TIOCGWINSZ */

	/*
	 * if that failed or we don't have it, fall back to using
	 * escape sequences
	 */
	if (write(STDOUT_FILENO, "\033[9999;9999H\033[6n", 16) != 16)
		return -1;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	timeout.tv_sec = 0;
	timeout.tv_usec = RESIZE_FALLBACK_MS * 1000;

	if (select(1, &rfds, NULL, NULL, &timeout) < 1)
		return -1;

	for (; i < sizeof(buf) && c != 'R'; ++i) {
		if (read(STDIN_FILENO, &c, 1) < 0)
			return -1;
		buf[i] = c;
	}
	if (i < 2 || i == sizeof(buf))
		return -1;
	buf[i] = '\0';
	if (sscanf(buf, "\033[%d;%dR", &size[1], &size[0]) != 2)
		return -1;
	return 0;
}

static int
try_read_chr(char *c)
{
	if (read(STDIN_FILENO, c, 1) < 0) {
		if (errno == EAGAIN)
			return 0;
		else
			die("read:");
	}
	return 1;
}

#if defined(SIGWINCH)
static void
winch(int unused)
{
	(void)unused;
	win_resized = 1;
}
#endif /* SIGWINCH */

/*
 * ===========================================================================
 * strings
 */
static void
str_insertchar(struct str *str, char c, size_t index)
{
	if (str->len + 1 >= str->size) {
		/* if the string is too small, increase its size */
		str->size += ROW_SIZE_INCREMENT;
		str->s = erealloc(str->s, str->size);
	}
	if (index > str->len)
		index = str->len;

	if (str->len == index) {
		/* if we need to insert at the end, simply do that */
		str->s[index] = c;
		str->s[index + 1] = '\0';
		++str->len;
	} else {
		/*
		 * if we need to insert elsewhere, move the portion
		 * of the string beginning from the index forwards
		 * to make room for the character
		 */
		memmove(str->s + index + 1, str->s + index,
				str->len - index + 1);
		str->s[index] = c;
		++str->len;
	}
}

static void
str_removechar(struct str *str, size_t index)
{
	if (str->len == 0)
		return;

	if (index > str->len)
		index = str->len;

	if (str->len - 1 == index) {
		/* if we need to remove at the end, just do that */
		str->s[index] = '\0';
		--str->len;
	} else {
		/*
		 * if we need to remove elsewhere, shift the portion
		 * of the string beginning from (index + 1) backwards
		 */
		memmove(str->s + index, str->s + index + 1, str->len - index);
		--str->len;
	}
}
/*
 * ===========================================================================
 * buffer management
 */
static void
buf_char_insert(struct buf *buf, size_t elem, char c, size_t index)
{
	if (!buf->b[elem]) {
		buf->b[elem] = emalloc(sizeof(struct str));
		buf->b[elem]->s = emalloc(INITIAL_ROW_SIZE);
		buf->b[elem]->s[0] = c;
		buf->b[elem]->s[1] = '\0';
		buf->b[elem]->len = 1;
		buf->b[elem]->size = INITIAL_ROW_SIZE;
	} else {
		str_insertchar(buf->b[elem], c, index);
	}
}

static void
buf_char_remove(struct buf *buf, size_t elem, size_t index)
{
	if (!buf->b[elem]) {
		buf->b[elem] = emalloc(sizeof(struct str));
		buf->b[elem]->s = emalloc(INITIAL_ROW_SIZE);
		buf->b[elem]->s[0] = '\0';
		buf->b[elem]->len = 0;
		buf->b[elem]->size = INITIAL_ROW_SIZE;
	} else {
		str_removechar(buf->b[elem], index);
	}
}

static void
buf_create(struct buf *buf, size_t size)
{
	buf->b = ecalloc(size, sizeof(struct str *));
	buf->size = size;
}

static size_t
buf_elem_len(struct buf *buf, size_t elem)
{
	return (buf->b[elem]) ? buf->b[elem]->len : 0;
}

static void
buf_free(const struct buf *buf)
{
	size_t i = 0;
	for (; i < buf->size; ++i) {
		if (buf->b[i]) {
			free(buf->b[i]->s);
			free(buf->b[i]);
		}
	}
	free(buf->b);
}

static void
buf_resize(struct buf *buf, size_t size)
{
	if (size != buf->size) {
		size_t oldsize = buf->size;
		if (size < oldsize) {
			size_t i = oldsize - 1;
			for (; i >= size; --i) {
				if (buf->b[i]) {
					free(buf->b[i]->s);
					free(buf->b[i]);
				}
			}
		}

		buf->b = ereallocarray(buf->b, size, sizeof(struct str *));
		buf->size = size;

		if (size > oldsize) {
			size_t i = oldsize;
			for (; i < size; ++i) {
				buf->b[i] = NULL;
			}
		}
	}
}

/*
 * ===========================================================================
 * main program loop
 */
static void
run(void)
{
	/* the main buffer; this is where the text is stored. */
	struct buf buf;

	/* window dimensions and the cursor's current position. */
	int size[2];
	int x = 0, y = 0;

	/* current event. */
	struct term_event ev;

	/* if this is true, the loop will finish. */
	int done = 0;

	size_t i = 0; /* DEBUG */

	buf_create(&buf, INITIAL_BUFFER_ROWS);
	if (term_size(size) < 0) {
		size[0] = FALLBACK_WIDTH;
		size[1] = FALLBACK_HEIGHT;
	}
	term_set_cursor(0, 0);

	while (!done) {
		term_event_wait(&ev);

		switch (ev.type) {
#if defined(SIGWINCH)
		case TERM_EVENT_RESIZE:
			if (term_size(size) < 0) {
				size[0] = FALLBACK_WIDTH;
				size[1] = FALLBACK_HEIGHT;
			}

			/*
			 * check if current cursor position is now
			 * outside the window, might happen if it
			 * was resized to be smaller
			 */
			if (x >= size[0] - 1)
				x = size[0] - 1;
			if (y >= size[1] - 1)
				y = size[1] - 1;

			break;
#endif /* SIGWINCH */
		case TERM_EVENT_KEY:
			switch (ev.key) {
			case KEY_ESC:
				/* quit */
				done = 1;
				break;
			case KEY_ARROW_UP:
				/* move cursor up */
				if (y > 0) {
					size_t elen = buf_elem_len(&buf,
							(size_t)--y);
					if ((size_t)x > elen)
						x = (int)elen;
					term_set_cursor(x, y);
				}
				break;
			case KEY_ARROW_DOWN:
				/* move cursor down */
				if (y < size[1] - 1) {
					size_t elen = buf_elem_len(&buf,
							(size_t)++y);
					if ((size_t)x > elen)
						x = (int)elen;
					term_set_cursor(x, y);
				}
				break;
			case KEY_ARROW_RIGHT:
				/* move cursor right */
				if (x < size[0] - 1 &&
					(size_t)x < buf_elem_len(&buf,
						(size_t)y))
					term_set_cursor(++x, y);
				break;
			case KEY_ARROW_LEFT:
				/* move cursor left */
				if (x > 0)
					term_set_cursor(--x, y);
				break;
			case KEY_ENTER:
				/* go to next row */
				if (y < size[1] - 1) {
					x = 0;
					term_set_cursor(x, ++y);
				}
				break;
			case KEY_BACKSPACE:
				/*
				 * remove char behind cursor, if it's not
				 * at the beginning of the row and there's
				 * some text on the current row
				 */
				if (x > 0 && buf.b[y] && buf.b[y]->len) {
					buf_char_remove(&buf, (size_t)(y),
							(size_t)(x - 1));
					term_print(0, y, COLOR_DEFAULT,
							buf.b[y]->s);
					term_set_cursor(--x, y);
				}
				break;
			case KEY_DELETE:
				/*
				 * remove char at cursor, if there's some
				 * text on the current row
				 */
				if (buf.b[y] && buf.b[y]->len) {
					buf_char_remove(&buf, (size_t)y,
							(size_t)x);
					term_print(0, y, COLOR_DEFAULT,
							buf.b[y]->s);
					term_set_cursor(x, y);
				}
				break;
			case KEY_CHAR:
				/* regular key */
				if (x < size[0] - 1) {
					if ((size_t)y >= buf.size)
						buf_resize(&buf,
							(size_t)ROUNDUPTO(y,
							BUF_SIZE_INCREMENT));
					buf_char_insert(&buf, (size_t)y,
							ev.ch, (size_t)x);
					term_print(0, y, COLOR_DEFAULT,
							buf.b[y]->s);
					term_set_cursor(++x, y);
				}
				break;
			}
		}
	}

	/* DEBUG */
	term_shutdown();
	for (i = 0; i < buf.size; ++i) {
		if (buf.b[i]) {
			printf("%lu | %s\n", (unsigned long)i, buf.b[i]->s);
		} else {
			printf("%lu ~\n", (unsigned long)i);
		}
	}
	buf_free(&buf);
	exit(0);
}

/*
 * ===========================================================================
 * main()
 */
int
main(int argc, char *argv[])
{
	if (argc)
		argv0 = argv[0];

	term_init();
	run();
	term_shutdown();
	return 0;
}
