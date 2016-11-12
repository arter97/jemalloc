#define	JEMALLOC_PAGES_C_
#include "jemalloc/internal/jemalloc_internal.h"

#ifdef JEMALLOC_SYSCTL_VM_OVERCOMMIT
#include <sys/sysctl.h>
#endif

/******************************************************************************/
/* Data. */

#ifndef _WIN32
#  define PAGES_PROT_COMMIT (PROT_READ | PROT_WRITE)
#  define PAGES_PROT_DECOMMIT (PROT_NONE)
static int	mmap_flags;
#endif
#define JEMALLOC_ENABLE_OVERCOMMIT
static bool	os_overcommits;

/******************************************************************************/
/* Defines/includes needed for special android code. */

#if defined(__ANDROID__)
#include <sys/prctl.h>

/* Definitions of prctl arguments to set a vma name in Android kernels. */
#define ANDROID_PR_SET_VMA            0x53564d41
#define ANDROID_PR_SET_VMA_ANON_NAME  0
#endif

/******************************************************************************/

void *
pages_map(void *addr, size_t size, bool *commit)
{
	void *ret;

	assert(size != 0);

#ifdef JEMALLOC_ENABLE_OVERCOMMIT
	*commit = true;
#endif

#ifdef _WIN32
	/*
	 * If VirtualAlloc can't allocate at the given address when one is
	 * given, it fails and returns NULL.
	 */
	ret = VirtualAlloc(addr, size, MEM_RESERVE | (*commit ? MEM_COMMIT : 0),
	    PAGE_READWRITE);
#else
	/*
	 * We don't use MAP_FIXED here, because it can cause the *replacement*
	 * of existing mappings, and we only want to create new mappings.
	 */
	{
		int prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;

		ret = mmap(addr, size, prot, mmap_flags, -1, 0);
	}
	assert(ret != NULL);

	if (ret == MAP_FAILED)
		ret = NULL;
	else if (addr != NULL && ret != addr) {
		/*
		 * We succeeded in mapping memory, but not in the right place.
		 */
		pages_unmap(ret, size);
		ret = NULL;
	}
#endif
#if defined(__ANDROID__)
	if (ret != NULL) {
		/* Name this memory as being used by libc */
		prctl(ANDROID_PR_SET_VMA, ANDROID_PR_SET_VMA_ANON_NAME, ret,
		    size, "libc_malloc");
	}
#endif
	assert(ret == NULL || (addr == NULL && ret != addr)
	    || (addr != NULL && ret == addr));
	return (ret);
}

void
pages_unmap(void *addr, size_t size)
{

#ifdef _WIN32
	if (VirtualFree(addr, 0, MEM_RELEASE) == 0)
#else
	if (munmap(addr, size) == -1)
#endif
	{
		char buf[BUFERROR_BUF];

		buferror(get_errno(), buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in "
#ifdef _WIN32
		              "VirtualFree"
#else
		              "munmap"
#endif
		              "(): %s\n", buf);
		if (opt_abort)
			abort();
	}
}

void *
pages_trim(void *addr, size_t alloc_size, size_t leadsize, size_t size,
    bool *commit)
{
	void *ret = (void *)((uintptr_t)addr + leadsize);

	assert(alloc_size >= leadsize + size);
#ifdef _WIN32
	{
		void *new_addr;

		pages_unmap(addr, alloc_size);
		new_addr = pages_map(ret, size, commit);
		if (new_addr == ret)
			return (ret);
		if (new_addr)
			pages_unmap(new_addr, size);
		return (NULL);
	}
#else
	{
		size_t trailsize = alloc_size - leadsize - size;

		if (leadsize != 0)
			pages_unmap(addr, leadsize);
		if (trailsize != 0)
			pages_unmap((void *)((uintptr_t)ret + size), trailsize);
		return (ret);
	}
#endif
}

static bool
pages_commit_impl(void *addr, size_t size, bool commit)
{
#ifdef JEMALLOC_ENABLE_OVERCOMMIT
	return (true);
#else

#ifdef _WIN32
	return (commit ? (addr != VirtualAlloc(addr, size, MEM_COMMIT,
	    PAGE_READWRITE)) : (!VirtualFree(addr, size, MEM_DECOMMIT)));
#else
	{
		int prot = commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
		void *result = mmap(addr, size, prot, mmap_flags | MAP_FIXED,
		    -1, 0);
		if (result == MAP_FAILED)
			return (true);
		if (result != addr) {
			/*
			 * We succeeded in mapping memory, but not in the right
			 * place.
			 */
			pages_unmap(result, size);
			return (true);
		}
		return (false);
	}
#endif
#endif
}

bool
pages_commit(void *addr, size_t size)
{

	return (pages_commit_impl(addr, size, true));
}

bool
pages_decommit(void *addr, size_t size)
{

	return (pages_commit_impl(addr, size, false));
}

bool
pages_purge(void *addr, size_t size)
{
	bool unzeroed;

#ifdef _WIN32
	VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
	unzeroed = true;
#elif defined(JEMALLOC_HAVE_MADVISE)
#  ifdef JEMALLOC_PURGE_MADVISE_DONTNEED
#    define JEMALLOC_MADV_PURGE MADV_DONTNEED
#    define JEMALLOC_MADV_ZEROS true
#  elif defined(JEMALLOC_PURGE_MADVISE_FREE)
#    define JEMALLOC_MADV_PURGE MADV_FREE
#    define JEMALLOC_MADV_ZEROS false
#  else
#    error "No madvise(2) flag defined for purging unused dirty pages."
#  endif
	int err = madvise(addr, size, JEMALLOC_MADV_PURGE);
	unzeroed = (!JEMALLOC_MADV_ZEROS || err != 0);
#  undef JEMALLOC_MADV_PURGE
#  undef JEMALLOC_MADV_ZEROS
#else
	/* Last resort no-op. */
	unzeroed = true;
#endif
	return (unzeroed);
}

void
pages_boot(void)
{

#ifndef _WIN32
	mmap_flags = MAP_PRIVATE | MAP_ANON;
#endif

#ifdef JEMALLOC_ENABLE_OVERCOMMIT
	mmap_flags |= MAP_NORESERVE;
#endif
}
