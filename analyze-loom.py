#!/bin/env python
from sys import argv, exit
import bitstring

if len(argv) != 2:
    print("usage: analyze-loom.py <folder>")
    exit(1)

def loom(i):
  return bitstring.Bits(filename=f"{argv[1]}/{i:08}.bin")

i = 1
old = loom(0)
while True:
  try:
    new = loom(i)
  except FileNotFoundError:
    break

  print("in snapshot", i,
    "dirtied", ", ".join(map(str, (new & ~old).findall("0b1"))),
    "cleaned", ", ".join(map(str, (old & ~new).findall("0b1"))))

  i += 1
  old = new
