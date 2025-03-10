//! @file events.c
//!
//! incremental, orthogonal, paginated loom snapshots
//!
//! ### components
//!
//!   - page: 16KB chunk of the loom.
//!   - north segment (u3e_image, north.bin): low contiguous loom pages,
//!     (in practice, the home road heap). indexed from low to high:
//!     in-order on disk.
//!   - south segment (u3e_image, south.bin): high contiguous loom pages,
//!     (in practice, the home road stack). indexed from high to low:
//!     reversed on disk.
//!   - patch memory (memory.bin): new or changed pages since the last snapshot
//!   - patch control (u3e_control control.bin): patch metadata, watermarks,
//!     and indices/mugs for pages in patch memory.
//!
//! ### initialization (u3e_live())
//!
//!   - with the loom already mapped, all pages are marked dirty in a bitmap.
//!   - if snapshot is missing or partial, empty segments are created.
//!   - if a patch is present, it's applied (crash recovery).
//!   - snapshot segments are copied onto the loom; all included pages
//!     are marked clean and protected (read-only).
//!
//! #### page faults (u3e_fault())
//!
//!   - stores into protected pages generate faults (currently SIGSEGV,
//!     handled outside this module).
//!   - faults are handled by dirtying the page and switching protections to
//!     read/write.
//!
//! ### updates (u3e_save())
//!
//!   - all updates to a snapshot are made through a patch.
//!   - high/low watermarks for the north/south segments are established,
//!     and dirty pages below/above them are added to the patch.
//!     - modifications have been caught by the fault handler.
//!     - newly-used pages are automatically included (preemptively dirtied).
//!     - unused, innermost pages are reclaimed (segments are truncated to the
//!       high/low watermarks; the last page in each is always adjacent to the
//!       contiguous free space).
//!   - patch pages are written to memory.bin, metadata to control.bin.
//!   - the patch is applied to the snapshot segments, in-place.
//!   - patch files are deleted.
//!
//! ### limitations
//!
//!   - loom page size is fixed (16 KB), and must be a multiple of the
//!     system page size. (can the size vary at runtime give south.bin's
//!     reversed order? alternately, if system page size > ours, the fault
//!     handler could dirty N pages at a time.)
//!   - update atomicity is suspect: patch application must either
//!     completely succeed or leave on-disk segments intact. unapplied
//!     patches can be discarded (triggering event replay), but once
//!     patch application begins it must succeed (can fail if disk is full).
//!     may require integration into the overall signal-handling regime.
//!   - any errors are handled with assertions; failed/partial writes are not
//!     retried.
//!
//! ### enhancements
//!
//!   - use platform specific page fault mechanism (mach rpc, userfaultfd, &c).
//!   - implement demand paging / heuristic page-out.
//!   - add a guard page in the middle of the loom to reactively handle stack overflow.
//!   - parallelism
//!

#include "all.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef U3_SNAPSHOT_VALIDATION
/* Image check.
*/
struct {
  c3_w nor_w;
  c3_w sou_w;
  c3_w mug_w[u3a_pages];
} u3K;

/* _ce_check_page(): checksum page.
*/
static c3_w
_ce_check_page(c3_w pag_w)
{
  c3_w* mem_w = u3_Loom + (pag_w << u3a_page);
  c3_w  mug_w = u3r_mug_words(mem_w, (1 << u3a_page));

  return mug_w;
}

/* u3e_check(): compute a checksum on all memory within the watermarks.
*/
void
u3e_check(c3_c* cap_c)
{
  c3_w nor_w = 0;
  c3_w sou_w = 0;

  {
    c3_w nwr_w, swu_w;

    u3m_water(&nwr_w, &swu_w);

    nor_w = (nwr_w + ((1 << u3a_page) - 1)) >> u3a_page;
    sou_w = (swu_w + ((1 << u3a_page) - 1)) >> u3a_page;
  }

  /* Count dirty pages.
  */
  {
    c3_w i_w, sum_w, mug_w;

    sum_w = 0;
    for ( i_w = 0; i_w < nor_w; i_w++ ) {
      mug_w = _ce_check_page(i_w);
      if ( strcmp(cap_c, "boot") ) {
        c3_assert(mug_w == u3K.mug_w[i_w]);
      }
      sum_w += mug_w;
    }
    for ( i_w = 0; i_w < sou_w; i_w++ ) {
      mug_w = _ce_check_page((u3a_pages - (i_w + 1)));
      if ( strcmp(cap_c, "boot") ) {
        c3_assert(mug_w == u3K.mug_w[(u3a_pages - (i_w + 1))]);
      }
      sum_w += mug_w;
    }
    u3l_log("%s: sum %x (%x, %x)\r\n", cap_c, sum_w, nor_w, sou_w);
  }
}

/* _ce_maplloc(): crude off-loom allocator.
*/
static void*
_ce_maplloc(c3_w len_w)
{
  void* map_v;

  map_v = mmap(0,
               len_w,
               (PROT_READ | PROT_WRITE),
               (MAP_ANON | MAP_PRIVATE),
               -1, 0);

  if ( -1 == (c3_ps)map_v ) {
    c3_assert(0);
  }
  else {
    c3_w* map_w = map_v;

    map_w[0] = len_w;

    return map_w + 1;
  }
}

/* _ce_mapfree(): crude off-loom allocator.
*/
static void
_ce_mapfree(void* map_v)
{
  c3_w* map_w = map_v;
  c3_i res_i;

  map_w -= 1;
  res_i = munmap(map_w, map_w[0]);

  c3_assert(0 == res_i);
}
#endif

/* u3e_fault(): handle a memory event with libsigsegv protocol.
*/
c3_i
u3e_fault(void* adr_v, c3_i ser_i)
{
  //  Let the stack overflow handler run.
  if ( 0 == ser_i ) {
    return 0;
  }

  //  XX u3l_log avoid here, as it can
  //  cause problems when handling errors

  c3_w* adr_w = (c3_w*) adr_v;

  if ( (adr_w < u3_Loom) || (adr_w >= (u3_Loom + u3a_words)) ) {
    fprintf(stderr, "address %p out of loom!\r\n", adr_w);
    fprintf(stderr, "loom: [%p : %p)\r\n", u3_Loom, u3_Loom + u3a_words);
    c3_assert(0);
    return 0;
  }
  else {
    c3_w off_w = u3a_outa(adr_w);
    c3_w pag_w = off_w >> u3a_page;
    c3_w blk_w = (pag_w >> 5);
    c3_w bit_w = (pag_w & 31);

#if 0
    if ( pag_w == 131041 ) {
      u3l_log("dirty page %d (at %p); unprotecting %p to %p\r\n",
              pag_w,
              adr_v,
              (u3_Loom + (pag_w << u3a_page)),
              (u3_Loom + (pag_w << u3a_page) + (1 << u3a_page)));
    }
#endif

    if ( 0 != (u3P.dit_w[blk_w] & (1 << bit_w)) ) {
      fprintf(stderr, "strange page: %d, at %p, off %x\r\n",
              pag_w, adr_w, off_w);
      c3_assert(0);
      return 0;
    }

    u3P.dit_w[blk_w] |= (1 << bit_w);

    if ( -1 == mprotect((void *)(u3_Loom + (pag_w << u3a_page)),
                        (1 << (u3a_page + 2)),
                        (PROT_READ | PROT_WRITE)) )
    {
      fprintf(stderr, "loom: fault mprotect: %s\r\n", strerror(errno));
      c3_assert(0);
      return 0;
    }
  }
  return 1;
}

/* _ce_image_open(): open or create image.
*/
static c3_o
_ce_image_open(u3e_image* img_u)
{
  c3_i mod_i = O_RDWR | O_CREAT;
  c3_c ful_c[8193];

  snprintf(ful_c, 8192, "%s", u3P.dir_c);
  c3_mkdir(ful_c, 0700);

  snprintf(ful_c, 8192, "%s/.urb", u3P.dir_c);
  c3_mkdir(ful_c, 0700);

  snprintf(ful_c, 8192, "%s/.urb/chk", u3P.dir_c);
  c3_mkdir(ful_c, 0700);

  snprintf(ful_c, 8192, "%s/.urb/chk/%s.bin", u3P.dir_c, img_u->nam_c);
  if ( -1 == (img_u->fid_i = c3_open(ful_c, mod_i, 0666)) ) {
    fprintf(stderr, "loom: c3_open %s: %s\r\n", ful_c, strerror(errno));
    return c3n;
  }
  else {
    struct stat buf_u;

    if ( -1 == fstat(img_u->fid_i, &buf_u) ) {
      fprintf(stderr, "loom: stat %s: %s\r\n", ful_c, strerror(errno));
      c3_assert(0);
      return c3n;
    }
    else {
      c3_d siz_d = buf_u.st_size;
      c3_d pgs_d = (siz_d + (c3_d)((1 << (u3a_page + 2)) - 1)) >>
                   (c3_d)(u3a_page + 2);

      if ( !siz_d ) {
        return c3y;
      }
      else {
        if ( siz_d != (pgs_d << (c3_d)(u3a_page + 2)) ) {
          fprintf(stderr, "%s: corrupt size %" PRIx64 "\r\n", ful_c, siz_d);
          return c3n;
        }
        img_u->pgs_w = (c3_w) pgs_d;
        c3_assert(pgs_d == (c3_d)img_u->pgs_w);

        return c3y;
      }
    }
  }
}

/* _ce_patch_write_control(): write control block file.
*/
static void
_ce_patch_write_control(u3_ce_patch* pat_u)
{
  c3_w len_w = sizeof(u3e_control) +
               (pat_u->con_u->pgs_w * sizeof(u3e_line));

  if ( len_w != write(pat_u->ctl_i, pat_u->con_u, len_w) ) {
    c3_assert(0);
  }
}

/* _ce_patch_read_control(): read control block file.
*/
static c3_o
_ce_patch_read_control(u3_ce_patch* pat_u)
{
  c3_w len_w;

  c3_assert(0 == pat_u->con_u);
  {
    struct stat buf_u;

    if ( -1 == fstat(pat_u->ctl_i, &buf_u) ) {
      c3_assert(0);
      return c3n;
    }
    len_w = (c3_w) buf_u.st_size;
  }

  pat_u->con_u = c3_malloc(len_w);
  if ( (len_w != read(pat_u->ctl_i, pat_u->con_u, len_w)) ||
        (len_w != sizeof(u3e_control) +
                  (pat_u->con_u->pgs_w * sizeof(u3e_line))) )
  {
    c3_free(pat_u->con_u);
    pat_u->con_u = 0;
    return c3n;
  }
  return c3y;
}

/* _ce_patch_create(): create patch files.
*/
static void
_ce_patch_create(u3_ce_patch* pat_u)
{
  c3_c ful_c[8193];

  snprintf(ful_c, 8192, "%s", u3P.dir_c);
  c3_mkdir(ful_c, 0700);

  snprintf(ful_c, 8192, "%s/.urb", u3P.dir_c);
  c3_mkdir(ful_c, 0700);

  snprintf(ful_c, 8192, "%s/.urb/chk/control.bin", u3P.dir_c);
  if ( -1 == (pat_u->ctl_i = c3_open(ful_c, O_RDWR | O_CREAT | O_EXCL, 0600)) ) {
    fprintf(stderr, "loom: patch c3_open control.bin: %s\r\n", strerror(errno));
    c3_assert(0);
  }

  snprintf(ful_c, 8192, "%s/.urb/chk/memory.bin", u3P.dir_c);
  if ( -1 == (pat_u->mem_i = c3_open(ful_c, O_RDWR | O_CREAT | O_EXCL, 0600)) ) {
    fprintf(stderr, "loom: patch c3_open memory.bin: %s\r\n", strerror(errno));
    c3_assert(0);
  }
}

/* _ce_patch_delete(): delete a patch.
*/
static void
_ce_patch_delete(void)
{
  c3_c ful_c[8193];

  snprintf(ful_c, 8192, "%s/.urb/chk/control.bin", u3P.dir_c);
  c3_unlink(ful_c);

  snprintf(ful_c, 8192, "%s/.urb/chk/memory.bin", u3P.dir_c);
  c3_unlink(ful_c);
}

/* _ce_patch_verify(): check patch data mug.
*/
static c3_o
_ce_patch_verify(u3_ce_patch* pat_u)
{
  c3_w i_w;

  if ( u3e_version != pat_u->con_u->ver_y ) {
    fprintf(stderr, "loom: patch version mismatch: have %u, need %u\r\n",
                    pat_u->con_u->ver_y,
                    u3e_version);
    return c3n;
  }

  for ( i_w = 0; i_w < pat_u->con_u->pgs_w; i_w++ ) {
    c3_w pag_w = pat_u->con_u->mem_u[i_w].pag_w;
    c3_w mug_w = pat_u->con_u->mem_u[i_w].mug_w;
    c3_w mem_w[1 << u3a_page];

    if ( -1 == lseek(pat_u->mem_i, (i_w << (u3a_page + 2)), SEEK_SET) ) {
      fprintf(stderr, "loom: patch seek: %s\r\n", strerror(errno));
      return c3n;
    }
    if ( -1 == read(pat_u->mem_i, mem_w, (1 << (u3a_page + 2))) ) {
      fprintf(stderr, "loom: patch read: %s\r\n", strerror(errno));
      return c3n;
    }
    {
      c3_w nug_w = u3r_mug_words(mem_w, (1 << u3a_page));

      if ( mug_w != nug_w ) {
        fprintf(stderr, "loom: patch mug mismatch %d/%d; (%x, %x)\r\n",
                        pag_w, i_w, mug_w, nug_w);
        return c3n;
      }
#if 0
      else {
        u3l_log("verify: patch %d/%d, %x\r\n", pag_w, i_w, mug_w);
      }
#endif
    }
  }
  return c3y;
}

/* _ce_patch_free(): free a patch.
*/
static void
_ce_patch_free(u3_ce_patch* pat_u)
{
  c3_free(pat_u->con_u);
  close(pat_u->ctl_i);
  close(pat_u->mem_i);
  c3_free(pat_u);
}

/* _ce_patch_open(): open patch, if any.
*/
static u3_ce_patch*
_ce_patch_open(void)
{
  u3_ce_patch* pat_u;
  c3_c ful_c[8193];
  c3_i ctl_i, mem_i;

  snprintf(ful_c, 8192, "%s", u3P.dir_c);
  c3_mkdir(ful_c, 0700);

  snprintf(ful_c, 8192, "%s/.urb", u3P.dir_c);
  c3_mkdir(ful_c, 0700);

  snprintf(ful_c, 8192, "%s/.urb/chk/control.bin", u3P.dir_c);
  if ( -1 == (ctl_i = c3_open(ful_c, O_RDWR)) ) {
    return 0;
  }

  snprintf(ful_c, 8192, "%s/.urb/chk/memory.bin", u3P.dir_c);
  if ( -1 == (mem_i = c3_open(ful_c, O_RDWR)) ) {
    close(ctl_i);

    _ce_patch_delete();
    return 0;
  }
  pat_u = c3_malloc(sizeof(u3_ce_patch));
  pat_u->ctl_i = ctl_i;
  pat_u->mem_i = mem_i;
  pat_u->con_u = 0;

  if ( c3n == _ce_patch_read_control(pat_u) ) {
    close(pat_u->ctl_i);
    close(pat_u->mem_i);
    c3_free(pat_u);

    _ce_patch_delete();
    return 0;
  }
  if ( c3n == _ce_patch_verify(pat_u) ) {
    _ce_patch_free(pat_u);
    _ce_patch_delete();
    return 0;
  }
  return pat_u;
}

/* _ce_patch_write_page(): write a page of patch memory.
*/
static void
_ce_patch_write_page(u3_ce_patch* pat_u,
                     c3_w         pgc_w,
                     c3_w*        mem_w)
{
  if ( -1 == lseek(pat_u->mem_i, (pgc_w << (u3a_page + 2)), SEEK_SET) ) {
    c3_assert(0);
  }
  if ( (1 << (u3a_page + 2)) !=
       write(pat_u->mem_i, mem_w, (1 << (u3a_page + 2))) )
  {
    c3_assert(0);
  }
}

/* _ce_patch_count_page(): count a page, producing new counter.
*/
static c3_w
_ce_patch_count_page(c3_w pag_w,
                     c3_w pgc_w)
{
  c3_w blk_w = (pag_w >> 5);
  c3_w bit_w = (pag_w & 31);

  if ( u3P.dit_w[blk_w] & (1 << bit_w) ) {
    pgc_w += 1;
  }
  return pgc_w;
}

/* _ce_patch_save_page(): save a page, producing new page counter.
*/
static c3_w
_ce_patch_save_page(u3_ce_patch* pat_u,
                    c3_w         pag_w,
                    c3_w         pgc_w)
{
  c3_w blk_w = (pag_w >> 5);
  c3_w bit_w = (pag_w & 31);

  if ( u3P.dit_w[blk_w] & (1 << bit_w) ) {
    c3_w* mem_w = u3_Loom + (pag_w << u3a_page);

    pat_u->con_u->mem_u[pgc_w].pag_w = pag_w;
    pat_u->con_u->mem_u[pgc_w].mug_w = u3r_mug_words(mem_w,
                                                       (1 << u3a_page));

#if 0
    u3l_log("protect a: page %d\r\n", pag_w);
#endif
    _ce_patch_write_page(pat_u, pgc_w, mem_w);

    if ( -1 == mprotect(u3_Loom + (pag_w << u3a_page),
                        (1 << (u3a_page + 2)),
                        PROT_READ) )
    {
      c3_assert(0);
    }

    u3P.dit_w[blk_w] &= ~(1 << bit_w);
    pgc_w += 1;
  }
  return pgc_w;
}

/* _ce_patch_compose(): make and write current patch.
*/
static u3_ce_patch*
_ce_patch_compose(void)
{
  c3_w pgs_w = 0;
  c3_w nor_w = 0;
  c3_w sou_w = 0;

  /* Calculate number of saved pages, north and south.
  */
  {
    c3_w nwr_w, swu_w;

    u3m_water(&nwr_w, &swu_w);

    nor_w = (nwr_w + ((1 << u3a_page) - 1)) >> u3a_page;
    sou_w = (swu_w + ((1 << u3a_page) - 1)) >> u3a_page;
  }

#ifdef U3_SNAPSHOT_VALIDATION
  u3K.nor_w = nor_w;
  u3K.sou_w = sou_w;
#endif

  /* Count dirty pages.
  */
  {
    c3_w i_w;

    for ( i_w = 0; i_w < nor_w; i_w++ ) {
      pgs_w = _ce_patch_count_page(i_w, pgs_w);
    }
    for ( i_w = 0; i_w < sou_w; i_w++ ) {
      pgs_w = _ce_patch_count_page((u3a_pages - (i_w + 1)), pgs_w);
    }
  }

  if ( !pgs_w ) {
    return 0;
  }
  else {
    u3_ce_patch* pat_u = c3_malloc(sizeof(u3_ce_patch));
    c3_w i_w, pgc_w;

    _ce_patch_create(pat_u);
    pat_u->con_u = c3_malloc(sizeof(u3e_control) + (pgs_w * sizeof(u3e_line)));
    pat_u->con_u->ver_y = u3e_version;
    pgc_w = 0;

    for ( i_w = 0; i_w < nor_w; i_w++ ) {
      pgc_w = _ce_patch_save_page(pat_u, i_w, pgc_w);
    }
    for ( i_w = 0; i_w < sou_w; i_w++ ) {
      pgc_w = _ce_patch_save_page(pat_u, (u3a_pages - (i_w + 1)), pgc_w);
    }

    pat_u->con_u->nor_w = nor_w;
    pat_u->con_u->sou_w = sou_w;
    pat_u->con_u->pgs_w = pgc_w;

    _ce_patch_write_control(pat_u);
    return pat_u;
  }
}

/* _ce_patch_sync(): make sure patch is synced to disk.
*/
static void
_ce_patch_sync(u3_ce_patch* pat_u)
{
  if ( -1 == c3_sync(pat_u->ctl_i) ) {
    fprintf(stderr, "loom: control file sync failed: %s\r\n",
                    strerror(errno));
    c3_assert(!"loom: control sync");
  }

  if ( -1 == c3_sync(pat_u->mem_i) ) {
    fprintf(stderr, "loom: patch file sync failed: %s\r\n",
                    strerror(errno));
    c3_assert(!"loom: patch sync");
  }
}

/* _ce_image_sync(): make sure image is synced to disk.
*/
static void
_ce_image_sync(u3e_image* img_u)
{
  if ( -1 == c3_sync(img_u->fid_i) ) {
    fprintf(stderr, "loom: image (%s) sync failed: %s\r\n",
                    img_u->nam_c,
                    strerror(errno));
    c3_assert(!"loom: image sync");
  }
}

/* _ce_image_resize(): resize image, truncating if it shrunk.
*/
static void
_ce_image_resize(u3e_image* img_u, c3_w pgs_w)
{
  if ( img_u->pgs_w > pgs_w ) {
    if ( ftruncate(img_u->fid_i, pgs_w << (u3a_page + 2)) ) {
      fprintf(stderr, "loom: image truncate %s: %s\r\n",
                      img_u->nam_c,
                      strerror(errno));
      c3_assert(0);
    }
  }

  img_u->pgs_w = pgs_w;
}

/* _ce_patch_apply(): apply patch to images.
*/
static void
_ce_patch_apply(u3_ce_patch* pat_u)
{
  c3_w i_w;

  //  resize images
  //
  _ce_image_resize(&u3P.nor_u, pat_u->con_u->nor_w);
  _ce_image_resize(&u3P.sou_u, pat_u->con_u->sou_w);

  //  seek to begining of patch and images
  //
  if (  (-1 == lseek(pat_u->mem_i, 0, SEEK_SET))
     || (-1 == lseek(u3P.nor_u.fid_i, 0, SEEK_SET))
     || (-1 == lseek(u3P.sou_u.fid_i, 0, SEEK_SET)) )
  {
    fprintf(stderr, "loom: patch apply seek 0: %s\r\n", strerror(errno));
    c3_assert(0);
  }

  //  write patch pages into the appropriate image
  //
  for ( i_w = 0; i_w < pat_u->con_u->pgs_w; i_w++ ) {
    c3_w pag_w = pat_u->con_u->mem_u[i_w].pag_w;
    c3_w mem_w[1 << u3a_page];
    c3_i fid_i;
    c3_w off_w;

    if ( pag_w < pat_u->con_u->nor_w ) {
      fid_i = u3P.nor_u.fid_i;
      off_w = pag_w;
    }
    else {
      fid_i = u3P.sou_u.fid_i;
      off_w = (u3a_pages - (pag_w + 1));
    }

    if ( -1 == read(pat_u->mem_i, mem_w, (1 << (u3a_page + 2))) ) {
      fprintf(stderr, "loom: patch apply read: %s\r\n", strerror(errno));
      c3_assert(0);
    }
    else {
      if ( -1 == lseek(fid_i, (off_w << (u3a_page + 2)), SEEK_SET) ) {
        fprintf(stderr, "loom: patch apply seek: %s\r\n", strerror(errno));
        c3_assert(0);
      }
      if ( -1 == write(fid_i, mem_w, (1 << (u3a_page + 2))) ) {
        fprintf(stderr, "loom: patch apply write: %s\r\n", strerror(errno));
        c3_assert(0);
      }
    }
#if 0
    u3l_log("apply: %d, %x\n", pag_w, u3r_mug_words(mem_w, (1 << u3a_page)));
#endif
  }
}

/* _ce_image_blit(): apply image to memory.
*/
static void
_ce_image_blit(u3e_image* img_u,
               c3_w*        ptr_w,
               c3_ws        stp_ws)
{
  if ( 0 == img_u->pgs_w ) {
    return;
  }

  c3_w i_w;
  c3_w siz_w = 1 << (u3a_page + 2);

  lseek(img_u->fid_i, 0, SEEK_SET);
  for ( i_w = 0; i_w < img_u->pgs_w; i_w++ ) {
    if ( -1 == read(img_u->fid_i, ptr_w, siz_w) ) {
      fprintf(stderr, "loom: image blit read: %s\r\n", strerror(errno));
      c3_assert(0);
    }

    if ( 0 != mprotect(ptr_w, siz_w, PROT_READ) ) {
      fprintf(stderr, "loom: live mprotect: %s\r\n", strerror(errno));
      c3_assert(0);
    }

    c3_w pag_w = u3a_outa(ptr_w) >> u3a_page;
    c3_w blk_w = pag_w >> 5;
    c3_w bit_w = pag_w & 31;
    u3P.dit_w[blk_w] &= ~(1 << bit_w);

    ptr_w += stp_ws;
  }
}

#ifdef U3_SNAPSHOT_VALIDATION
/* _ce_image_fine(): compare image to memory.
*/
static void
_ce_image_fine(u3e_image* img_u,
               c3_w*        ptr_w,
               c3_ws        stp_ws)
{
  c3_w i_w;
  c3_w buf_w[1 << u3a_page];

  lseek(img_u->fid_i, 0, SEEK_SET);
  for ( i_w=0; i_w < img_u->pgs_w; i_w++ ) {
    c3_w mem_w, fil_w;

    if ( -1 == read(img_u->fid_i, buf_w, (1 << (u3a_page + 2))) ) {
      fprintf(stderr, "loom: image fine read: %s\r\n", strerror(errno));
      c3_assert(0);
    }
    mem_w = u3r_mug_words(ptr_w, (1 << u3a_page));
    fil_w = u3r_mug_words(buf_w, (1 << u3a_page));

    if ( mem_w != fil_w ) {
      c3_w pag_w = (ptr_w - u3_Loom) >> u3a_page;

      fprintf(stderr, "mismatch: page %d, mem_w %x, fil_w %x, K %x\r\n",
                     pag_w,
                     mem_w,
                     fil_w,
                     u3K.mug_w[pag_w]);
      abort();
    }
    ptr_w += stp_ws;
  }
}
#endif

/* _ce_image_copy():
*/
static c3_o
_ce_image_copy(u3e_image* fom_u, u3e_image* tou_u)
{
  c3_w i_w;

  //  resize images
  //
  _ce_image_resize(tou_u, fom_u->pgs_w);

  //  seek to begining of patch and images
  //
  if (  (-1 == lseek(fom_u->fid_i, 0, SEEK_SET))
     || (-1 == lseek(tou_u->fid_i, 0, SEEK_SET)) )
  {
    fprintf(stderr, "loom: image copy seek 0: %s\r\n", strerror(errno));
    return c3n;
  }

  //  copy pages into destination image
  //
  for ( i_w = 0; i_w < fom_u->pgs_w; i_w++ ) {
    c3_w mem_w[1 << u3a_page];
    c3_w off_w = i_w;

    if ( -1 == read(fom_u->fid_i, mem_w, (1 << (u3a_page + 2))) ) {
      fprintf(stderr, "loom: image copy read: %s\r\n", strerror(errno));
      return c3n;
    }
    else {
      if ( -1 == lseek(tou_u->fid_i, (off_w << (u3a_page + 2)), SEEK_SET) ) {
        fprintf(stderr, "loom: image copy seek: %s\r\n", strerror(errno));
        return c3n;
      }
      if ( -1 == write(tou_u->fid_i, mem_w, (1 << (u3a_page + 2))) ) {
        fprintf(stderr, "loom: image copy write: %s\r\n", strerror(errno));
        return c3n;
      }
    }
  }

  return c3y;
}

/* _ce_backup();
*/
static void
_ce_backup(void)
{
  u3e_image nop_u = { .nam_c = "north", .pgs_w = 0 };
  u3e_image sop_u = { .nam_c = "south", .pgs_w = 0 };
  c3_i mod_i = O_RDWR | O_CREAT;
  c3_c ful_c[8193];

  snprintf(ful_c, 8192, "%s/.urb/bhk", u3P.dir_c);

  if ( c3_mkdir(ful_c, 0700) ) {
    if ( EEXIST != errno ) {
      fprintf(stderr, "loom: image backup: %s\r\n", strerror(errno));
    }
    return;
  }

  snprintf(ful_c, 8192, "%s/.urb/bhk/%s.bin", u3P.dir_c, nop_u.nam_c);

  if ( -1 == (nop_u.fid_i = c3_open(ful_c, mod_i, 0666)) ) {
    fprintf(stderr, "loom: c3_open %s: %s\r\n", ful_c, strerror(errno));
    return;
  }

  snprintf(ful_c, 8192, "%s/.urb/bhk/%s.bin", u3P.dir_c, sop_u.nam_c);

  if ( -1 == (sop_u.fid_i = c3_open(ful_c, mod_i, 0666)) ) {
    fprintf(stderr, "loom: c3_open %s: %s\r\n", ful_c, strerror(errno));
    return;
  }

  if (  (c3n == _ce_image_copy(&u3P.nor_u, &nop_u))
     || (c3n == _ce_image_copy(&u3P.sou_u, &sop_u)) )
  {

    c3_unlink(ful_c);
    snprintf(ful_c, 8192, "%s/.urb/bhk/%s.bin", u3P.dir_c, nop_u.nam_c);
    c3_unlink(ful_c);
    snprintf(ful_c, 8192, "%s/.urb/bhk", u3P.dir_c);
    c3_rmdir(ful_c);
  }

  close(nop_u.fid_i);
  close(sop_u.fid_i);
}

/*
  u3e_save(): save current changes.

  If we are in dry-run mode, do nothing.

  First, call `_ce_patch_compose` to write all dirty pages to disk and
  clear protection and dirty bits. If there were no dirty pages to write,
  then we're done.

  - Sync the patch files to disk.
  - Verify the patch (because why not?)
  - Write the patch data into the image file (This is idempotent.).
  - Sync the image file.
  - Delete the patchfile and free it.

  Once we've written the dirty pages to disk (and have reset their dirty bits
  and protection flags), we *could* handle the rest of the checkpointing
  process in a separate thread, but we'd need to wait until that finishes
  before we try to make another snapshot.
*/
void
u3e_save(void)
{
  u3_ce_patch* pat_u;

  if ( u3C.wag_w & u3o_dryrun ) {
    return;
  }

  if ( !(pat_u = _ce_patch_compose()) ) {
    return;
  }

  // u3a_print_memory(stderr, "sync: save", 4096 * pat_u->con_u->pgs_w);

  _ce_patch_sync(pat_u);

  if ( c3n == _ce_patch_verify(pat_u) ) {
    c3_assert(!"loom: save failed");
  }

  _ce_patch_apply(pat_u);

#ifdef U3_SNAPSHOT_VALIDATION
  {
    _ce_image_fine(&u3P.nor_u,
                   u3_Loom,
                   (1 << u3a_page));

    _ce_image_fine(&u3P.sou_u,
                   (u3_Loom + (1 << u3a_bits) - (1 << u3a_page)),
                   -(1 << u3a_page));

    c3_assert(u3P.nor_u.pgs_w == u3K.nor_w);
    c3_assert(u3P.sou_u.pgs_w == u3K.sou_w);
  }
#endif

  _ce_image_sync(&u3P.nor_u);
  _ce_image_sync(&u3P.sou_u);
  _ce_patch_free(pat_u);
  _ce_patch_delete();

  _ce_backup();
}

/* u3e_live(): start the checkpointing system.
*/
c3_o
u3e_live(c3_o nuu_o, c3_c* dir_c)
{
  //  require that our page size is a multiple of the system page size.
  //
  c3_assert(0 == (1 << (2 + u3a_page)) % sysconf(_SC_PAGESIZE));

  u3P.dir_c = dir_c;
  u3P.nor_u.nam_c = "north";
  u3P.sou_u.nam_c = "south";

  //  XX review dryrun requirements, enable or remove
  //
#if 0
  if ( u3C.wag_w & u3o_dryrun ) {
    return c3y;
  } else
#endif
  {
    //  Open image files.
    //
    if ( (c3n == _ce_image_open(&u3P.nor_u)) ||
         (c3n == _ce_image_open(&u3P.sou_u)) )
    {
      fprintf(stderr, "boot: image failed\r\n");
      exit(1);
    }
    else {
      u3_ce_patch* pat_u;

      /* Load any patch files; apply them to images.
      */
      if ( 0 != (pat_u = _ce_patch_open()) ) {
        _ce_patch_apply(pat_u);
        _ce_image_sync(&u3P.nor_u);
        _ce_image_sync(&u3P.sou_u);
        _ce_patch_free(pat_u);
        _ce_patch_delete();
      }

      //  mark all pages dirty (pages in the snapshot will be marked clean)
      //
      u3e_foul();

      /* Write image files to memory; reinstate protection.
      */
      {
        _ce_image_blit(&u3P.nor_u,
                       u3_Loom,
                       (1 << u3a_page));

        _ce_image_blit(&u3P.sou_u,
                       (u3_Loom + (1 << u3a_bits) - (1 << u3a_page)),
                       -(1 << u3a_page));

        u3l_log("boot: protected loom\r\n");
      }

      /* If the images were empty, we are logically booting.
      */
      if ( (0 == u3P.nor_u.pgs_w) && (0 == u3P.sou_u.pgs_w) ) {
        u3l_log("live: logical boot\r\n");
        nuu_o = c3y;
      }
      else {
        u3a_print_memory(stderr, "live: loaded",
                         (u3P.nor_u.pgs_w + u3P.sou_u.pgs_w) << u3a_page);
      }
    }
  }
  return nuu_o;
}

/* u3e_yolo(): disable dirty page tracking, read/write whole loom.
*/
c3_o
u3e_yolo(void)
{
  //    NB: u3e_save() will reinstate protection flags
  //
  if ( 0 != mprotect((void *)u3_Loom, u3a_bytes, (PROT_READ | PROT_WRITE)) ) {
    return c3n;
  }

  return c3y;
}

/* u3e_foul(): dirty all the pages of the loom.
*/
void
u3e_foul(void)
{
  memset((void*)u3P.dit_w, 0xff, sizeof(u3P.dit_w));
}
