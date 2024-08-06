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
 * - pressing enter won't actually insert a new line, fix this
 * - scrolling
 * - opening files
 * - optimize certain movements to use less escape sequences
 * - use select() in try_read_char to allow parsing of cursor key sequences
 *   if there's a delay between the characters
 * - optimize memory usage on large files
 * - UTF-8 support
 * - maybe try to handle OOM more gracefully instead of exiting instantly
 */

/*
 * ============================================================================
 * configurable macros
 */

/*
 * ===================
 * general
 */

/* enable usage of OpenBSD's pledge(2). 0 = false, 1 = true */
#define ENABLE_PLEDGE 0

/* mode for newly created files; will be modified by the process's umask(2) */
#define NEW_FILE_MODE 0666

/*
 * ===================
 * terminal
 */

/* if getting the terminal's size fails, the following size is used instead */
#define FALLBACK_WIDTH  80
#define FALLBACK_HEIGHT 24

/*
 * how long to wait for the terminal's response when getting the size with
 * the fallback method. cant be higher than 999
 */
#define RESIZE_FALLBACK_MS 500

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

/* how many iovec structures to use when writing to a file */
#define IOV_SIZE            32

/*
 * ===================
 * commands
 */

/*
 * how many characters to initially allocate for the command
 * buffer, cant be 0
 */
#define INITIAL_CMD_SIZE    16

/*
 * how many characters to add to the command buffer's size when it's too
 * small, cant be 0
 */
#define CMD_SIZE_INCREMENT  16

/*
 * ============================================================================
 * compatibility with certain platforms
 */

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
	defined(__bsdi__) || defined(__DragonFly__) || defined(_SYSTYPE_BSD)
/* BSD needs _BSD_SOURCE for SIGWINCH and (on OpenBSD) pledge() */
#define _BSD_SOURCE
#endif /* BSD */

#if defined(__dietlibc__) && defined(__x86_64__)
/* needed to work around a bug in dietlibc */
#include <stdint.h>
typedef uint64_t __u64;
#endif /* __dietlibc__ && __x86_64__ */

/*
 * ============================================================================
 * includes
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/uio.h>

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
 * ============================================================================
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

enum mode {
	MODE_NORMAL,
	MODE_INSERT,
	MODE_COMMAND_LINE
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
	size_t len, size;
};

struct state {
	struct buf buf; /* the main buffer */
	struct str cmd; /* string used to hold commands */

	int size[2]; /* window dimensions */
	int x, y; /* cursor's current position */

	enum mode mode; /* current mode */
	int storedx; /* x position before entering command-line mode */

	char *name; /* name of file being edited */
	int name_needs_free; /* whether name should be free()'d */
	int modified; /* whether the buffer has unwritten changes */
	int written; /* whether we've written into a file once */

	struct term_event ev; /* current terminal event */
	int done; /* if this is true, the main loop will finish */
};

/*
 * ============================================================================
 * function declarations
 */

/* memory allocation */
static void *ecalloc(size_t nmemb, size_t size);
static void *emalloc(size_t size);
static void *erealloc(void *ptr, size_t size);
static void *ereallocarray(void *ptr, size_t nmemb, size_t size);
static char *estrdup(const char *s);

/* errors */
static void die(const char *fmt, ...);

/* terminal */
static void readkey(struct term_event *ev);
static void term_clear_row(int y);
static void term_event_wait(struct term_event *ev);
static void term_init(void);
static void term_print(int x, int y, const char *color, const char *str);
static void term_printf(int x, int y, const char *color, const char *fmt, ...);
static void term_set_cursor(int x, int y);
static void term_shutdown(void);
static int term_size(int size[2]);
static int try_read_chr(char *c);
#if defined(SIGWINCH)
static void winch(int unused);
#endif /* SIGWINCH */

/* strings */
static void str_insertchar(struct str *str, char c, size_t index,
		size_t size_increment);
static void str_removechar(struct str *str, size_t index);

/* buffer management */
static void buf_char_insert(struct buf *buf, size_t elem, char c,
		size_t index);
static void buf_char_remove(struct buf *buf, size_t elem, size_t index);
static void buf_create(struct buf *buf, size_t size);
static size_t buf_elem_len(struct buf *buf, size_t elem);
static void buf_free(const struct buf *buf);
static void buf_resize(struct buf *buf, size_t size);
static int buf_write(const struct buf *buf, const char *filename,
		int overwrite);
static int iov_write(struct iovec *iov, int *iovcnt, size_t iov_size,
		int writefd, char *str, size_t len);

/* movement */
static void cursor_up(struct state *st);
static void cursor_down(struct state *st);
static void cursor_right(struct state *st);
static void cursor_left(struct state *st);
static void cursor_start(struct state *st);
static void cursor_end(struct state *st);
static void cursor_startnextrow(struct state *st);

/* commands */
static const char *cmdarg(const char *cmd);
static int cmdchrcmp(const char *cmd, char c);
static int cmdstrcmp(const char *cmd, const char *s, size_t sl);
static int exec_cmd(struct state *st);

/* main program loop */
static void key_command_line(struct state *st);
static void key_insert(struct state *st);
static void key_normal(struct state *st);
static void run(int argc, char *argv[]);

/*
 * ============================================================================
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
 * ============================================================================
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

static char *
estrdup(const char *s)
{
	char *ret = strdup(s);
	if (!ret)
		die("strdup: out of memory");
	return ret;
}

/*
 * ============================================================================
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
 * ============================================================================
 * terminal
 */
static void
readkey(struct term_event *ev)
{
	/* read a key from stdin and write the data into a term_event. */
	char c;
	for (;;) {
		read(STDIN_FILENO, &c, 1);
		switch (c) {
		case '\033':
			/*
			 * <ESC>, might be the start of a multi-character
			 * key sequence
			 */
			if (try_read_chr(&c)) {
				/* atleast two characters long */
				if (c == '[' && try_read_chr(&c)) {
					/* <ESC>[<something> */
					switch (c) {
					case '3':
						/* <ESC>[3~ = delete */
						if (try_read_chr(&c) &&
								c == '~') {
							ev->key = KEY_DELETE;
							return;
						}
						break;
					case 'A':
						/* <ESC>[A = up arrow */
						ev->key = KEY_ARROW_UP;
						return;
					case 'B':
						/* <ESC>[B = down arrow */
						ev->key = KEY_ARROW_DOWN;
						return;
					case 'C':
						/* <ESC>[C = right arrow */
						ev->key = KEY_ARROW_RIGHT;
						return;
					case 'D':
						/* <ESC>[D = left arrow */
						ev->key = KEY_ARROW_LEFT;
						return;
					}
				}
			} else {
				/* it's just <ESC> */
				ev->key = KEY_ESC;
				return;
			}
			break;
		case '\r':
			/* carriage return = enter */
			ev->key = KEY_ENTER;
			return;
		case 127:
			/* <DEL> = backspace */
			ev->key = KEY_BACKSPACE;
			return;
		default:
			/* another key, currently only ASCII is supported */
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
term_clear_row(int y)
{
	/* clear the row at y-coordinate y. */
	if (y < 0)
		return;
	printf("\033[%d;H\033[2K", y + 1);
	fflush(stdout);
}

static void
term_event_wait(struct term_event *ev)
{
	/* wait for a terminal event (either resize or keypress). */
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
	/* initialize the terminal for use by the other term_* functions. */
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
	/*
	 * clear the row at y-coordinate y and print the string str at
	 * the location (x, y).
	 */
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
term_printf(int x, int y, const char *color, const char *fmt, ...)
{
	/* same as term_print, but printf. */
	va_list ap;

	if (x < 0 || y < 0)
		return;
	printf("\033[%d;%dH\033[2K", y + 1, x + 1);
	if (color)
		fputs(color, stdout);

	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);

	if (color)
		fputs(COLOR_RESET, stdout);
	fflush(stdout);
}

static void
term_set_cursor(int x, int y)
{
	/* set the cursor to the location (x, y). */
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
	/*
	 * get the terminal's size and store it in size:
	 * size[0] = width
	 * size[1] = height
	 *
	 * if getting the size fails, return -1; otherwise, return 0.
	 */
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
	/*
	 * try to read a character from stdin.
	 * if there's no data available on stdin, return 0.
	 * if there's data available, store the character in *c and return 1.
	 *
	 * requires stdin to be in non-blocking mode.
	 */
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
 * ============================================================================
 * strings
 */
static void
str_insertchar(struct str *str, char c, size_t index, size_t size_increment)
{
	/*
	 * insert the character c into the string str at the index index.
	 * if the string is too small to hold the extra character,
	 * increase its size by size_increment.
	 */
	if (str->len + 1 >= str->size) {
		/* if the string is too small, increase its size */
		str->size += size_increment;
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
	/* remove the character located at index index from the string str. */
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
 * ============================================================================
 * buffer management
 */
static void
buf_char_insert(struct buf *buf, size_t elem, char c, size_t index)
{
	/*
	 * insert a character into a specific element of a buffer,
	 * creating it if it doesn't exist.
	 */
	if (elem >= buf->size)
		buf_resize(buf, ROUNDUPTO(elem, BUF_SIZE_INCREMENT));
	if (elem >= buf->len)
		buf->len = elem + 1;
	if (!buf->b[elem]) {
		buf->b[elem] = emalloc(sizeof(struct str));
		buf->b[elem]->s = emalloc(INITIAL_ROW_SIZE);
		buf->b[elem]->s[0] = c;
		buf->b[elem]->s[1] = '\0';
		buf->b[elem]->len = 1;
		buf->b[elem]->size = INITIAL_ROW_SIZE;
	} else {
		str_insertchar(buf->b[elem], c, index, ROW_SIZE_INCREMENT);
	}
}

static void
buf_char_remove(struct buf *buf, size_t elem, size_t index)
{
	/* remove a character from a specific element of a buffer. */
	if (elem < buf->size && buf->b[elem])
		str_removechar(buf->b[elem], index);
}

static void
buf_create(struct buf *buf, size_t size)
{
	/* create a new buffer. */
	buf->b = ecalloc(size, sizeof(struct str *));
	buf->len = 0;
	buf->size = size;
}

static size_t
buf_elem_len(struct buf *buf, size_t elem)
{
	/*
	 * returns the length of an element of a buffer, or 0 if it
	 * doesn't exist.
	 */
	return (buf->b[elem]) ? buf->b[elem]->len : 0;
}

static void
buf_free(const struct buf *buf)
{
	/* free a buffer and all of its elements. */
	size_t i = 0;
	for (; i < buf->len; ++i) {
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
	/* resize a buffer. */
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
			if (buf->len > size) {
				buf->len = size - 1;
				while (buf->len && !buf->b[buf->len])
					--buf->len;
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

static int
buf_write(const struct buf *buf, const char *filename, int overwrite)
{
	/* write the contents of a buffer to a file. */
	struct iovec iov[IOV_SIZE];
	int fd;
	char newline = '\n';
	int iovcnt = 0; /* int since writev() takes an int for iovcnt */
	size_t i = 0;

	if (overwrite)
		fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC,
				NEW_FILE_MODE);
	else
		fd = open(filename, O_WRONLY | O_CREAT | O_EXCL,
				NEW_FILE_MODE);
	if (fd < 0)
		return -1;

	for (; i < buf->len; ++i) {
		if (buf->b[i]) {
			if (iov_write(iov, &iovcnt, IOV_SIZE, fd,
					buf->b[i]->s, buf->b[i]->len) < 0)
				return -1;
			if (iov_write(iov, &iovcnt, IOV_SIZE, fd,
					&newline, 1) < 0)
				return -1;
		} else if (iov_write(iov, &iovcnt, IOV_SIZE, fd,
					&newline, 1) < 0) {
			return -1;
		}
	}
	if (iovcnt && writev(fd, iov, iovcnt) < 0)
		return -1;
	return close(fd);
}

static int
iov_write(struct iovec *iov, int *iovcnt, size_t iov_size, int writefd,
		char *str, size_t len)
{
	/*
	 * add an entry to an array of iovec structures, or flush the
	 * data to a file if there's no space remaining in the array.
	 */
	if ((size_t)*iovcnt >= iov_size) {
		if (writev(writefd, iov, *iovcnt) < 0)
			return -1;
		*iovcnt = 0;
	}
	iov[*iovcnt].iov_base = str;
	iov[(*iovcnt)++].iov_len = len;
	return 0;
}

/*
 * ============================================================================
 * movement
 */
static void
cursor_up(struct state *st)
{
	if (st->y > 0) {
		size_t elen = buf_elem_len(&st->buf, (size_t)--(st->y));
		if ((size_t)st->x > elen)
			st->x = (int)elen;
		term_set_cursor(st->x, st->y);
	}
}

static void
cursor_down(struct state *st)
{
	/*
	 * height is subtracted by 2 since we have to reserve the
	 * last row for the command text or mode indicator
	 */
	if (st->y < st->size[1] - 2) {
		size_t elen = buf_elem_len(&st->buf, (size_t)++st->y);
		if ((size_t)st->x > elen)
			st->x = (int)elen;
		term_set_cursor(st->x, st->y);
	}
}

static void
cursor_right(struct state *st)
{
	if (st->x < st->size[0] - 1 && (size_t)st->x < buf_elem_len(&st->buf,
				(size_t)st->y))
		term_set_cursor(++st->x, st->y);
}

static void
cursor_left(struct state *st)
{
	if (st->x > 0)
		term_set_cursor(--(st->x), st->y);
}

static void
cursor_start(struct state *st)
{
	st->x = 0;
	term_set_cursor(st->x, st->y);
}

static void
cursor_end(struct state *st)
{
	size_t l = buf_elem_len(&st->buf, (size_t)st->y);
	if (l)
		--l;
	st->x = (int)l;
	term_set_cursor(st->x, st->y);
}

static void
cursor_startnextrow(struct state *st)
{
	/* same as earlier, subtract 2 to reserve one row */
	if (st->y < st->size[1] - 2) {
		st->x = 0;
		term_set_cursor(st->x, ++st->y);
	}
}

static void
cursor_endpreviousrow(struct state *st)
{
	if (st->y > 0) {
		st->x = (int)buf_elem_len(&st->buf, (size_t)--(st->y));
		term_set_cursor(st->x, st->y);
	}
}

/*
 * ============================================================================
 * commands
 */
static const char *
cmdarg(const char *cmd)
{
	/*
	 * get the argument portion of a command.
	 * returns NULL if there isn't one.
	 */

	/*
	 * if there's a space and the char afterwards isn't a '\0',
	 * return a pointer to that char
	 */
	const char *p = strchr(cmd, ' ');
	return (p && *(++p)) ? p : NULL;
}

static int
cmdchrcmp(const char *cmd, char c)
{
	/*
	 * check if a command (not counting the argument portion) equals to
	 * a character with an optional bang at the end.
	 *
	 * for example, cmdchrcmp(cmd, 'q') will return true for "q", "q!",
	 * "q " followed by anything, or "q! " followed by anything.
	 */
	size_t end = 1;

	/* if the command doesn't start with c, return false */
	if (cmd[0] != c)
		return 0;

	/* if there's a bang, count it as part of the command */
	if (cmd[end] == '!')
		++end;

	/* if the end of the command isn't a '\0' or a ' ', return false */
	if (cmd[end] != '\0' && cmd[end] != ' ')
		return 0;
	return 1;
}

static int
cmdstrcmp(const char *cmd, const char *s, size_t sl)
{
	/* same as cmdchrcmp(), but compare against a string. */
	size_t end = sl;

	/* if the command doesn't start with s, return false */
	if (strncmp(cmd, s, sl))
		return 0;

	/* if there's a bang, count it as part of the command */
	if (cmd[end] == '!')
		++end;

	/* if the end of the command isn't a '\0' or a ' ', return false */
	if (cmd[end] != '\0' && cmd[end] != ' ')
		return 0;
	return -1;
}

static int
exec_cmd(struct state *st)
{
	/* execute a command. returns 0 on success and -1 on error. */
	if (cmdchrcmp(st->cmd.s, 'q')) {
		/* :q || :q! */
		if (st->cmd.s[1] != '!' && st->modified) {
			term_print(0, st->size[1] - 1, COLOR_RED,
					"buffer modified");
			return -1;
		}
		st->done = 1;
	} else if (cmdchrcmp(st->cmd.s, 'w') || cmdstrcmp(st->cmd.s, "wq",
				2)) {
		/* :w || :w! || :wq || :wq! */
		const char *arg = cmdarg(st->cmd.s);
		const char *name = (arg) ? arg : st->name;
		int bang = (st->cmd.s[1] == '!' || (st->cmd.s[1] == 'q' &&
					st->cmd.s[2] == '!'));

		if (arg && !st->name) {
			st->name = estrdup(arg);
			st->name_needs_free = 1;
		}
		if (name) {
			if (buf_write(&st->buf, name,
						bang || st->written) < 0) {
				if (errno == EEXIST) {
					term_print(0, st->size[1] - 1,
							COLOR_RED,
							"file exists (add ! "
							"to override)");
				} else {
					term_printf(0, st->size[1] - 1,
							COLOR_RED,
							"writing to file "
							"failed: %s",
							strerror(errno));
				}
				return -1;
			}
			st->modified = 0;
			st->written = 1;
		} else {
			term_print(0, st->size[1] - 1, COLOR_RED,
					"no file name specified");
			return -1;
		}
		if (st->cmd.s[1] == 'q')
			st->done = 1;
	}
	return 0;
}

/*
 * ============================================================================
 * main program loop
 */
static void
key_command_line(struct state *st)
{
	/* handle a key event in command-line mode. */
	switch (st->ev.key) {
	case KEY_ESC:
		/* discard command and return to normal mode */
		st->mode = MODE_NORMAL;
		st->cmd.s[0] = '\0';
		st->cmd.len = 0;
		term_clear_row(st->size[1] - 1);
		st->x = st->storedx;
		term_set_cursor(st->x, st->y);
		break;
	case KEY_ARROW_RIGHT:
		/* move cursor right */
		if (st->x < st->size[0] - 1 && (size_t)(st->x - 1) <
				st->cmd.len)
			term_set_cursor(++st->x, st->size[1] - 1);
		break;
	case KEY_ARROW_LEFT:
		/* move cursor left */
		if (st->x > 1)
			term_set_cursor(--(st->x), st->size[1] - 1);
		break;
	case KEY_ENTER:
		/* execute command and return to normal mode */
		if (exec_cmd(st) >= 0)
			term_clear_row(st->size[1] - 1);
		st->mode = MODE_NORMAL;
		st->cmd.s[0] = '\0';
		st->cmd.len = 0;
		st->x = st->storedx;
		term_set_cursor(st->x, st->y);
		break;
	case KEY_BACKSPACE:
		/*
		 * remove char behind cursor, if it's not
		 * at the beginning of the row and there's
		 * some text on the current row
		 */
		if (st->x > 1 && st->cmd.len) {
			str_removechar(&st->cmd, (size_t)(st->x - 2));
			term_printf(0, st->size[1] - 1, COLOR_DEFAULT,
					":%s", st->cmd.s);
			term_set_cursor(--(st->x), st->size[1] - 1);
		}
		break;
	case KEY_DELETE:
		/*
		 * remove char at cursor, if there's some
		 * text on the current row
		 */
		if (st->cmd.len) {
			str_removechar(&st->cmd, (size_t)(st->x - 1));
			term_printf(0, st->size[1] - 1, COLOR_DEFAULT,
					":%s", st->cmd.s);
			term_set_cursor(st->x, st->size[1] - 1);
		}
		break;
	case KEY_CHAR:
		/* regular key */
		if (st->x > 0 && st->x < st->size[0] - 1) {
			str_insertchar(&st->cmd, st->ev.ch,
					(size_t)(st->x - 1),
					CMD_SIZE_INCREMENT);
			term_printf(0, st->size[1] - 1, COLOR_DEFAULT,
					":%s", st->cmd.s);
			term_set_cursor(++st->x, st->size[1] - 1);
		}
		break;
	default:
		break;
	}
}

static void
key_insert(struct state *st)
{
	/* handle a key event in insert mode. */
	switch (st->ev.key) {
	case KEY_ESC:
		/* go into normal mode */
		st->mode = MODE_NORMAL;
		term_clear_row(st->size[1] - 1);
		term_set_cursor(st->x, st->y);
		break;
	case KEY_ARROW_UP:
		cursor_up(st);
		break;
	case KEY_ARROW_DOWN:
		cursor_down(st);
		break;
	case KEY_ARROW_RIGHT:
		cursor_right(st);
		break;
	case KEY_ARROW_LEFT:
		cursor_left(st);
		break;
	case KEY_ENTER:
		cursor_startnextrow(st);
		break;
	case KEY_BACKSPACE:
		/*
		 * remove char behind cursor, if it's not
		 * at the beginning of the row and there's
		 * some text on the current row
		 */
		if (st->x > 0 && st->buf.b[st->y] &&
				st->buf.b[st->y]->len) {
			st->modified = 1;
			buf_char_remove(&st->buf, (size_t)st->y,
					(size_t)(st->x - 1));
			term_print(0, st->y, COLOR_DEFAULT,
					st->buf.b[st->y]->s);
			term_set_cursor(--(st->x), st->y);
		}
		break;
	case KEY_DELETE:
		/*
		 * remove char at cursor, if there's some
		 * text on the current row
		 */
		if (st->buf.b[st->y] && st->buf.b[st->y]->len) {
			st->modified = 1;
			buf_char_remove(&st->buf, (size_t)st->y,
					(size_t)st->x);
			term_print(0, st->y, COLOR_DEFAULT,
					st->buf.b[st->y]->s);
			term_set_cursor(st->x, st->y);
		}
		break;
	case KEY_CHAR:
		/* regular key */
		if (st->x < st->size[0] - 1) {
			st->modified = 1;
			buf_char_insert(&st->buf, (size_t)st->y, st->ev.ch,
					(size_t)st->x);
			term_print(0, st->y, COLOR_DEFAULT,
					st->buf.b[st->y]->s);
			term_set_cursor(++st->x, st->y);
		}
		break;
	default:
		break;
	}
}

static void
key_normal(struct state *st)
{
	/* handle a key event in normal mode. */
	switch (st->ev.key) {
	case KEY_ARROW_UP:
		cursor_up(st);
		break;
	case KEY_ARROW_DOWN:
		cursor_down(st);
		break;
	case KEY_ARROW_RIGHT:
		cursor_right(st);
		break;
	case KEY_ARROW_LEFT:
		cursor_left(st);
		break;
	case KEY_ENTER:
		cursor_startnextrow(st);
		break;
	case KEY_BACKSPACE:
		/* move to previous char */
		if (st->x == 0 && st->y > 0)
			cursor_endpreviousrow(st);
		else
			cursor_left(st);
		break;
	case KEY_CHAR:
		switch (st->ev.ch) {
		case 'h':
			cursor_left(st);
			break;
		case 'j':
			cursor_down(st);
			break;
		case 'k':
			cursor_up(st);
			break;
		case 'l':
			cursor_right(st);
			break;
		case '0':
			cursor_start(st);
			break;
		case '$':
			cursor_end(st);
			break;
		case 'i':
			st->mode = MODE_INSERT;
			term_print(0, st->size[1] - 1, COLOR_DEFAULT,
					"INSERT");
			term_set_cursor(st->x, st->y);
			break;
		case 'a':
			cursor_right(st);
			st->mode = MODE_INSERT;
			term_print(0, st->size[1] - 1, COLOR_DEFAULT,
					"INSERT");
			term_set_cursor(st->x, st->y);
			break;
		case ':':
			st->mode = MODE_COMMAND_LINE;
			st->storedx = st->x;
			st->x = 1;
			term_print(0, st->size[1] - 1, COLOR_DEFAULT,
					":");
			term_set_cursor(st->x, st->size[1] - 1);
			break;
		}
	default:
		break;
	}
}

static void
run(int argc, char *argv[])
{
	/* main program loop. */
	struct state st;

	/* initialize state */
	buf_create(&st.buf, INITIAL_BUFFER_ROWS);

	st.cmd.s = emalloc(INITIAL_CMD_SIZE);
	st.cmd.s[0] = '\0';
	st.cmd.len = 0;
	st.cmd.size = INITIAL_ROW_SIZE;

	st.x = 0;
	st.y = 0;
	st.mode = MODE_NORMAL;
	st.storedx = 0;
	st.name = (argc > 1) ? argv[1] : NULL;
	st.name_needs_free = 0;
	st.modified = 0;
	st.written = 0;
	st.done = 0;

	/* get terminal size */
	if (term_size(st.size) < 0) {
		st.size[0] = FALLBACK_WIDTH;
		st.size[1] = FALLBACK_HEIGHT;
	}
	if (st.size[1] < 2)
		die("terminal height too low");

	term_set_cursor(0, 0);

	/* main loop */
	while (!st.done) {
		term_event_wait(&st.ev);

		switch (st.ev.type) {
#if defined(SIGWINCH)
		case TERM_EVENT_RESIZE:
			if (term_size(st.size) < 0) {
				st.size[0] = FALLBACK_WIDTH;
				st.size[1] = FALLBACK_HEIGHT;
			}
			if (st.size[1] < 2)
				die("terminal height too low");

			/*
			 * check if current cursor position is now
			 * outside the window, might happen if it
			 * was resized to be smaller
			 */
			if (st.x >= st.size[0] - 1)
				st.x = st.size[0] - 1;
			if (st.y >= st.size[1] - 1)
				st.y = st.size[1] - 1;

			break;
#endif /* SIGWINCH */
		case TERM_EVENT_KEY:
			if (st.mode == MODE_COMMAND_LINE)
				key_command_line(&st);
			else if (st.mode == MODE_INSERT)
				key_insert(&st);
			else if (st.mode == MODE_NORMAL)
				key_normal(&st);
			break;
		}
	}

	if (st.name_needs_free)
		free(st.name);
	free(st.cmd.s);
	buf_free(&st.buf);
}

/*
 * ============================================================================
 * main()
 */
int
main(int argc, char *argv[])
{
	if (argc)
		argv0 = argv[0];

#if ENABLE_PLEDGE
        if (pledge("stdio rpath wpath cpath tty", NULL) < 0)
		die("pledge:");
#endif /* ENABLE_PLEDGE */

	term_init();
	run(argc, argv);
	term_shutdown();
	return 0;
}
