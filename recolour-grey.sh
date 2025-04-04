#!/bin/sh
#
# Adjust the colours of an image to be grey, not black. We simply change the
# values of the pixels to be at least hexadecimal C.
#

sed 's/0x[0-9]/0xA/g' | sed 's/0x\(.\)[0-9]/0x\1A/g'
