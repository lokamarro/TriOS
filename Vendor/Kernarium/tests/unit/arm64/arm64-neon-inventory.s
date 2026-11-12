// SPDX-License-Identifier: GPL-2.0-or-later
// ARMv8-A Advanced SIMD instruction inventory used to audit the static decoder.
// Each line deliberately uses non-alias register operands so llvm-mc exposes the
// canonical encoding family.  The common Armv8 cryptographic extension is
// included; FP16, dot-product, RDM, SVE and SME remain outside this inventory.

.text
.arch armv8-a
.p2align 2
.globl _uml_arm64_neon_inventory_begin
_uml_arm64_neon_inventory_begin:

// Three-register same-width integer arithmetic and comparisons.
shadd v0.8b, v1.8b, v2.8b
uhadd v0.8b, v1.8b, v2.8b
sqadd v0.8b, v1.8b, v2.8b
uqadd v0.8b, v1.8b, v2.8b
srhadd v0.8b, v1.8b, v2.8b
urhadd v0.8b, v1.8b, v2.8b
shsub v0.8b, v1.8b, v2.8b
uhsub v0.8b, v1.8b, v2.8b
sqsub v0.8b, v1.8b, v2.8b
uqsub v0.8b, v1.8b, v2.8b
sshl v0.8b, v1.8b, v2.8b
ushl v0.8b, v1.8b, v2.8b
sqshl v0.8b, v1.8b, v2.8b
uqshl v0.8b, v1.8b, v2.8b
srshl v0.8b, v1.8b, v2.8b
urshl v0.8b, v1.8b, v2.8b
sqrshl v0.8b, v1.8b, v2.8b
uqrshl v0.8b, v1.8b, v2.8b
smax v0.8b, v1.8b, v2.8b
umax v0.8b, v1.8b, v2.8b
smin v0.8b, v1.8b, v2.8b
umin v0.8b, v1.8b, v2.8b
sabd v0.8b, v1.8b, v2.8b
uabd v0.8b, v1.8b, v2.8b
saba v0.8b, v1.8b, v2.8b
uaba v0.8b, v1.8b, v2.8b
add v0.8b, v1.8b, v2.8b
sub v0.8b, v1.8b, v2.8b
cmtst v0.8b, v1.8b, v2.8b
cmeq v0.8b, v1.8b, v2.8b
cmgt v0.8b, v1.8b, v2.8b
cmge v0.8b, v1.8b, v2.8b
cmhi v0.8b, v1.8b, v2.8b
cmhs v0.8b, v1.8b, v2.8b
mla v0.8b, v1.8b, v2.8b
mls v0.8b, v1.8b, v2.8b
mul v0.8b, v1.8b, v2.8b
pmul v0.8b, v1.8b, v2.8b
smaxp v0.8b, v1.8b, v2.8b
umaxp v0.8b, v1.8b, v2.8b
sminp v0.8b, v1.8b, v2.8b
uminp v0.8b, v1.8b, v2.8b
addp v0.8b, v1.8b, v2.8b
sqdmulh v0.4h, v1.4h, v2.4h
sqrdmulh v0.4h, v1.4h, v2.4h

// Three-register widening and narrowing arithmetic.
saddl v0.8h, v1.8b, v2.8b
uaddl v0.8h, v1.8b, v2.8b
saddw v0.8h, v1.8h, v2.8b
uaddw v0.8h, v1.8h, v2.8b
ssubl v0.8h, v1.8b, v2.8b
usubl v0.8h, v1.8b, v2.8b
ssubw v0.8h, v1.8h, v2.8b
usubw v0.8h, v1.8h, v2.8b
addhn v0.8b, v1.8h, v2.8h
raddhn v0.8b, v1.8h, v2.8h
subhn v0.8b, v1.8h, v2.8h
rsubhn v0.8b, v1.8h, v2.8h
sabdl v0.8h, v1.8b, v2.8b
uabdl v0.8h, v1.8b, v2.8b
sabal v0.8h, v1.8b, v2.8b
uabal v0.8h, v1.8b, v2.8b
smlal v0.8h, v1.8b, v2.8b
umlal v0.8h, v1.8b, v2.8b
smlsl v0.8h, v1.8b, v2.8b
umlsl v0.8h, v1.8b, v2.8b
smull v0.8h, v1.8b, v2.8b
umull v0.8h, v1.8b, v2.8b
sqdmlal v0.4s, v1.4h, v2.4h
sqdmlsl v0.4s, v1.4h, v2.4h
sqdmull v0.4s, v1.4h, v2.4h

// Two-register integer, narrowing, widening and reciprocal estimate forms.
umov w0, v1.b[7]
umov x0, v1.d[1]
smov w0, v1.b[7]
smov x0, v1.s[3]
rev64 v0.8b, v1.8b
rev32 v0.8b, v1.8b
rev16 v0.8b, v1.8b
saddlp v0.4h, v1.8b
uaddlp v0.4h, v1.8b
sadalp v0.4h, v1.8b
uadalp v0.4h, v1.8b
suqadd v0.8b, v1.8b
usqadd v0.8b, v1.8b
cls v0.8b, v1.8b
clz v0.8b, v1.8b
cnt v0.8b, v1.8b
not v0.8b, v1.8b
rbit v0.8b, v1.8b
sqabs v0.8b, v1.8b
sqneg v0.8b, v1.8b
abs v0.8b, v1.8b
neg v0.8b, v1.8b
cmgt v0.8b, v1.8b, #0
cmge v0.8b, v1.8b, #0
cmeq v0.8b, v1.8b, #0
cmle v0.8b, v1.8b, #0
cmlt v0.8b, v1.8b, #0
xtn v0.8b, v1.8h
sqxtn v0.8b, v1.8h
uqxtn v0.8b, v1.8h
sqxtun v0.8b, v1.8h
shll v0.8h, v1.8b, #8
urecpe v0.2s, v1.2s
ursqrte v0.2s, v1.2s

// Across-lane reductions.
saddlv h0, v1.8b
uaddlv h0, v1.8b
smaxv b0, v1.8b
umaxv b0, v1.8b
sminv b0, v1.8b
uminv b0, v1.8b
addv b0, v1.8b

// Floating point vector arithmetic, comparisons, estimates and reductions.
fmaxnm v0.2s, v1.2s, v2.2s
fminnm v0.2s, v1.2s, v2.2s
fmla v0.2s, v1.2s, v2.2s
fmls v0.2s, v1.2s, v2.2s
fadd v0.2s, v1.2s, v2.2s
fsub v0.2s, v1.2s, v2.2s
faddp v0.2s, v1.2s, v2.2s
fabd v0.2s, v1.2s, v2.2s
fmul v0.2s, v1.2s, v2.2s
fmulx v0.2s, v1.2s, v2.2s
fcmeq v0.2s, v1.2s, v2.2s
fcmge v0.2s, v1.2s, v2.2s
fcmgt v0.2s, v1.2s, v2.2s
facge v0.2s, v1.2s, v2.2s
facgt v0.2s, v1.2s, v2.2s
fmax v0.2s, v1.2s, v2.2s
fmin v0.2s, v1.2s, v2.2s
fmaxnmp v0.2s, v1.2s, v2.2s
fminnmp v0.2s, v1.2s, v2.2s
fmaxp v0.2s, v1.2s, v2.2s
fminp v0.2s, v1.2s, v2.2s
fdiv v0.2s, v1.2s, v2.2s
frecps v0.2s, v1.2s, v2.2s
frsqrts v0.2s, v1.2s, v2.2s
fabs v0.2s, v1.2s
fneg v0.2s, v1.2s
fsqrt v0.2s, v1.2s
frecpe v0.2s, v1.2s
frsqrte v0.2s, v1.2s
fcmeq v0.2s, v1.2s, #0.0
fcmge v0.2s, v1.2s, #0.0
fcmgt v0.2s, v1.2s, #0.0
fcmle v0.2s, v1.2s, #0.0
fcmlt v0.2s, v1.2s, #0.0
frintn v0.2s, v1.2s
frintp v0.2s, v1.2s
frintm v0.2s, v1.2s
frintz v0.2s, v1.2s
frinta v0.2s, v1.2s
frintx v0.2s, v1.2s
frinti v0.2s, v1.2s
fcvtns v0.2s, v1.2s
fcvtnu v0.2s, v1.2s
fcvtps v0.2s, v1.2s
fcvtpu v0.2s, v1.2s
fcvtms v0.2s, v1.2s
fcvtmu v0.2s, v1.2s
fcvtas v0.2s, v1.2s
fcvtau v0.2s, v1.2s
fcvtzs v0.2s, v1.2s
fcvtzu v0.2s, v1.2s
scvtf v0.2s, v1.2s
ucvtf v0.2s, v1.2s
fcvtn v0.2s, v1.2d
fcvtl v0.2d, v1.2s
fcvtxn v0.2s, v1.2d
fmaxnmv s0, v1.4s
fminnmv s0, v1.4s
fmaxv s0, v1.4s
fminv s0, v1.4s

// Immediate shifts, insertion, fixed-point conversion, and saturation.
sshr v0.8b, v1.8b, #1
ushr v0.8b, v1.8b, #1
ssra v0.8b, v1.8b, #1
usra v0.8b, v1.8b, #1
srshr v0.8b, v1.8b, #1
urshr v0.8b, v1.8b, #1
srsra v0.8b, v1.8b, #1
ursra v0.8b, v1.8b, #1
sri v0.8b, v1.8b, #1
shl v0.8b, v1.8b, #1
sli v0.8b, v1.8b, #1
sqshl v0.8b, v1.8b, #1
uqshl v0.8b, v1.8b, #1
sqshlu v0.8b, v1.8b, #1
shrn v0.8b, v1.8h, #1
rshrn v0.8b, v1.8h, #1
sqshrn v0.8b, v1.8h, #1
uqshrn v0.8b, v1.8h, #1
sqshrun v0.8b, v1.8h, #1
sqrshrn v0.8b, v1.8h, #1
uqrshrn v0.8b, v1.8h, #1
sqrshrun v0.8b, v1.8h, #1
sshll v0.8h, v1.8b, #1
ushll v0.8h, v1.8b, #1
scvtf v0.2s, v1.2s, #1
ucvtf v0.2s, v1.2s, #1
fcvtzs v0.2s, v1.2s, #1
fcvtzu v0.2s, v1.2s, #1

// Vector by-indexed-element multiply families.
mla v0.4h, v1.4h, v2.h[1]
mls v0.4h, v1.4h, v2.h[1]
mul v0.4h, v1.4h, v2.h[1]
smlal v0.4s, v1.4h, v2.h[1]
umlal v0.4s, v1.4h, v2.h[1]
smlsl v0.4s, v1.4h, v2.h[1]
umlsl v0.4s, v1.4h, v2.h[1]
smull v0.4s, v1.4h, v2.h[1]
umull v0.4s, v1.4h, v2.h[1]
sqdmlal v0.4s, v1.4h, v2.h[1]
sqdmlsl v0.4s, v1.4h, v2.h[1]
sqdmull v0.4s, v1.4h, v2.h[1]
sqdmulh v0.4h, v1.4h, v2.h[1]
sqrdmulh v0.4h, v1.4h, v2.h[1]
fmla v0.4s, v1.4s, v2.s[1]
fmls v0.4s, v1.4s, v2.s[1]
fmul v0.4s, v1.4s, v2.s[1]
fmulx v0.4s, v1.4s, v2.s[1]

// Scalar Advanced SIMD integer families.
sqadd b0, b1, b2
uqadd b0, b1, b2
sqsub b0, b1, b2
uqsub b0, b1, b2
sshl d0, d1, d2
ushl d0, d1, d2
sqshl b0, b1, b2
uqshl b0, b1, b2
srshl d0, d1, d2
urshl d0, d1, d2
sqrshl b0, b1, b2
uqrshl b0, b1, b2
add d0, d1, d2
sub d0, d1, d2
cmtst d0, d1, d2
cmeq d0, d1, d2
cmgt d0, d1, d2
cmge d0, d1, d2
cmhi d0, d1, d2
cmhs d0, d1, d2
sqdmulh h0, h1, h2
sqrdmulh h0, h1, h2
sqabs b0, b1
sqneg b0, b1
abs d0, d1
neg d0, d1
cmgt d0, d1, #0
cmge d0, d1, #0
cmeq d0, d1, #0
cmle d0, d1, #0
cmlt d0, d1, #0
sqxtn b0, h1
uqxtn b0, h1
sqxtun b0, h1
sqdmlal s0, h1, h2
sqdmlsl s0, h1, h2
sqdmull s0, h1, h2
addp d0, v1.2d

// Scalar immediate shifts and indexed multiply forms.
sshr d0, d1, #1
ushr d0, d1, #1
ssra d0, d1, #1
usra d0, d1, #1
srshr d0, d1, #1
urshr d0, d1, #1
srsra d0, d1, #1
ursra d0, d1, #1
sri d0, d1, #1
shl d0, d1, #1
sli d0, d1, #1
sqshl b0, b1, #1
uqshl b0, b1, #1
sqshlu b0, b1, #1
sqshrn b0, h1, #1
uqshrn b0, h1, #1
sqshrun b0, h1, #1
sqrshrn b0, h1, #1
uqrshrn b0, h1, #1
sqrshrun b0, h1, #1
scvtf s0, s1, #1
ucvtf s0, s1, #1
fcvtzs s0, s1, #1
fcvtzu s0, s1, #1
sqdmlal s0, h1, v2.h[1]
sqdmlsl s0, h1, v2.h[1]
sqdmull s0, h1, v2.h[1]
sqdmulh h0, h1, v2.h[1]
sqrdmulh h0, h1, v2.h[1]
fmla s0, s1, v2.s[1]
fmls s0, s1, v2.s[1]
fmul s0, s1, v2.s[1]
fmulx s0, s1, v2.s[1]

// Scalar floating pairwise/reduction and reciprocal forms.
faddp s0, v1.2s
fmaxnmp s0, v1.2s
fminnmp s0, v1.2s
fmaxp s0, v1.2s
fminp s0, v1.2s
frecpe s0, s1
frecpx s0, s1
frsqrte s0, s1

// Multiple-structure interleaving forms (lane and replicate forms are covered
// by the same single-structure decoder class).
ld2 {v0.8b, v1.8b}, [x0]
st2 {v0.8b, v1.8b}, [x0]
ld3 {v0.8b, v1.8b, v2.8b}, [x0]
st3 {v0.8b, v1.8b, v2.8b}, [x0]
ld4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x0]
st4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x0]

// Common TLS cryptography and hash acceleration.
.arch armv8-a+crypto
aese v0.16b, v1.16b
aesd v0.16b, v1.16b
aesmc v0.16b, v1.16b
aesimc v0.16b, v1.16b
pmull v0.8h, v1.8b, v2.8b
pmull2 v0.8h, v1.16b, v2.16b
pmull v0.1q, v1.1d, v2.1d
pmull2 v0.1q, v1.2d, v2.2d
sha1c q0, s1, v2.4s
sha1p q0, s1, v2.4s
sha1m q0, s1, v2.4s
sha1h s0, s1
sha1su0 v0.4s, v1.4s, v2.4s
sha1su1 v0.4s, v1.4s
sha256h q0, q1, v2.4s
sha256h2 q0, q1, v2.4s
sha256su0 v0.4s, v1.4s
sha256su1 v0.4s, v1.4s, v2.4s

// Armv8.2 SHA-512 and SHA-3 acceleration.
.arch armv8.2-a+sha3
sha512h q0, q1, v2.2d
sha512h2 q0, q1, v2.2d
sha512su0 v0.2d, v1.2d
sha512su1 v0.2d, v1.2d, v2.2d
eor3 v0.16b, v1.16b, v2.16b, v3.16b
rax1 v0.2d, v1.2d, v2.2d
xar v0.2d, v1.2d, v2.2d, #17
bcax v0.16b, v1.16b, v2.16b, v3.16b

// Armv8 CRC32 and CRC32C scalar accumulation.
.arch armv8-a+crc
crc32b w0, w1, w2
crc32h w0, w1, w2
crc32w w0, w1, w2
crc32x w0, w1, x2
crc32cb w0, w1, w2
crc32ch w0, w1, w2
crc32cw w0, w1, w2
crc32cx w0, w1, x2
.globl _uml_arm64_neon_inventory_end
_uml_arm64_neon_inventory_end:
