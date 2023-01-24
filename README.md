# kexec-multiboot
A multiboot kernel solution for Vuplus boxes

details at:
https://board.openbh.net/threads/vu-real-multiboot-now-available.3077/

needed patches:
[kexec-multiboot] tagged patch at the following repository

- ofgwrite (required to flash slots)
https://github.com/oe-alliance/ofgwrite/pull/14

- image manager 
https://github.com/BlackHole/obh-core/commits/master

- enigma2
https://github.com/BlackHole/enigma2/commits/Python3.11
