PM_MMAP_EXCLUSIVE = 1 << 56
PM_FILE           = 1 << 61
PM_SWAP           = 1 << 62
PM_PRESENT        = 1 << 63

def dirty(i):
  if i & (1 << 55):
    print("pte is soft-dirty")
  if i & PM_MMAP_EXCLUSIVE:
    print("page exclusively mapped (PM_MMAP_EXCLUSIVE)")
  if i & PM_FILE:
    print("page is file-page or shared-anon (PM_FILE)")
  if i & PM_SWAP:
    print("page swapped (PM_SWAP)")
  if i & PM_PRESENT:
    print("page present (PM_PRESENT)")
