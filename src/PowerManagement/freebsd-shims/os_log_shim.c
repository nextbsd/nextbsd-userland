/*
 * os_log_shim.c — the runtime half of freebsd-shims/os/log.h.
 *
 * Formats an os_log() message and writes one line to stderr. The format is
 * walked one conversion at a time, because two of os_log's extensions to
 * printf cannot be passed to vsnprintf:
 *
 *   %{...}x   a privacy or type annotation ({public}, {private}, {bool},
 *             {errno}, {time_t}, ...). It is dropped and the conversion is
 *             printed as plain printf would print it.
 *   %@        a CFTypeRef. A CFString is printed as UTF-8, anything else
 *             through CFCopyDescription(), NULL as "(null)".
 *
 * Every other conversion is rebuilt as a single-conversion printf spec, with
 * any '*' width or precision resolved to a number, and its argument is read
 * with the type its length modifier names, so the va_list stays in step.
 * %P (os_log's binary data) prints as "<data>", and %n is ignored.
 *
 * A message longer than PM_LOG_LINE_MAX is truncated.
 */
#include <os/log.h>

#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define	PM_LOG_LINE_MAX		1024
#define	PM_LOG_SPEC_MAX		32

struct pm_os_log_s {
	char	category[64];
};

struct pm_logbuf {
	char	*buf;
	size_t	 len;
	size_t	 cap;
};

static void
lb_puts(struct pm_logbuf *lb, const char *s, size_t n)
{
	size_t room;

	if (lb->len + 1 >= lb->cap)
		return;
	room = lb->cap - 1 - lb->len;
	if (n > room)
		n = room;
	memcpy(lb->buf + lb->len, s, n);
	lb->len += n;
	lb->buf[lb->len] = '\0';
}

static void
lb_printf(struct pm_logbuf *lb, const char *spec, ...)
{
	char tmp[PM_LOG_LINE_MAX];
	va_list ap;
	int n;

	va_start(ap, spec);
	n = vsnprintf(tmp, sizeof(tmp), spec, ap);
	va_end(ap);
	if (n > 0)
		lb_puts(lb, tmp, strlen(tmp));
}

static void
lb_cftype(struct pm_logbuf *lb, CFTypeRef obj)
{
	CFStringRef desc;
	char tmp[PM_LOG_LINE_MAX];

	if (obj == NULL) {
		lb_puts(lb, "(null)", 6);
		return;
	}
	if (CFGetTypeID(obj) == CFStringGetTypeID())
		desc = CFRetain(obj);
	else
		desc = CFCopyDescription(obj);
	if (desc == NULL) {
		lb_puts(lb, "(null)", 6);
		return;
	}
	if (CFStringGetCString(desc, tmp, sizeof(tmp), kCFStringEncodingUTF8))
		lb_puts(lb, tmp, strlen(tmp));
	else
		lb_puts(lb, "<?>", 3);
	CFRelease(desc);
}

/*
 * Format one message. The va_list is passed by pointer so each conversion
 * consumes its argument from the caller's list.
 */
static void
pm_log_format(struct pm_logbuf *lb, const char *fmt, va_list *ap,
    int saved_errno)
{
	const char *p = fmt;

	while (*p != '\0') {
		char spec[PM_LOG_SPEC_MAX];
		size_t sl = 0;
		const char *lit = p;
		int lmod = 0;		/* 'H' hh, 'h', 'l', 'q' ll, 'j', 'z', 't', 'L' */
		char conv;

		while (*p != '\0' && *p != '%')
			p++;
		if (p != lit)
			lb_puts(lb, lit, (size_t)(p - lit));
		if (*p == '\0')
			break;

		/* *p == '%' */
		p++;
		if (*p == '%') {
			lb_puts(lb, "%", 1);
			p++;
			continue;
		}

		/* os_log annotation: %{public}s, %{private, mask.hash}@ ... */
		if (*p == '{') {
			const char *close = strchr(p, '}');

			if (close == NULL)
				break;
			p = close + 1;
		}

		spec[sl++] = '%';

		/* flags */
		while (*p != '\0' && strchr("-+ #0'", *p) != NULL) {
			if (sl < sizeof(spec) - 16)
				spec[sl++] = *p;
			p++;
		}

		/* width */
		if (*p == '*') {
			int w = va_arg(*ap, int);

			sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl,
			    "%d", w);
			p++;
		} else {
			while (*p >= '0' && *p <= '9') {
				if (sl < sizeof(spec) - 12)
					spec[sl++] = *p;
				p++;
			}
		}

		/* precision */
		if (*p == '.') {
			spec[sl++] = '.';
			p++;
			if (*p == '*') {
				int pr = va_arg(*ap, int);

				sl += (size_t)snprintf(spec + sl,
				    sizeof(spec) - sl, "%d", pr < 0 ? 0 : pr);
				p++;
			} else {
				while (*p >= '0' && *p <= '9') {
					if (sl < sizeof(spec) - 8)
						spec[sl++] = *p;
					p++;
				}
			}
		}

		/* length modifier */
		switch (*p) {
		case 'h':
			p++;
			if (*p == 'h') {
				lmod = 'H';
				p++;
			} else
				lmod = 'h';
			break;
		case 'l':
			p++;
			if (*p == 'l') {
				lmod = 'q';
				p++;
			} else
				lmod = 'l';
			break;
		case 'q':
		case 'j':
		case 'z':
		case 't':
		case 'L':
			lmod = *p++;
			break;
		default:
			break;
		}

		conv = *p;
		if (conv == '\0')
			break;
		p++;

		/* Re-emit the length modifier into the spec. */
		switch (lmod) {
		case 'H':
			spec[sl++] = 'h';
			spec[sl++] = 'h';
			break;
		case 'q':
			spec[sl++] = 'l';
			spec[sl++] = 'l';
			break;
		case 0:
			break;
		default:
			spec[sl++] = (char)lmod;
			break;
		}
		spec[sl++] = conv;
		spec[sl] = '\0';

		switch (conv) {
		case '@':
			lb_cftype(lb, va_arg(*ap, CFTypeRef));
			break;
		case 'd':
		case 'i':
			switch (lmod) {
			case 'l':
				lb_printf(lb, spec, va_arg(*ap, long));
				break;
			case 'q':
				lb_printf(lb, spec, va_arg(*ap, long long));
				break;
			case 'j':
				lb_printf(lb, spec, va_arg(*ap, intmax_t));
				break;
			case 'z':
				lb_printf(lb, spec, va_arg(*ap, ssize_t));
				break;
			case 't':
				lb_printf(lb, spec, va_arg(*ap, ptrdiff_t));
				break;
			default:
				lb_printf(lb, spec, va_arg(*ap, int));
				break;
			}
			break;
		case 'o':
		case 'u':
		case 'x':
		case 'X':
			switch (lmod) {
			case 'l':
				lb_printf(lb, spec, va_arg(*ap, unsigned long));
				break;
			case 'q':
				lb_printf(lb, spec,
				    va_arg(*ap, unsigned long long));
				break;
			case 'j':
				lb_printf(lb, spec, va_arg(*ap, uintmax_t));
				break;
			case 'z':
				lb_printf(lb, spec, va_arg(*ap, size_t));
				break;
			case 't':
				lb_printf(lb, spec, va_arg(*ap, ptrdiff_t));
				break;
			default:
				lb_printf(lb, spec, va_arg(*ap, unsigned int));
				break;
			}
			break;
		case 'c':
			lb_printf(lb, spec, va_arg(*ap, int));
			break;
		case 's': {
			const char *s = va_arg(*ap, const char *);

			if (lmod == 'l') {
				/* %ls: not used by pmconfigd; print nothing */
				break;
			}
			lb_printf(lb, spec, s != NULL ? s : "(null)");
			break;
		}
		case 'p':
			lb_printf(lb, spec, va_arg(*ap, void *));
			break;
		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
			if (lmod == 'L')
				lb_printf(lb, spec, va_arg(*ap, long double));
			else
				lb_printf(lb, spec, va_arg(*ap, double));
			break;
		case 'P':
			(void)va_arg(*ap, void *);
			lb_puts(lb, "<data>", 6);
			break;
		case 'n':
			(void)va_arg(*ap, void *);
			break;
		case 'm':
			lb_printf(lb, "%s", strerror(saved_errno));
			break;
		default:
			/* Unknown conversion: print it literally, consume nothing. */
			lb_puts(lb, spec, strlen(spec));
			break;
		}
	}
}

os_log_t
_pm_os_log_create(const char *subsystem, const char *category)
{
	struct pm_os_log_s *log;

	(void)subsystem;
	log = calloc(1, sizeof(*log));
	if (log == NULL)
		return (OS_LOG_DEFAULT);
	if (category != NULL)
		(void)strlcpy(log->category, category, sizeof(log->category));
	return (log);
}

void
_pm_os_log(os_log_t log, os_log_type_t type, const char *fmt, ...)
{
	char line[PM_LOG_LINE_MAX];
	struct pm_logbuf lb = { line, 0, sizeof(line) };
	const char *tag;
	char tbuf[32];
	struct tm tm;
	time_t now;
	va_list ap;
	int saved_errno = errno;

	if (log == OS_LOG_DISABLED || fmt == NULL)
		return;
	line[0] = '\0';

	switch (type) {
	case OS_LOG_TYPE_ERROR:
		tag = "error: ";
		break;
	case OS_LOG_TYPE_FAULT:
		tag = "fault: ";
		break;
	default:
		tag = "";
		break;
	}

	now = time(NULL);
	if (localtime_r(&now, &tm) == NULL ||
	    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm) == 0)
		tbuf[0] = '\0';

	va_start(ap, fmt);
	pm_log_format(&lb, fmt, &ap, saved_errno);
	va_end(ap);

	/* Apple's messages often end in "\n" already; print exactly one. */
	while (lb.len > 0 && line[lb.len - 1] == '\n')
		line[--lb.len] = '\0';

	if (log != OS_LOG_DEFAULT && log->category[0] != '\0')
		(void)fprintf(stderr, "%s pmconfigd[%d] [%s] %s%s\n", tbuf,
		    (int)getpid(), log->category, tag, line);
	else
		(void)fprintf(stderr, "%s pmconfigd[%d] %s%s\n", tbuf,
		    (int)getpid(), tag, line);
	(void)fflush(stderr);
	errno = saved_errno;
}
