#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <archive.h>
#include <archive_entry.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/limits.h>

int nvidia_major_version = 0;
int nvidia_minor_version = 0;
int nvidia_patch_version = 0;

void
die_with_error (const char *format, ...)
{
  va_list args;
  int errsv;

  errsv = errno;

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, ": %s\n", strerror (errsv));

  exit (1);
}

void
die_with_libarchive (struct archive *ar, const char *format)
{
  fprintf (stderr, format, archive_error_string (ar));
  exit (1);
}

void
die (const char *format, ...)
{
  va_list args;

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, "\n");

  exit (1);
}

int
parse_driver_version (const char *version,
                      int *major,
                      int *minor,
                      int *patch)
{
  return sscanf(version, "%d.%d.%d", major, minor, patch) < 2;
}

void
checked_symlink (const char *target,
                 const char *linkpath)
{
  if (symlink (target, linkpath) != 0)
    die_with_error ("failed to symlink '%s' to '%s'", target, linkpath);
  if (access (linkpath, F_OK) != 0 && errno == ENOENT)
    die ("broken symlink: '%s' points to a missing file", linkpath);
}

static int
has_prefix (const char *str,
            const char *prefix)
{
  return strncmp (str, prefix, strlen (prefix)) == 0;
}

static int
has_suffix (const char *str, const char *suffix)
{
  int str_len;
  int suffix_len;

  str_len = strlen (str);
  suffix_len = strlen (suffix);

  if (str_len < suffix_len)
    return 0;

  return strcmp (str + str_len - suffix_len, suffix) == 0;
}

static void
copy_archive (struct archive *ar,
              struct archive *aw)
{
  int r;
  const void *buff;
  size_t size;
  int64_t offset;

  while (1)
    {
      r = archive_read_data_block (ar, &buff, &size, &offset);
      if (r == ARCHIVE_EOF)
        break;
      if (r != ARCHIVE_OK)
        die_with_libarchive (ar, "archive_read_data_block: %s");

      r = archive_write_data_block (aw, buff, size, offset);
      if (r != ARCHIVE_OK)
        die_with_libarchive (aw, "archive_write_data_block: %s");
    }
}

static void
extract (int fd)
{
  struct archive *a;
  struct archive *ext;
  struct archive_entry *entry;
  int r;

  a = archive_read_new ();
  ext = archive_write_disk_new ();
  archive_read_support_format_tar (a);
  archive_read_support_filter_xz (a);
  archive_read_support_filter_gzip (a);
  archive_read_support_filter_zstd (a);

  if ((r = archive_read_open_fd (a, fd, 16*1024)))
    die_with_libarchive (a, "archive_read_open_fd: %s");

  while (1)
    {
      r = archive_read_next_header (a, &entry);
      if (r == ARCHIVE_EOF)
        break;

      if (r != ARCHIVE_OK)
        die_with_libarchive (a, "archive_read_next_header: %s");

      const char *path = archive_entry_pathname (entry);

      if (!has_suffix (path, ".run"))
        continue;

      fprintf (stderr, "Found a .run file in: %s\n", path);
      path = strrchr(path, '/') + 1;
      fprintf (stderr, "Extracting it to: %s\n", path);

      archive_entry_set_pathname(entry, path);

      r = archive_write_header (ext, entry);
      if (r != ARCHIVE_OK)
        die_with_libarchive (ext, "archive_write_header: %s");
      else
        {
          copy_archive (a, ext);
          r = archive_write_finish_entry (ext);
          if (r != ARCHIVE_OK)
            die_with_libarchive (ext, "archive_write_finish_entry: %s");
          break;
        }
    }

  archive_read_close (a);
  archive_read_free (a);

  archive_write_close (ext);
  archive_write_free (ext);
}

static int
find_skip_lines (int fd)
{
  /*
   * CUDA drivers use makeself v2.1.4, which doesn't have a skip= line at the
   * beginning of the file. Instead, there's an
   * offset=`head -n "$skip" "$0" | wc -c | tr -d " "`
   * after all of the `--help` text, at approx the 14k byte offset.
   *
   * See https://github.com/megastep/makeself/commit/0d093c01e06ee76643ba2cffabf2ca07282d3af1
   */
  char buffer[16*1024];
  ssize_t size;
  char *line_start, *line_end, *buffer_end;
  char *skip_str = NULL;
  int skip_lines;

  size = pread (fd, buffer, sizeof buffer - 1, 0);
  if (size == -1)
    die_with_error ("read extra data");

  buffer[size] = 0; /* Ensure zero termination */
  buffer_end = buffer + size;

  line_start = buffer;
  while (line_start < buffer_end)
    {
      line_end = strchr (line_start, '\n');
      if (line_end != NULL)
        {
          *line_end = 0;
          line_end += 1;
        }
      else
        line_end = buffer_end;

      if (has_prefix (line_start, "skip="))
        {
          skip_str = line_start + 5;
          break;
        }
      else if (has_prefix (line_start, "offset=`head -n "))
        /* makeself v2 removed the skip= line, then added it back in v2.4.2 */
        {
          skip_str = line_start + 16;
          skip_lines = atoi (skip_str);
          if (skip_lines == 0)
            die ("Can't parse offset=`head -n %s", skip_str);

          return skip_lines + 1;
        }

      line_start = line_end;
    }

  if (skip_str == NULL)
    die ("Can't find skip size");

  skip_lines = atoi (skip_str);

  if (skip_lines == 0)
    die ("Can't parse skip=%s", skip_str);

  return skip_lines;
}

static off_t
find_line_offset (int fd, int skip_lines)
{
  off_t offset, buf_offset;
  int n_lines;
  ssize_t size;
  char buffer[16*1024];

  offset = 0;
  buf_offset = 0;
  n_lines = 1;

  while (1)
    {
      size = pread (fd, buffer, sizeof buffer, offset);
      if (size == -1)
        die_with_error ("read data");

      buf_offset = 0;
      while (buf_offset < size)
        {
          if (buffer[buf_offset] == '\n')
            {
              n_lines ++;
              if (n_lines == skip_lines)
                return offset + buf_offset + 1;
            }

          buf_offset++;
        }

      offset += size;
    }

  /* Should not happen */
  return 0;
}

int
main (int argc, char *argv[])
{
  int fd;
  int skip_lines;
  off_t tar_start;

  if (argc < 2)
  {
    die ("usage: %s <CUDA .run file>", argv[0]);
    return 1;
  }

  fd = open (argv[1], O_RDONLY);
  if (fd == -1)
    die_with_error ("failed to open .run file '%s'", argv[1]);

  skip_lines = find_skip_lines (fd);
  tar_start = find_line_offset (fd, skip_lines);

  if (lseek (fd, tar_start, SEEK_SET)!= tar_start)
    die ("Can't seek to tar");

  extract (fd);

  close (fd);

  return 0;
}
