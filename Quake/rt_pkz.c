
#include "quakedef.h"
#include "rt_pkz.h"
#include "sys.h"
#include "miniz.h"
#include <io.h>
#include <fcntl.h>

#define RT_PKZ_MAX_STREAMS  64
#define RT_PKZ_HANDLE_BASE  100000

typedef struct rt_pkz_archive_s
{
    struct rt_pkz_archive_s *prev;
    struct rt_pkz_archive_s *next;
    char           path[MAX_OSPATH];
    byte          *data;
    size_t         dataSize;
    mz_zip_archive zip;
    qboolean       opened;
} rt_pkz_archive_t;

typedef struct {
    qboolean inUse;
    rt_pkz_archive_t *archive;
    int fileIndex;
    byte *buf;
    size_t size;
    size_t pos;
} rt_pkz_stream_t;

static rt_pkz_archive_t *rt_pkz_first = NULL;
static rt_pkz_archive_t *rt_pkz_last = NULL;
static int               rt_pkz_count = 0;
static rt_pkz_stream_t   rt_pkz_streams[RT_PKZ_MAX_STREAMS];

static void *pkz_alloc(void *opaque, size_t items, size_t size)
{
    (void)opaque;
    return Mem_Alloc(items * size);
}

static void pkz_free(void *opaque, void *address)
{
    (void)opaque;
    Mem_Free(address);
}

static void *pkz_realloc(void *opaque, void *address, size_t items, size_t size)
{
    (void)opaque;
    return Mem_Realloc(address, items * size);
}

static size_t pkz_read(void *opaque, mz_uint64 file_ofs, void *pBuf, size_t n)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)opaque;
    if (file_ofs + n > a->dataSize)
    {
        return 0;
    }
    memcpy(pBuf, a->data + file_ofs, n);
    return n;
}

static void rt_pkz_add_searchpath(rt_pkz_archive_t *a, unsigned int path_id)
{
    searchpath_t *s = (searchpath_t *)Mem_Alloc(sizeof(searchpath_t));
    memset(s, 0, sizeof(*s));
    s->path_id = path_id;
    q_strlcpy(s->filename, a->path, sizeof(s->filename));
    s->rt_pkz = a;
    s->next = com_searchpaths;
    com_searchpaths = s;
}

void RT_PKZ_MountDir(const char *dir, unsigned int path_id)
{
    char pattern[MAX_OSPATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    q_snprintf(pattern, sizeof(pattern), "%s/*.pkz", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        char path[MAX_OSPATH];
        int handle = -1;
        int size;
        int rd;
        rt_pkz_archive_t *a;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }

        q_snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
        size = Sys_FileOpenRead(path, &handle);
        if (size <= 0)
        {
            if (handle != -1)
            {
                Sys_FileClose(handle);
            }
            continue;
        }

        a = (rt_pkz_archive_t *)Mem_Alloc(sizeof(*a));
        memset(a, 0, sizeof(*a));

        a->data = (byte *)Mem_Alloc(size);
        if (!a->data)
        {
            Sys_FileClose(handle);
            Mem_Free(a);
            continue;
        }

        rd = Sys_FileRead(handle, a->data, size);
        Sys_FileClose(handle);
        if (rd != size)
        {
            Mem_Free(a->data);
            Mem_Free(a);
            continue;
        }

        a->dataSize = (size_t)size;
        a->zip.m_pAlloc = pkz_alloc;
        a->zip.m_pFree = pkz_free;
        a->zip.m_pRealloc = pkz_realloc;
        a->zip.m_pAlloc_opaque = a;
        a->zip.m_pRead = pkz_read;
        a->zip.m_pIO_opaque = a;
        a->zip.m_pNeeds_keepalive = NULL;

        if (mz_zip_reader_init(&a->zip, a->dataSize, 0))
        {
            a->opened = true;
            q_strlcpy(a->path, path, sizeof(a->path));
            a->prev = rt_pkz_last;
            if (rt_pkz_last)
            {
                rt_pkz_last->next = a;
            }
            else
            {
                rt_pkz_first = a;
            }
            rt_pkz_last = a;
            rt_pkz_count++;
            rt_pkz_add_searchpath(a, path_id);
            Con_Printf("RT: mounted pkz %s (%u files)\n", path, (unsigned)a->zip.m_total_files);
        }
        else
        {
            Mem_Free(a->data);
            Mem_Free(a);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

void RT_PKZ_Unmount(void *varchive)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    int i;

    if (!a)
    {
        return;
    }

    for (i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        if (rt_pkz_streams[i].inUse && rt_pkz_streams[i].archive == a)
        {
            RT_PKZ_Close(RT_PKZ_HANDLE_BASE + i);
        }
    }

    if (a->prev)
    {
        a->prev->next = a->next;
    }
    else
    {
        rt_pkz_first = a->next;
    }
    if (a->next)
    {
        a->next->prev = a->prev;
    }
    else
    {
        rt_pkz_last = a->prev;
    }

    if (a->opened)
    {
        mz_zip_reader_end(&a->zip);
    }
    if (a->data)
    {
        Mem_Free(a->data);
    }
    Mem_Free(a);
    if (rt_pkz_count > 0)
    {
        rt_pkz_count--;
    }
}

void RT_PKZ_Shutdown(void)
{
    searchpath_t *s;
    int i;

    for (i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        if (rt_pkz_streams[i].inUse)
        {
            RT_PKZ_Close(RT_PKZ_HANDLE_BASE + i);
        }
    }

    s = com_searchpaths;
    while (s)
    {
        searchpath_t *next = s->next;

        if (s->rt_pkz)
        {
            searchpath_t **link;

            for (link = &com_searchpaths; *link; link = &(*link)->next)
            {
                if (*link == s)
                {
                    *link = s->next;
                    break;
                }
            }
            Mem_Free(s);
        }
        s = next;
    }

    while (rt_pkz_first)
    {
        RT_PKZ_Unmount(rt_pkz_first);
    }

    for (i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        rt_pkz_streams[i].inUse = false;
        if (rt_pkz_streams[i].buf)
        {
            Mem_Free(rt_pkz_streams[i].buf);
            rt_pkz_streams[i].buf = NULL;
        }
    }

}

int RT_PKZ_ListFiles(const char *dir, const char *ext,
                     int (*cb)(const char *name, void *ctx), void *ctx)
{
    int count = 0;
    size_t dirLen = strlen(dir);
    size_t extLen = strlen(ext);
    rt_pkz_archive_t *a;

    for (a = rt_pkz_first; a; a = a->next)
    {
        mz_uint num = (mz_uint)a->zip.m_total_files;
        for (mz_uint f = 0; f < num; f++)
        {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&a->zip, f, &st))
            {
                continue;
            }

            const char *name = st.m_filename;
            size_t nameLen = strlen(name);
            if (nameLen < dirLen + extLen)
            {
                continue;
            }
            if (q_strncasecmp(name, dir, dirLen) != 0)
            {
                continue;
            }
            if (q_strcasecmp(name + nameLen - extLen, ext) != 0)
            {
                continue;
            }

            count++;
            if (cb && cb(name, ctx))
            {
                return count;
            }
        }
    }

    return count;
}

static int rt_pkz_find_index(rt_pkz_archive_t *a, const char *name, int *outSize)
{
    if (!a || !name || !name[0])
    {
        return -1;
    }

    mz_uint32 file_index;
    if (!mz_zip_reader_locate_file_v2(&a->zip, name, NULL, 0, &file_index))
    {
        return -1;
    }

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&a->zip, file_index, &st))
    {
        return -1;
    }

    if (outSize)
    {
        *outSize = (int)st.m_uncomp_size;
    }
    return (int)file_index;
}

int RT_PKZ_FindFile(const void *varchive, const char *name, int *outSize)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    int size = 0;
    if (rt_pkz_find_index(a, name, &size) < 0)
    {
        return -1;
    }
    if (outSize)
    {
        *outSize = size;
    }
    return size;
}

int RT_PKZ_OpenFile(const void *varchive, const char *name, int *outSize)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    int size = 0;
    int fi = rt_pkz_find_index(a, name, &size);
    if (fi < 0)
    {
        return -1;
    }

    for (int i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        rt_pkz_stream_t *s = &rt_pkz_streams[i];
        if (s->inUse)
        {
            continue;
        }

        s->inUse = true;
        s->archive = a;
        s->fileIndex = fi;
        s->buf = NULL;
        s->size = (size_t)size;
        s->pos = 0;
        if (outSize)
        {
            *outSize = size;
        }
        return RT_PKZ_HANDLE_BASE + i;
    }
    return -1;
}

qboolean RT_PKZ_IsHandle(int handle)
{
    return handle >= RT_PKZ_HANDLE_BASE && handle < RT_PKZ_HANDLE_BASE + RT_PKZ_MAX_STREAMS;
}

static rt_pkz_stream_t *rt_pkz_get_stream(int handle)
{
    if (!RT_PKZ_IsHandle(handle))
    {
        return NULL;
    }
    rt_pkz_stream_t *s = &rt_pkz_streams[handle - RT_PKZ_HANDLE_BASE];
    return s->inUse ? s : NULL;
}

int RT_PKZ_Read(int handle, void *dest, int count)
{
    rt_pkz_stream_t *s = rt_pkz_get_stream(handle);
    if (!s || count <= 0)
    {
        return 0;
    }

    if (!s->buf)
    {
        rt_pkz_archive_t *a = s->archive;
        size_t len = 0;
        s->buf = (byte *)mz_zip_reader_extract_to_heap(&a->zip, (mz_uint)s->fileIndex, &len, 0);
        if (!s->buf)
        {
            return 0;
        }
        s->size = len;
    }

    size_t avail = s->size - s->pos;
    if ((size_t)count > avail)
    {
        count = (int)avail;
    }
    if (count <= 0)
    {
        return 0;
    }

    memcpy(dest, s->buf + s->pos, (size_t)count);
    s->pos += (size_t)count;
    return count;
}

void RT_PKZ_Seek(int handle, int position)
{
    rt_pkz_stream_t *s = rt_pkz_get_stream(handle);
    if (!s)
    {
        return;
    }
    if (position < 0)
    {
        position = 0;
    }
    s->pos = ((size_t)position < s->size) ? (size_t)position : s->size;
}

void RT_PKZ_Close(int handle)
{
    rt_pkz_stream_t *s = rt_pkz_get_stream(handle);
    if (!s)
    {
        return;
    }
    if (s->buf)
    {
        Mem_Free(s->buf);
        s->buf = NULL;
    }
    s->inUse = false;
}

FILE *RT_PKZ_OpenFileAsFILE(const void *varchive, const char *name)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    int fi = rt_pkz_find_index(a, name, NULL);
    char  tmppath[MAX_OSPATH];
    char  tmpfile[MAX_OSPATH] = "";
    HANDLE h;
    int    fd;
    FILE  *f;
    size_t len = 0;
    size_t got;
    void  *buf;

    if (fi < 0)
    {
        return NULL;
    }

    buf = mz_zip_reader_extract_to_heap(&a->zip, (mz_uint)fi, &len, 0);
    if (!buf)
    {
        return NULL;
    }

    if (!GetTempPathA(sizeof(tmppath), tmppath) ||
        !GetTempFileNameA(tmppath, "rtp", 0, tmpfile))
    {
        Mem_Free(buf);
        return NULL;
    }

    h = CreateFileA(tmpfile, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, TRUNCATE_EXISTING,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        DeleteFileA(tmpfile);
        Mem_Free(buf);
        return NULL;
    }

    fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
    if (fd == -1)
    {
        CloseHandle(h);
        Mem_Free(buf);
        return NULL;
    }

    f = _fdopen(fd, "r+b");
    if (!f)
    {
        _close(fd);
        Mem_Free(buf);
        return NULL;
    }

    got = (len > 0) ? fwrite(buf, 1, len, f) : 0;
    Mem_Free(buf);

    if (got != len || fflush(f) != 0)
    {
        fclose(f);
        return NULL;
    }

    rewind(f);
    return f;
}
