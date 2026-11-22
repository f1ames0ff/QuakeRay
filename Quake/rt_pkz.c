
#include "quakedef.h"
#include "atomics.h"
#include "rt_pkz.h"
#include "sys.h"
#include "miniz.h"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <dirent.h>
#endif

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
static SDL_SpinLock      rt_pkz_mutex_init_lock = 0;
static atomic_ptr_t      rt_pkz_mutex;

static SDL_mutex *rt_pkz_lock(void)
{
    SDL_mutex *m = (SDL_mutex *)Atomic_LoadPtr(&rt_pkz_mutex);
    if (!m)
    {
        SDL_AtomicLock(&rt_pkz_mutex_init_lock);
        m = (SDL_mutex *)Atomic_LoadPtr(&rt_pkz_mutex);
        if (!m)
        {
            m = SDL_CreateMutex();
            Atomic_StorePtr(&rt_pkz_mutex, m);
        }
        SDL_AtomicUnlock(&rt_pkz_mutex_init_lock);
    }
    if (m)
    {
        SDL_LockMutex(m);
    }
    return m;
}

static void rt_pkz_unlock(SDL_mutex *m)
{
    if (m)
    {
        SDL_UnlockMutex(m);
    }
}

static qboolean rt_pkz_has_archive_locked(const rt_pkz_archive_t *target)
{
    const rt_pkz_archive_t *a;

    if (!target)
    {
        return false;
    }
    for (a = rt_pkz_first; a; a = a->next)
    {
        if (a == target)
        {
            return true;
        }
    }
    return false;
}

static rt_pkz_stream_t *rt_pkz_get_stream_locked(int handle)
{
    if (!RT_PKZ_IsHandle(handle))
    {
        return NULL;
    }
    rt_pkz_stream_t *s = &rt_pkz_streams[handle - RT_PKZ_HANDLE_BASE];
    return s->inUse ? s : NULL;
}

static void rt_pkz_close_locked(int handle)
{
    rt_pkz_stream_t *s = rt_pkz_get_stream_locked(handle);
    if (!s)
    {
        return;
    }
    if (s->buf)
    {
        Mem_Free(s->buf);
        s->buf = NULL;
    }
    s->archive = NULL;
    s->fileIndex = -1;
    s->size = 0;
    s->pos = 0;
    s->inUse = false;
}

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

static void rt_pkz_mount_file(const char *path, unsigned int path_id)
{
	int handle = -1;
	int size;
	int rd;
	rt_pkz_archive_t *a;

	size = Sys_FileOpenRead(path, &handle);
	if (size <= 0)
	{
		if (handle != -1)
		{
			Sys_FileClose(handle);
		}
		return;
	}

	a = (rt_pkz_archive_t *)Mem_Alloc(sizeof(*a));
	memset(a, 0, sizeof(*a));

	a->data = (byte *)Mem_Alloc(size);
	if (!a->data)
	{
		Sys_FileClose(handle);
		Mem_Free(a);
		return;
	}

	rd = Sys_FileRead(handle, a->data, size);
	Sys_FileClose(handle);
	if (rd != size)
	{
		Mem_Free(a->data);
		Mem_Free(a);
		return;
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
		SDL_mutex *m;
		mz_uint    total_files;

		a->opened = true;
		q_strlcpy(a->path, path, sizeof(a->path));
		total_files = a->zip.m_total_files;

		m = rt_pkz_lock();
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
		rt_pkz_unlock(m);

		Con_Printf("RT: mounted pkz %s (%u files)\n", path, (unsigned)total_files);
	}
	else
	{
		Mem_Free(a->data);
		Mem_Free(a);
	}
}

void RT_PKZ_MountDir(const char *dir, unsigned int path_id)
{
	char path[MAX_OSPATH];

#ifdef _WIN32
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
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			continue;
		}

		q_snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
		rt_pkz_mount_file(path, path_id);
	} while (FindNextFileA(h, &fd));

	FindClose(h);
#else
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
	{
		return;
	}

	while ((e = readdir(d)) != NULL)
	{
		size_t n = strlen(e->d_name);

		if (n < 4 || q_strcasecmp(e->d_name + n - 4, ".pkz") != 0)
		{
			continue;
		}

		q_snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		rt_pkz_mount_file(path, path_id);
	}

	closedir(d);
#endif
}

static void rt_pkz_unmount_locked(rt_pkz_archive_t *a)
{
    int i;

    if (!rt_pkz_has_archive_locked(a))
    {
        return;
    }

    for (i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        if (rt_pkz_streams[i].inUse && rt_pkz_streams[i].archive == a)
        {
            rt_pkz_close_locked(RT_PKZ_HANDLE_BASE + i);
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

void RT_PKZ_Unmount(void *varchive)
{
    SDL_mutex *m;

    if (!varchive)
    {
        return;
    }

    m = rt_pkz_lock();
    rt_pkz_unmount_locked((rt_pkz_archive_t *)varchive);
    rt_pkz_unlock(m);
}

void RT_PKZ_Shutdown(void)
{
    SDL_mutex    *m = rt_pkz_lock();
    searchpath_t *s;
    int           i;

    for (i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        if (rt_pkz_streams[i].inUse)
        {
            rt_pkz_close_locked(RT_PKZ_HANDLE_BASE + i);
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
        rt_pkz_unmount_locked(rt_pkz_first);
    }

    for (i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        rt_pkz_streams[i].inUse = false;
        rt_pkz_streams[i].archive = NULL;
        if (rt_pkz_streams[i].buf)
        {
            Mem_Free(rt_pkz_streams[i].buf);
            rt_pkz_streams[i].buf = NULL;
        }
    }

    rt_pkz_unlock(m);

    SDL_AtomicLock(&rt_pkz_mutex_init_lock);
    m = (SDL_mutex *)Atomic_LoadPtr(&rt_pkz_mutex);
    Atomic_StorePtr(&rt_pkz_mutex, NULL);
    SDL_AtomicUnlock(&rt_pkz_mutex_init_lock);
    if (m)
    {
        SDL_DestroyMutex(m);
    }
}

static void rt_pkz_collect_archive_locked(rt_pkz_archive_t *a,
                                          const char *dir, size_t dirLen,
                                          const char *ext, size_t extLen,
                                          qboolean collect,
                                          char ***names, int *count, int *cap)
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

        if (collect)
        {
            if (*count >= *cap)
            {
                int newCap = (*cap > 0) ? (*cap * 2) : 32;
                *names = (char **)Mem_Realloc(*names, (size_t)newCap * sizeof(char *));
                *cap = newCap;
            }
            (*names)[*count] = q_strdup(name);
        }
        (*count)++;
    }
}

static int rt_pkz_dispatch_listed(char **names, int matched,
                                  int (*cb)(const char *name, void *ctx), void *ctx)
{
    int invoked = 0;
    int i;

    if (!cb)
    {
        return matched;
    }

    for (i = 0; i < matched; i++)
    {
        invoked++;
        if (cb(names[i], ctx))
        {
            break;
        }
    }

    for (i = 0; i < matched; i++)
    {
        Mem_Free(names[i]);
    }
    Mem_Free(names);
    return invoked;
}

int RT_PKZ_ListFiles(const char *dir, const char *ext,
                     int (*cb)(const char *name, void *ctx), void *ctx)
{
    SDL_mutex        *m;
    size_t            dirLen;
    size_t            extLen;
    rt_pkz_archive_t *a;
    char            **names = NULL;
    int               matched = 0;
    int               cap = 0;

    if (!dir || !ext)
    {
        return 0;
    }

    dirLen = strlen(dir);
    extLen = strlen(ext);

    m = rt_pkz_lock();
    for (a = rt_pkz_first; a; a = a->next)
    {
        rt_pkz_collect_archive_locked(a, dir, dirLen, ext, extLen,
                                      cb != NULL, &names, &matched, &cap);
    }
    rt_pkz_unlock(m);

    return rt_pkz_dispatch_listed(names, matched, cb, ctx);
}

int RT_PKZ_ListArchiveFiles(const void *varchive, const char *dir, const char *ext,
                            int (*cb)(const char *name, void *ctx), void *ctx)
{
    SDL_mutex        *m;
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    size_t            dirLen;
    size_t            extLen;
    char            **names = NULL;
    int               matched = 0;
    int               cap = 0;

    if (!a || !dir || !ext)
    {
        return 0;
    }

    dirLen = strlen(dir);
    extLen = strlen(ext);

    m = rt_pkz_lock();
    if (rt_pkz_has_archive_locked(a))
    {
        rt_pkz_collect_archive_locked(a, dir, dirLen, ext, extLen,
                                      cb != NULL, &names, &matched, &cap);
    }
    rt_pkz_unlock(m);

    return rt_pkz_dispatch_listed(names, matched, cb, ctx);
}

static int rt_pkz_find_index_locked(rt_pkz_archive_t *a, const char *name, int *outSize)
{
    if (!rt_pkz_has_archive_locked(a) || !name || !name[0])
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
    SDL_mutex        *m = rt_pkz_lock();
    int               size = 0;
    int               fi = rt_pkz_find_index_locked(a, name, &size);

    rt_pkz_unlock(m);
    if (fi < 0)
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
    SDL_mutex        *m = rt_pkz_lock();
    int               size = 0;
    int               fi = rt_pkz_find_index_locked(a, name, &size);
    int               handle = -1;

    if (fi >= 0)
    {
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
            handle = RT_PKZ_HANDLE_BASE + i;
            break;
        }
    }
    rt_pkz_unlock(m);

    if (handle >= 0 && outSize)
    {
        *outSize = size;
    }
    return handle;
}

byte *RT_PKZ_LoadFile(const void *varchive, const char *name, int *outSize)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    SDL_mutex        *m = rt_pkz_lock();
    int               fi = rt_pkz_find_index_locked(a, name, NULL);
    void             *raw = NULL;
    size_t            len = 0;
    byte             *out;

    if (fi >= 0)
    {
        raw = mz_zip_reader_extract_to_heap(&a->zip, (mz_uint)fi, &len, 0);
    }
    rt_pkz_unlock(m);

    if (!raw)
    {
        return NULL;
    }

    out = (byte *)Mem_Alloc(len + 1);
    if (len > 0)
    {
        memcpy(out, raw, len);
    }
    out[len] = 0;
    Mem_Free(raw);

    if (outSize)
    {
        *outSize = (int)len;
    }
    return out;
}

qboolean RT_PKZ_IsHandle(int handle)
{
    return handle >= RT_PKZ_HANDLE_BASE && handle < RT_PKZ_HANDLE_BASE + RT_PKZ_MAX_STREAMS;
}

int RT_PKZ_Read(int handle, void *dest, int count)
{
    SDL_mutex       *m;
    rt_pkz_stream_t *s;

    if (count <= 0 || !dest)
    {
        return 0;
    }

    m = rt_pkz_lock();
    s = rt_pkz_get_stream_locked(handle);
    if (!s)
    {
        rt_pkz_unlock(m);
        return 0;
    }

    if (!s->buf)
    {
        rt_pkz_archive_t *a = s->archive;
        size_t            len = 0;

        if (!rt_pkz_has_archive_locked(a))
        {
            rt_pkz_unlock(m);
            return 0;
        }
        s->buf = (byte *)mz_zip_reader_extract_to_heap(&a->zip, (mz_uint)s->fileIndex, &len, 0);
        if (!s->buf)
        {
            rt_pkz_unlock(m);
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
        rt_pkz_unlock(m);
        return 0;
    }

    memcpy(dest, s->buf + s->pos, (size_t)count);
    s->pos += (size_t)count;
    rt_pkz_unlock(m);
    return count;
}

void RT_PKZ_Seek(int handle, int position)
{
    SDL_mutex       *m = rt_pkz_lock();
    rt_pkz_stream_t *s = rt_pkz_get_stream_locked(handle);

    if (s)
    {
        if (position < 0)
        {
            position = 0;
        }
        s->pos = ((size_t)position < s->size) ? (size_t)position : s->size;
    }
    rt_pkz_unlock(m);
}

void RT_PKZ_Close(int handle)
{
    SDL_mutex *m = rt_pkz_lock();
    rt_pkz_close_locked(handle);
    rt_pkz_unlock(m);
}

FILE *RT_PKZ_OpenFileAsFILE(const void *varchive, const char *name)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    SDL_mutex        *m = rt_pkz_lock();
    int               fi = rt_pkz_find_index_locked(a, name, NULL);
    FILE             *f;
    size_t            len = 0;
    size_t            got;
    void             *buf = NULL;

    if (fi >= 0)
    {
        buf = mz_zip_reader_extract_to_heap(&a->zip, (mz_uint)fi, &len, 0);
    }
    rt_pkz_unlock(m);

    if (!buf)
    {
        return NULL;
    }

#ifdef _WIN32
    {
        char   tmppath[MAX_OSPATH];
        char   tmpfile[MAX_OSPATH] = "";
        HANDLE h;
        int    fd;

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
    }
#else
    f = tmpfile();
    if (!f)
    {
        Mem_Free(buf);
        return NULL;
    }
#endif

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
