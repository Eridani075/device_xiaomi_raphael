/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Replacement Thumb blocks for haydn libqcodec2_utils.so, source SHA-1
 * 63911243cc5a97f4d1ce46d0be030ccb1afc7c32. Addresses below are original VAs;
 * the executable segment's file offsets are VA - 0x1000.
 *
 * Ordinary VENUS_NV12 (0x7fa30c04) uses 128-byte stride and 32 scanlines.
 * The shared low-valued 0x114/0x116 paths retain their 512-byte/line layout.
 * RGB/UBWC/P010 and other switch destinations are outside these blocks.
 *
 * Assemble with llvm-mc -triple=thumbv7a-linux-gnueabi -filetype=obj.
 * extract-files.py embeds the 50-byte size block and 34-byte Y-size block,
 * padded with Thumb NOPs. It also retargets the calcStride VENUS_NV12 TBB
 * entry at VA 0x24ab4 from 0x13 (512) to 0x04 (existing 128-byte path).
 */

.syntax unified
.thumb

.section .text.nv12_size,"ax",%progbits
.org 0x23ff0
size_start:
    movw r0, #511
    mov r1, r0
    cmp.w r5, #0x116
    itt hi
    movhi r0, #127
    movhi r1, #31
    add r2, r0
    bics r2, r0
    add r3, r1
    bics r3, r1
    muls r2, r3
    add.w r4, r2, r2, lsr #1
    movw r0, #4095
    add r4, r0
    bics r4, r0
    b.w size_return
size_end:
.org 0x242ea
size_return:
    nop

.section .text.nv12_y_size,"ax",%progbits
.org 0x24418
y_size_start:
    movw r0, #511
    mov r1, r0
    cmp.w r4, #0x116
    itt hi
    movhi r0, #127
    movhi r1, #31
    add r2, r0
    bics r2, r0
    add r3, r1
    bics r3, r1
    muls r2, r3
    mov r0, r2
    b.w y_size_return
y_size_end:
.org 0x24502
y_size_return:
    nop
