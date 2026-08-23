/* Nintendo 64 implementation of the <base/system.h> subset that the
 * deterministic Teeworlds simulation actually links against.
 *
 * src/base/system.c is POSIX-only (sockets, pthreads, dirent, gettimeofday),
 * so it is deliberately NOT compiled for the target. Every routine below is a
 * byte-for-byte port of the corresponding host implementation so that shared
 * engine/game code observes identical semantics; only the OS-facing bottom
 * (file IO, clock, logging, directory queries) is retargeted:
 *
 *   - io_*     -> newlib stdio over libdragon DragonFS ("rom:/").
 *   - time_*   -> libdragon microsecond tick counter.
 *   - dbg_msg  -> stderr, which the debug channels forward to IS Viewer.
 *   - fs_*     -> read-only stubs; the DFS image has no writable tree.
 *
 * Nothing here is reachable from the host build.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug.h>
#include <n64sys.h>

#include <base/system.h>

/* ------------------------------------------------------------------ */
/* Debug                                                              */
/* ------------------------------------------------------------------ */

void dbg_break(void)
{
	__builtin_trap();
}

void dbg_assert_imp(const char *filename, int line, int test, const char *msg)
{
	if(!test)
	{
		dbg_msg("assert", "%s(%d): %s", filename, line, msg);
		dbg_break();
	}
}

void dbg_msg(const char *sys, const char *fmt, ...)
{
	va_list args;
	fprintf(stderr, "[%s]: ", sys);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

void dbg_logger(DBG_LOGGER logger, DBG_LOGGER_FINISH finish, void *user)
{
	(void)logger;
	(void)finish;
	(void)user;
}

void dbg_logger_stdout(void) {}
void dbg_logger_debugger(void) {}
void dbg_logger_file(IOHANDLE logfile) { (void)logfile; }

/* ------------------------------------------------------------------ */
/* Memory                                                             */
/* ------------------------------------------------------------------ */

void *mem_alloc(unsigned size)
{
	return malloc(size);
}

void mem_free(void *p)
{
	free(p);
}

void mem_copy(void *dest, const void *source, unsigned size)
{
	memcpy(dest, source, size);
}

void mem_move(void *dest, const void *source, unsigned size)
{
	memmove(dest, source, size);
}

void mem_zero(void *block, unsigned size)
{
	memset(block, 0, size);
}

int mem_comp(const void *a, const void *b, int size)
{
	return memcmp(a, b, size);
}

int mem_has_null(const void *block, unsigned size)
{
	const unsigned char *bytes = (const unsigned char *)block;
	unsigned i;
	for(i = 0; i < size; i++)
	{
		if(bytes[i] == 0)
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* File IO over DragonFS                                              */
/* ------------------------------------------------------------------ */

/* The engine hands us checkout-relative paths such as "./maps/dm1.map" or
 * "datasrc/maps/dm1.map". DragonFS exposes the ROM filesystem under "rom:/",
 * so normalise the leading "./" and re-root the request. */
static void tw_rom_path(char *dst, int dst_size, const char *filename)
{
	while(filename[0] == '.' && filename[1] == '/')
		filename += 2;
	while(filename[0] == '/')
		++filename;
	snprintf(dst, dst_size, "rom:/%s", filename);
	dst[dst_size - 1] = 0;
}

static IOHANDLE io_open_impl(const char *filename, int flags)
{
	char path[IO_MAX_PATH_LENGTH + 8];
	if((flags & IOFLAG_READ) == 0)
		return 0x0; /* the ROM filesystem is read-only */
	tw_rom_path(path, sizeof(path), filename);
	return (IOHANDLE)fopen(path, "rb");
}

IOHANDLE io_open(const char *filename, int flags)
{
	IOHANDLE result = io_open_impl(filename, flags);
	unsigned char buf[3];
	if((flags & IOFLAG_SKIP_BOM) == 0 || !result)
		return result;
	if(io_read(result, buf, sizeof(buf)) != 3 || buf[0] != 0xef || buf[1] != 0xbb || buf[2] != 0xbf)
		io_seek(result, 0, IOSEEK_START);
	return result;
}

unsigned io_read(IOHANDLE io, void *buffer, unsigned size)
{
	return fread(buffer, 1, size, (FILE *)io);
}

int io_seek(IOHANDLE io, int offset, int origin)
{
	int real_origin;
	switch(origin)
	{
	case IOSEEK_START: real_origin = SEEK_SET; break;
	case IOSEEK_CUR: real_origin = SEEK_CUR; break;
	case IOSEEK_END: real_origin = SEEK_END; break;
	default: return -1;
	}
	return fseek((FILE *)io, offset, real_origin);
}

long int io_tell(IOHANDLE io)
{
	return ftell((FILE *)io);
}

long int io_length(IOHANDLE io)
{
	long int length;
	io_seek(io, 0, IOSEEK_END);
	length = io_tell(io);
	io_seek(io, 0, IOSEEK_START);
	return length;
}

unsigned io_skip(IOHANDLE io, int size)
{
	fseek((FILE *)io, size, SEEK_CUR);
	return size;
}

unsigned io_unread_byte(IOHANDLE io, unsigned char byte)
{
	return ungetc(byte, (FILE *)io) == EOF;
}

int io_error(IOHANDLE io)
{
	return ferror((FILE *)io);
}

int io_close(IOHANDLE io)
{
	fclose((FILE *)io);
	return 0;
}

int io_flush(IOHANDLE io)
{
	fflush((FILE *)io);
	return 0;
}

unsigned io_write(IOHANDLE io, const void *buffer, unsigned size)
{
	return fwrite(buffer, 1, size, (FILE *)io);
}

unsigned io_write_newline(IOHANDLE io)
{
	return fwrite("\n", 1, 1, (FILE *)io);
}

void io_read_all(IOHANDLE io, void **result, unsigned *result_len)
{
	unsigned len = (unsigned)io_length(io);
	char *buffer = (char *)mem_alloc(len + 1);
	unsigned read = io_read(io, buffer, len + 1);
	if(read < len)
	{
		buffer = (char *)realloc(buffer, read + 1);
		len = read;
	}
	else if(read > len)
	{
		unsigned cap = 2 * read;
		len = read;
		buffer = (char *)realloc(buffer, cap);
		while((read = io_read(io, buffer + len, cap - len)) != 0)
		{
			len += read;
			if(len == cap)
			{
				cap *= 2;
				buffer = (char *)realloc(buffer, cap);
			}
		}
		buffer = (char *)realloc(buffer, len + 1);
	}
	buffer[len] = 0;
	*result = buffer;
	*result_len = len;
}

char *io_read_all_str(IOHANDLE io)
{
	void *buffer;
	unsigned len;
	io_read_all(io, &buffer, &len);
	if(mem_has_null(buffer, len))
	{
		mem_free(buffer);
		return 0x0;
	}
	return (char *)buffer;
}

IOHANDLE io_stdin(void) { return (IOHANDLE)stdin; }
IOHANDLE io_stdout(void) { return (IOHANDLE)stdout; }
IOHANDLE io_stderr(void) { return (IOHANDLE)stderr; }

/* ------------------------------------------------------------------ */
/* Filesystem queries (read-only ROM image)                           */
/* ------------------------------------------------------------------ */

int fs_is_dir(const char *path)
{
	/* CreateTestStorage() only ever probes "."; the DFS root stands in. */
	return path && path[0] == '.' && path[1] == 0;
}

char *fs_getcwd(char *buffer, int buffer_size)
{
	if(!buffer || buffer_size <= 0)
		return 0;
	str_copy(buffer, ".", buffer_size);
	return buffer;
}

int fs_storage_path(const char *appname, char *path, int max)
{
	(void)appname;
	if(max > 0)
		path[0] = 0;
	return -1;
}

void fs_listdir(const char *dir, FS_LISTDIR_CALLBACK cb, int type, void *user)
{
	(void)dir;
	(void)cb;
	(void)type;
	(void)user;
}

void fs_listdir_fileinfo(const char *dir, FS_LISTDIR_CALLBACK_FILEINFO cb, int type, void *user)
{
	(void)dir;
	(void)cb;
	(void)type;
	(void)user;
}

int fs_makedir(const char *path)
{
	(void)path;
	return -1;
}

int fs_makedir_recursive(const char *path)
{
	(void)path;
	return -1;
}

int fs_remove(const char *filename)
{
	(void)filename;
	return 1;
}

int fs_rename(const char *oldname, const char *newname)
{
	(void)oldname;
	(void)newname;
	return 1;
}

int fs_file_time(const char *name, time_t *created, time_t *modified)
{
	(void)name;
	*created = 0;
	*modified = 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Endianness                                                         */
/* ------------------------------------------------------------------ */

/* datafile.cpp byte-swaps every map record on big-endian targets, so this is
 * load bearing on the N64. Port of the host implementation. */
void swap_endian(void *data, unsigned elem_size, unsigned num)
{
	char *src = (char *)data;
	char *dst = src + (elem_size - 1);

	while(num)
	{
		unsigned n = elem_size >> 1;
		char tmp;
		while(n)
		{
			tmp = *src;
			*src = *dst;
			*dst = tmp;

			src++;
			dst--;
			n--;
		}

		src = src + (elem_size >> 1);
		dst = src + (elem_size - 1);
		num--;
	}
}

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

int64 time_freq(void)
{
	return 1000000;
}

int64 time_get(void)
{
	return (int64)get_ticks_us();
}

void thread_sleep(int milliseconds)
{
	(void)milliseconds;
}

void thread_yield(void)
{
}

/* ------------------------------------------------------------------ */
/* Strings (ports of the host implementations)                        */
/* ------------------------------------------------------------------ */

int str_length(const char *str)
{
	return (int)strlen(str);
}

void str_copy(char *dst, const char *src, int dst_size)
{
	dbg_assert(dst_size > 0, "dst_size invalid");
	dst[0] = '\0';
	strncat(dst, src, dst_size - 1);
}

void str_append(char *dst, const char *src, int dst_size)
{
	int s;
	int i = 0;
	dbg_assert(dst_size > 0, "dst_size invalid");
	s = str_length(dst);
	while(s < dst_size)
	{
		dst[s] = src[i];
		if(!src[i])
			break;
		s++;
		i++;
	}
	dst[dst_size - 1] = 0;
}

void str_format(char *buffer, int buffer_size, const char *format, ...)
{
	va_list ap;
	dbg_assert(buffer_size > 0, "buffer_size invalid");
	va_start(ap, format);
	vsnprintf(buffer, buffer_size, format, ap);
	va_end(ap);
	buffer[buffer_size - 1] = 0;
}

int str_comp(const char *a, const char *b)
{
	return strcmp(a, b);
}

int str_comp_num(const char *a, const char *b, const int num)
{
	return strncmp(a, b, num);
}

int str_comp_nocase(const char *a, const char *b)
{
	return strcasecmp(a, b);
}

int str_comp_nocase_num(const char *a, const char *b, const int num)
{
	return strncasecmp(a, b, num);
}

int str_comp_filenames(const char *a, const char *b)
{
	int result;
	for(; *a && *b; ++a, ++b)
	{
		if(isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
		{
			result = 0;
			do
			{
				if(!result)
					result = *a - *b;
				++a;
				++b;
			} while(isdigit((unsigned char)*a) && isdigit((unsigned char)*b));

			if(isdigit((unsigned char)*a))
				return 1;
			else if(isdigit((unsigned char)*b))
				return -1;
			else if(result)
				return result;
		}

		if(*a != *b)
			break;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

const char *str_startswith(const char *str, const char *prefix)
{
	int prefixl = str_length(prefix);
	if(str_comp_num(str, prefix, prefixl) == 0)
		return str + prefixl;
	return 0;
}

const char *str_endswith(const char *str, const char *suffix)
{
	int strl = str_length(str);
	int suffixl = str_length(suffix);
	const char *strsuffix;
	if(strl < suffixl)
		return 0;
	strsuffix = str + strl - suffixl;
	if(str_comp(strsuffix, suffix) == 0)
		return strsuffix;
	return 0;
}

const char *str_find_nocase(const char *haystack, const char *needle)
{
	while(*haystack)
	{
		const char *a = haystack;
		const char *b = needle;
		while(*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
		{
			a++;
			b++;
		}
		if(!(*b))
			return haystack;
		haystack++;
	}
	return 0;
}

const char *str_find(const char *haystack, const char *needle)
{
	while(*haystack)
	{
		const char *a = haystack;
		const char *b = needle;
		while(*a && *b && *a == *b)
		{
			a++;
			b++;
		}
		if(!(*b))
			return haystack;
		haystack++;
	}
	return 0;
}

int str_isspace(char c)
{
	return c == ' ' || c == '\n' || c == '\t';
}

char *str_skip_to_whitespace(char *str)
{
	while(*str && (*str != ' ' && *str != '\t' && *str != '\n'))
		str++;
	return str;
}

const char *str_skip_to_whitespace_const(const char *str)
{
	while(*str && (*str != ' ' && *str != '\t' && *str != '\n'))
		str++;
	return str;
}

char *str_skip_whitespaces(char *str)
{
	while(*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r'))
		str++;
	return str;
}

const char *str_skip_whitespaces_const(const char *str)
{
	while(*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r'))
		str++;
	return str;
}

int str_span(const char *str, const char *set)
{
	return strcspn(str, set);
}

void str_sanitize(char *str_in)
{
	unsigned char *str = (unsigned char *)str_in;
	while(*str)
	{
		if(*str < 32 && !(*str == '\r') && !(*str == '\n') && !(*str == '\t'))
			*str = ' ';
		str++;
	}
}

void str_sanitize_cc(char *str_in)
{
	unsigned char *str = (unsigned char *)str_in;
	while(*str)
	{
		if(*str < 32)
			*str = ' ';
		str++;
	}
}

void str_clean_whitespaces(char *str_in)
{
	char *read = str_in;
	char *write = str_in;

	/* skip initial whitespace */
	while(*read == ' ')
		read++;

	/* end of read string is detected in the loop */
	while(1)
	{
		/* skip whitespace */
		int found_whitespace = 0;
		for(; *read == ' '; read++)
			found_whitespace = 1;
		/* if not at the end of the string, put a found whitespace here */
		if(*read)
		{
			if(found_whitespace)
				*write++ = ' ';
			*write++ = *read++;
		}
		else
		{
			*write = 0;
			break;
		}
	}
}

int str_path_unsafe(const char *str)
{
	int parse_counter = 0;
	while(*str)
	{
		if(*str == '\\' || *str == '/')
		{
			if(parse_counter == 2)
				return -1;
			else
				parse_counter = 0;
		}
		else if(parse_counter >= 0)
		{
			if(*str == '.')
				parse_counter++;
			else
				parse_counter = -1;
		}
		++str;
	}
	if(parse_counter == 2)
		return -1;
	return 0;
}

int str_toint(const char *str)
{
	return atoi(str);
}

float str_tofloat(const char *str)
{
	return atof(str);
}

void str_hex(char *dst, int dst_size, const void *data, int data_size)
{
	static const char hex[] = "0123456789ABCDEF";
	int data_index;
	int dst_index;
	for(data_index = 0, dst_index = 0; data_index < data_size && dst_index < dst_size - 3; data_index++)
	{
		dst[data_index * 3] = hex[((const unsigned char *)data)[data_index] >> 4];
		dst[data_index * 3 + 1] = hex[((const unsigned char *)data)[data_index] & 0xf];
		dst[data_index * 3 + 2] = ' ';
		dst_index += 3;
	}
	dst[dst_index] = '\0';
}

/* The N64 has no dependable wall clock; timestamps are diagnostics only and
 * never feed the simulation, so emit a stable placeholder. */
void str_timestamp_ex(time_t time_data, char *buffer, int buffer_size, const char *format)
{
	(void)time_data;
	(void)format;
	str_copy(buffer, "n64", buffer_size);
}

void str_timestamp_format(char *buffer, int buffer_size, const char *format)
{
	str_timestamp_ex(0, buffer, buffer_size, format);
}

void str_timestamp(char *buffer, int buffer_size)
{
	str_timestamp_ex(0, buffer, buffer_size, FORMAT_NOSPACE);
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                              */
/* ------------------------------------------------------------------ */

static int str_utf8_isstart(char c)
{
	if((c & 0xC0) == 0x80)
		return 0;
	return 1;
}

int str_utf8_is_whitespace(int code)
{
	if(code > 0x20 && code != 0xA0 && code != 0x034F && (code < 0x2000 || code > 0x200F) && (code < 0x2028 || code > 0x202F) &&
		(code < 0x205F || code > 0x2064) && (code < 0x206A || code > 0x206F) && code != 0x3000 && (code < 0xFE00 || code > 0xFE0F) &&
		code != 0xFEFF && (code < 0xFFF9 || code > 0xFFFC))
	{
		return 0;
	}
	return 1;
}

int str_utf8_forward(const char *str, int cursor)
{
	const char *buf = str + cursor;
	if(!buf[0])
		return cursor;

	if((*buf & 0x80) == 0x0)
		return cursor + 1;
	else if((*buf & 0xE0) == 0xC0)
	{
		if(!buf[1]) return cursor + 1;
		return cursor + 2;
	}
	else if((*buf & 0xF0) == 0xE0)
	{
		if(!buf[1]) return cursor + 1;
		if(!buf[2]) return cursor + 2;
		return cursor + 3;
	}
	else if((*buf & 0xF8) == 0xF0)
	{
		if(!buf[1]) return cursor + 1;
		if(!buf[2]) return cursor + 2;
		if(!buf[3]) return cursor + 3;
		return cursor + 4;
	}
	return cursor + 1;
}

int str_utf8_rewind(const char *str, int cursor)
{
	while(cursor)
	{
		cursor--;
		if(str_utf8_isstart(*(str + cursor)))
			break;
	}
	return cursor;
}

int str_utf8_decode(const char **ptr)
{
	const char *buf = *ptr;
	int ch = 0;

	do
	{
		if((*buf & 0x80) == 0x0)
		{
			ch = (unsigned char)*buf;
			buf++;
		}
		else if((*buf & 0xE0) == 0xC0)
		{
			ch = (*buf++ & 0x3F) << 6; if(!(*buf) || (*buf & 0xC0) != 0x80) break;
			ch += (*buf++ & 0x3F);
			if(ch < 0x80 || ch > 0x7FF) ch = -1;
		}
		else if((*buf & 0xF0) == 0xE0)
		{
			ch = (*buf++ & 0x1F) << 12; if(!(*buf) || (*buf & 0xC0) != 0x80) break;
			ch += (*buf++ & 0x3F) << 6; if(!(*buf) || (*buf & 0xC0) != 0x80) break;
			ch += (*buf++ & 0x3F);
			if(ch < 0x800 || ch > 0xFFFF) ch = -1;
		}
		else if((*buf & 0xF8) == 0xF0)
		{
			ch = (*buf++ & 0x0F) << 18; if(!(*buf) || (*buf & 0xC0) != 0x80) break;
			ch += (*buf++ & 0x3F) << 12; if(!(*buf) || (*buf & 0xC0) != 0x80) break;
			ch += (*buf++ & 0x3F) << 6; if(!(*buf) || (*buf & 0xC0) != 0x80) break;
			ch += (*buf++ & 0x3F);
			if(ch < 0x10000 || ch > 0x10FFFF) ch = -1;
		}
		else
		{
			buf++;
			break;
		}

		*ptr = buf;
		return ch;
	} while(0);

	*ptr = buf;
	return -1;
}

int str_utf8_encode(char *ptr, int chr)
{
	if(chr <= 0x7F)
	{
		ptr[0] = (char)chr;
		return 1;
	}
	else if(chr <= 0x7FF)
	{
		ptr[0] = 0xC0 | ((chr >> 6) & 0x1F);
		ptr[1] = 0x80 | (chr & 0x3F);
		return 2;
	}
	else if(chr <= 0xFFFF)
	{
		ptr[0] = 0xE0 | ((chr >> 12) & 0x0F);
		ptr[1] = 0x80 | ((chr >> 6) & 0x3F);
		ptr[2] = 0x80 | (chr & 0x3F);
		return 3;
	}
	else if(chr <= 0x10FFFF)
	{
		ptr[0] = 0xF0 | ((chr >> 18) & 0x07);
		ptr[1] = 0x80 | ((chr >> 12) & 0x3F);
		ptr[2] = 0x80 | ((chr >> 6) & 0x3F);
		ptr[3] = 0x80 | (chr & 0x3F);
		return 4;
	}
	return 0;
}

int str_utf8_check(const char *str)
{
	while(*str)
	{
		if((*str & 0x80) == 0x0)
			str++;
		else if((*str & 0xE0) == 0xC0 && (*(str + 1) & 0xC0) == 0x80)
			str += 2;
		else if((*str & 0xF0) == 0xE0 && (*(str + 1) & 0xC0) == 0x80 && (*(str + 2) & 0xC0) == 0x80)
			str += 3;
		else if((*str & 0xF8) == 0xF0 && (*(str + 1) & 0xC0) == 0x80 && (*(str + 2) & 0xC0) == 0x80 && (*(str + 3) & 0xC0) == 0x80)
			str += 4;
		else
			return 0;
	}
	return 1;
}

void str_utf8_copy_num(char *dst, const char *src, int dst_size, int num)
{
	int new_cursor;
	int cursor = 0;
	dbg_assert(dst_size > 0, "dst_size invalid");

	while(src[cursor] && num > 0)
	{
		new_cursor = str_utf8_forward(src, cursor);
		if(new_cursor >= dst_size)
			break;
		else
			cursor = new_cursor;
		--num;
	}

	str_copy(dst, src, cursor < dst_size ? cursor + 1 : dst_size);
}

void str_utf8_stats(const char *str, int max_size, int max_count, int *size, int *count)
{
	*size = 0;
	*count = 0;
	while(*size < max_size && *count < max_count)
	{
		int new_size = str_utf8_forward(str, *size);
		if(new_size == *size || new_size >= max_size)
			break;
		*size = new_size;
		++(*count);
	}
}

const char *str_utf8_skip_whitespaces(const char *str)
{
	const char *str_old;
	int code;

	while(*str)
	{
		str_old = str;
		code = str_utf8_decode(&str);
		if(!str_utf8_is_whitespace(code))
			return str_old;
	}
	return str;
}

void str_utf8_trim_whitespaces_right(char *str)
{
	int cursor = str_length(str);
	const char *last = str + cursor;
	while(str_utf8_is_whitespace(str_utf8_decode(&last)))
	{
		str[cursor] = 0;
		cursor = str_utf8_rewind(str, cursor);
		last = str + cursor;
		if(cursor == 0)
			break;
	}
}
