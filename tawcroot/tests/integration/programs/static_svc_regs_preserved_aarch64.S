/* Exit 42 iff a trapped syscall (getuid, 256 times from one site) leaves
 * every register but x0 untouched: x1-x30, NZCV and all of v0-v31.
 * Repeating the site lets a fastpath build (src/fastpath.c) rewrite it
 * part way, so both the SIGSYS and the direct path are checked.
 * Failure: 100+n for xn, 140+n for vn, 99 for NZCV.
 */

	.text
	.globl _start
	.type  _start, %function
_start:
	mov     x28, #256
.Lloop:
	mov     x0, #23040
	movk    x0, #0xc3c3, lsl #48
	dup     v0.2d, x0
	mov     x0, #23041
	movk    x0, #0xc3c3, lsl #48
	dup     v1.2d, x0
	mov     x0, #23042
	movk    x0, #0xc3c3, lsl #48
	dup     v2.2d, x0
	mov     x0, #23043
	movk    x0, #0xc3c3, lsl #48
	dup     v3.2d, x0
	mov     x0, #23044
	movk    x0, #0xc3c3, lsl #48
	dup     v4.2d, x0
	mov     x0, #23045
	movk    x0, #0xc3c3, lsl #48
	dup     v5.2d, x0
	mov     x0, #23046
	movk    x0, #0xc3c3, lsl #48
	dup     v6.2d, x0
	mov     x0, #23047
	movk    x0, #0xc3c3, lsl #48
	dup     v7.2d, x0
	mov     x0, #23048
	movk    x0, #0xc3c3, lsl #48
	dup     v8.2d, x0
	mov     x0, #23049
	movk    x0, #0xc3c3, lsl #48
	dup     v9.2d, x0
	mov     x0, #23050
	movk    x0, #0xc3c3, lsl #48
	dup     v10.2d, x0
	mov     x0, #23051
	movk    x0, #0xc3c3, lsl #48
	dup     v11.2d, x0
	mov     x0, #23052
	movk    x0, #0xc3c3, lsl #48
	dup     v12.2d, x0
	mov     x0, #23053
	movk    x0, #0xc3c3, lsl #48
	dup     v13.2d, x0
	mov     x0, #23054
	movk    x0, #0xc3c3, lsl #48
	dup     v14.2d, x0
	mov     x0, #23055
	movk    x0, #0xc3c3, lsl #48
	dup     v15.2d, x0
	mov     x0, #23056
	movk    x0, #0xc3c3, lsl #48
	dup     v16.2d, x0
	mov     x0, #23057
	movk    x0, #0xc3c3, lsl #48
	dup     v17.2d, x0
	mov     x0, #23058
	movk    x0, #0xc3c3, lsl #48
	dup     v18.2d, x0
	mov     x0, #23059
	movk    x0, #0xc3c3, lsl #48
	dup     v19.2d, x0
	mov     x0, #23060
	movk    x0, #0xc3c3, lsl #48
	dup     v20.2d, x0
	mov     x0, #23061
	movk    x0, #0xc3c3, lsl #48
	dup     v21.2d, x0
	mov     x0, #23062
	movk    x0, #0xc3c3, lsl #48
	dup     v22.2d, x0
	mov     x0, #23063
	movk    x0, #0xc3c3, lsl #48
	dup     v23.2d, x0
	mov     x0, #23064
	movk    x0, #0xc3c3, lsl #48
	dup     v24.2d, x0
	mov     x0, #23065
	movk    x0, #0xc3c3, lsl #48
	dup     v25.2d, x0
	mov     x0, #23066
	movk    x0, #0xc3c3, lsl #48
	dup     v26.2d, x0
	mov     x0, #23067
	movk    x0, #0xc3c3, lsl #48
	dup     v27.2d, x0
	mov     x0, #23068
	movk    x0, #0xc3c3, lsl #48
	dup     v28.2d, x0
	mov     x0, #23069
	movk    x0, #0xc3c3, lsl #48
	dup     v29.2d, x0
	mov     x0, #23070
	movk    x0, #0xc3c3, lsl #48
	dup     v30.2d, x0
	mov     x0, #23071
	movk    x0, #0xc3c3, lsl #48
	dup     v31.2d, x0
	mov     x1, #4097
	movk    x1, #0xa5a5, lsl #48
	mov     x2, #4098
	movk    x2, #0xa5a5, lsl #48
	mov     x3, #4099
	movk    x3, #0xa5a5, lsl #48
	mov     x4, #4100
	movk    x4, #0xa5a5, lsl #48
	mov     x5, #4101
	movk    x5, #0xa5a5, lsl #48
	mov     x6, #4102
	movk    x6, #0xa5a5, lsl #48
	mov     x7, #4103
	movk    x7, #0xa5a5, lsl #48
	mov     x9, #4105
	movk    x9, #0xa5a5, lsl #48
	mov     x10, #4106
	movk    x10, #0xa5a5, lsl #48
	mov     x11, #4107
	movk    x11, #0xa5a5, lsl #48
	mov     x12, #4108
	movk    x12, #0xa5a5, lsl #48
	mov     x13, #4109
	movk    x13, #0xa5a5, lsl #48
	mov     x14, #4110
	movk    x14, #0xa5a5, lsl #48
	mov     x15, #4111
	movk    x15, #0xa5a5, lsl #48
	mov     x16, #4112
	movk    x16, #0xa5a5, lsl #48
	mov     x17, #4113
	movk    x17, #0xa5a5, lsl #48
	mov     x18, #4114
	movk    x18, #0xa5a5, lsl #48
	mov     x19, #4115
	movk    x19, #0xa5a5, lsl #48
	mov     x20, #4116
	movk    x20, #0xa5a5, lsl #48
	mov     x21, #4117
	movk    x21, #0xa5a5, lsl #48
	mov     x22, #4118
	movk    x22, #0xa5a5, lsl #48
	mov     x23, #4119
	movk    x23, #0xa5a5, lsl #48
	mov     x24, #4120
	movk    x24, #0xa5a5, lsl #48
	mov     x25, #4121
	movk    x25, #0xa5a5, lsl #48
	mov     x26, #4122
	movk    x26, #0xa5a5, lsl #48
	mov     x27, #4123
	movk    x27, #0xa5a5, lsl #48
	mov     x29, #4125
	movk    x29, #0xa5a5, lsl #48
	mov     x30, #4126
	movk    x30, #0xa5a5, lsl #48
	mov     x0, #0xa0000000
	msr     nzcv, x0
	mov     x8, #174                 /* __NR_getuid */
	svc     #0
	b.pl    .Lnzcv
	b.cc    .Lnzcv
	b.eq    .Lnzcv
	b.vs    .Lnzcv
	cmp     x8, #174
	b.ne    .Lx8
	mov     x0, #4097
	movk    x0, #0xa5a5, lsl #48
	cmp     x1, x0
	b.ne    .Lx1
	mov     x0, #4098
	movk    x0, #0xa5a5, lsl #48
	cmp     x2, x0
	b.ne    .Lx2
	mov     x0, #4099
	movk    x0, #0xa5a5, lsl #48
	cmp     x3, x0
	b.ne    .Lx3
	mov     x0, #4100
	movk    x0, #0xa5a5, lsl #48
	cmp     x4, x0
	b.ne    .Lx4
	mov     x0, #4101
	movk    x0, #0xa5a5, lsl #48
	cmp     x5, x0
	b.ne    .Lx5
	mov     x0, #4102
	movk    x0, #0xa5a5, lsl #48
	cmp     x6, x0
	b.ne    .Lx6
	mov     x0, #4103
	movk    x0, #0xa5a5, lsl #48
	cmp     x7, x0
	b.ne    .Lx7
	mov     x0, #4105
	movk    x0, #0xa5a5, lsl #48
	cmp     x9, x0
	b.ne    .Lx9
	mov     x0, #4106
	movk    x0, #0xa5a5, lsl #48
	cmp     x10, x0
	b.ne    .Lx10
	mov     x0, #4107
	movk    x0, #0xa5a5, lsl #48
	cmp     x11, x0
	b.ne    .Lx11
	mov     x0, #4108
	movk    x0, #0xa5a5, lsl #48
	cmp     x12, x0
	b.ne    .Lx12
	mov     x0, #4109
	movk    x0, #0xa5a5, lsl #48
	cmp     x13, x0
	b.ne    .Lx13
	mov     x0, #4110
	movk    x0, #0xa5a5, lsl #48
	cmp     x14, x0
	b.ne    .Lx14
	mov     x0, #4111
	movk    x0, #0xa5a5, lsl #48
	cmp     x15, x0
	b.ne    .Lx15
	mov     x0, #4112
	movk    x0, #0xa5a5, lsl #48
	cmp     x16, x0
	b.ne    .Lx16
	mov     x0, #4113
	movk    x0, #0xa5a5, lsl #48
	cmp     x17, x0
	b.ne    .Lx17
	mov     x0, #4114
	movk    x0, #0xa5a5, lsl #48
	cmp     x18, x0
	b.ne    .Lx18
	mov     x0, #4115
	movk    x0, #0xa5a5, lsl #48
	cmp     x19, x0
	b.ne    .Lx19
	mov     x0, #4116
	movk    x0, #0xa5a5, lsl #48
	cmp     x20, x0
	b.ne    .Lx20
	mov     x0, #4117
	movk    x0, #0xa5a5, lsl #48
	cmp     x21, x0
	b.ne    .Lx21
	mov     x0, #4118
	movk    x0, #0xa5a5, lsl #48
	cmp     x22, x0
	b.ne    .Lx22
	mov     x0, #4119
	movk    x0, #0xa5a5, lsl #48
	cmp     x23, x0
	b.ne    .Lx23
	mov     x0, #4120
	movk    x0, #0xa5a5, lsl #48
	cmp     x24, x0
	b.ne    .Lx24
	mov     x0, #4121
	movk    x0, #0xa5a5, lsl #48
	cmp     x25, x0
	b.ne    .Lx25
	mov     x0, #4122
	movk    x0, #0xa5a5, lsl #48
	cmp     x26, x0
	b.ne    .Lx26
	mov     x0, #4123
	movk    x0, #0xa5a5, lsl #48
	cmp     x27, x0
	b.ne    .Lx27
	mov     x0, #4125
	movk    x0, #0xa5a5, lsl #48
	cmp     x29, x0
	b.ne    .Lx29
	mov     x0, #4126
	movk    x0, #0xa5a5, lsl #48
	cmp     x30, x0
	b.ne    .Lx30
	mov     x1, #23040
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d0
	cmp     x0, x1
	b.ne    .Lv0
	umov    x0, v0.d[1]
	cmp     x0, x1
	b.ne    .Lv0
	mov     x1, #23041
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d1
	cmp     x0, x1
	b.ne    .Lv1
	umov    x0, v1.d[1]
	cmp     x0, x1
	b.ne    .Lv1
	mov     x1, #23042
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d2
	cmp     x0, x1
	b.ne    .Lv2
	umov    x0, v2.d[1]
	cmp     x0, x1
	b.ne    .Lv2
	mov     x1, #23043
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d3
	cmp     x0, x1
	b.ne    .Lv3
	umov    x0, v3.d[1]
	cmp     x0, x1
	b.ne    .Lv3
	mov     x1, #23044
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d4
	cmp     x0, x1
	b.ne    .Lv4
	umov    x0, v4.d[1]
	cmp     x0, x1
	b.ne    .Lv4
	mov     x1, #23045
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d5
	cmp     x0, x1
	b.ne    .Lv5
	umov    x0, v5.d[1]
	cmp     x0, x1
	b.ne    .Lv5
	mov     x1, #23046
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d6
	cmp     x0, x1
	b.ne    .Lv6
	umov    x0, v6.d[1]
	cmp     x0, x1
	b.ne    .Lv6
	mov     x1, #23047
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d7
	cmp     x0, x1
	b.ne    .Lv7
	umov    x0, v7.d[1]
	cmp     x0, x1
	b.ne    .Lv7
	mov     x1, #23048
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d8
	cmp     x0, x1
	b.ne    .Lv8
	umov    x0, v8.d[1]
	cmp     x0, x1
	b.ne    .Lv8
	mov     x1, #23049
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d9
	cmp     x0, x1
	b.ne    .Lv9
	umov    x0, v9.d[1]
	cmp     x0, x1
	b.ne    .Lv9
	mov     x1, #23050
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d10
	cmp     x0, x1
	b.ne    .Lv10
	umov    x0, v10.d[1]
	cmp     x0, x1
	b.ne    .Lv10
	mov     x1, #23051
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d11
	cmp     x0, x1
	b.ne    .Lv11
	umov    x0, v11.d[1]
	cmp     x0, x1
	b.ne    .Lv11
	mov     x1, #23052
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d12
	cmp     x0, x1
	b.ne    .Lv12
	umov    x0, v12.d[1]
	cmp     x0, x1
	b.ne    .Lv12
	mov     x1, #23053
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d13
	cmp     x0, x1
	b.ne    .Lv13
	umov    x0, v13.d[1]
	cmp     x0, x1
	b.ne    .Lv13
	mov     x1, #23054
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d14
	cmp     x0, x1
	b.ne    .Lv14
	umov    x0, v14.d[1]
	cmp     x0, x1
	b.ne    .Lv14
	mov     x1, #23055
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d15
	cmp     x0, x1
	b.ne    .Lv15
	umov    x0, v15.d[1]
	cmp     x0, x1
	b.ne    .Lv15
	mov     x1, #23056
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d16
	cmp     x0, x1
	b.ne    .Lv16
	umov    x0, v16.d[1]
	cmp     x0, x1
	b.ne    .Lv16
	mov     x1, #23057
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d17
	cmp     x0, x1
	b.ne    .Lv17
	umov    x0, v17.d[1]
	cmp     x0, x1
	b.ne    .Lv17
	mov     x1, #23058
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d18
	cmp     x0, x1
	b.ne    .Lv18
	umov    x0, v18.d[1]
	cmp     x0, x1
	b.ne    .Lv18
	mov     x1, #23059
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d19
	cmp     x0, x1
	b.ne    .Lv19
	umov    x0, v19.d[1]
	cmp     x0, x1
	b.ne    .Lv19
	mov     x1, #23060
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d20
	cmp     x0, x1
	b.ne    .Lv20
	umov    x0, v20.d[1]
	cmp     x0, x1
	b.ne    .Lv20
	mov     x1, #23061
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d21
	cmp     x0, x1
	b.ne    .Lv21
	umov    x0, v21.d[1]
	cmp     x0, x1
	b.ne    .Lv21
	mov     x1, #23062
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d22
	cmp     x0, x1
	b.ne    .Lv22
	umov    x0, v22.d[1]
	cmp     x0, x1
	b.ne    .Lv22
	mov     x1, #23063
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d23
	cmp     x0, x1
	b.ne    .Lv23
	umov    x0, v23.d[1]
	cmp     x0, x1
	b.ne    .Lv23
	mov     x1, #23064
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d24
	cmp     x0, x1
	b.ne    .Lv24
	umov    x0, v24.d[1]
	cmp     x0, x1
	b.ne    .Lv24
	mov     x1, #23065
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d25
	cmp     x0, x1
	b.ne    .Lv25
	umov    x0, v25.d[1]
	cmp     x0, x1
	b.ne    .Lv25
	mov     x1, #23066
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d26
	cmp     x0, x1
	b.ne    .Lv26
	umov    x0, v26.d[1]
	cmp     x0, x1
	b.ne    .Lv26
	mov     x1, #23067
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d27
	cmp     x0, x1
	b.ne    .Lv27
	umov    x0, v27.d[1]
	cmp     x0, x1
	b.ne    .Lv27
	mov     x1, #23068
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d28
	cmp     x0, x1
	b.ne    .Lv28
	umov    x0, v28.d[1]
	cmp     x0, x1
	b.ne    .Lv28
	mov     x1, #23069
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d29
	cmp     x0, x1
	b.ne    .Lv29
	umov    x0, v29.d[1]
	cmp     x0, x1
	b.ne    .Lv29
	mov     x1, #23070
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d30
	cmp     x0, x1
	b.ne    .Lv30
	umov    x0, v30.d[1]
	cmp     x0, x1
	b.ne    .Lv30
	mov     x1, #23071
	movk    x1, #0xc3c3, lsl #48
	fmov    x0, d31
	cmp     x0, x1
	b.ne    .Lv31
	umov    x0, v31.d[1]
	cmp     x0, x1
	b.ne    .Lv31
	subs    x28, x28, #1
	b.ne    .Lloop
	mov     x0, #42
	b       .Lexit
.Lnzcv:
	mov     x0, #99
	b       .Lexit
.Lx1:
	mov     x0, #101
	b       .Lexit
.Lx2:
	mov     x0, #102
	b       .Lexit
.Lx3:
	mov     x0, #103
	b       .Lexit
.Lx4:
	mov     x0, #104
	b       .Lexit
.Lx5:
	mov     x0, #105
	b       .Lexit
.Lx6:
	mov     x0, #106
	b       .Lexit
.Lx7:
	mov     x0, #107
	b       .Lexit
.Lx8:
	mov     x0, #108
	b       .Lexit
.Lx9:
	mov     x0, #109
	b       .Lexit
.Lx10:
	mov     x0, #110
	b       .Lexit
.Lx11:
	mov     x0, #111
	b       .Lexit
.Lx12:
	mov     x0, #112
	b       .Lexit
.Lx13:
	mov     x0, #113
	b       .Lexit
.Lx14:
	mov     x0, #114
	b       .Lexit
.Lx15:
	mov     x0, #115
	b       .Lexit
.Lx16:
	mov     x0, #116
	b       .Lexit
.Lx17:
	mov     x0, #117
	b       .Lexit
.Lx18:
	mov     x0, #118
	b       .Lexit
.Lx19:
	mov     x0, #119
	b       .Lexit
.Lx20:
	mov     x0, #120
	b       .Lexit
.Lx21:
	mov     x0, #121
	b       .Lexit
.Lx22:
	mov     x0, #122
	b       .Lexit
.Lx23:
	mov     x0, #123
	b       .Lexit
.Lx24:
	mov     x0, #124
	b       .Lexit
.Lx25:
	mov     x0, #125
	b       .Lexit
.Lx26:
	mov     x0, #126
	b       .Lexit
.Lx27:
	mov     x0, #127
	b       .Lexit
.Lx29:
	mov     x0, #129
	b       .Lexit
.Lx30:
	mov     x0, #130
	b       .Lexit
.Lv0:
	mov     x0, #140
	b       .Lexit
.Lv1:
	mov     x0, #141
	b       .Lexit
.Lv2:
	mov     x0, #142
	b       .Lexit
.Lv3:
	mov     x0, #143
	b       .Lexit
.Lv4:
	mov     x0, #144
	b       .Lexit
.Lv5:
	mov     x0, #145
	b       .Lexit
.Lv6:
	mov     x0, #146
	b       .Lexit
.Lv7:
	mov     x0, #147
	b       .Lexit
.Lv8:
	mov     x0, #148
	b       .Lexit
.Lv9:
	mov     x0, #149
	b       .Lexit
.Lv10:
	mov     x0, #150
	b       .Lexit
.Lv11:
	mov     x0, #151
	b       .Lexit
.Lv12:
	mov     x0, #152
	b       .Lexit
.Lv13:
	mov     x0, #153
	b       .Lexit
.Lv14:
	mov     x0, #154
	b       .Lexit
.Lv15:
	mov     x0, #155
	b       .Lexit
.Lv16:
	mov     x0, #156
	b       .Lexit
.Lv17:
	mov     x0, #157
	b       .Lexit
.Lv18:
	mov     x0, #158
	b       .Lexit
.Lv19:
	mov     x0, #159
	b       .Lexit
.Lv20:
	mov     x0, #160
	b       .Lexit
.Lv21:
	mov     x0, #161
	b       .Lexit
.Lv22:
	mov     x0, #162
	b       .Lexit
.Lv23:
	mov     x0, #163
	b       .Lexit
.Lv24:
	mov     x0, #164
	b       .Lexit
.Lv25:
	mov     x0, #165
	b       .Lexit
.Lv26:
	mov     x0, #166
	b       .Lexit
.Lv27:
	mov     x0, #167
	b       .Lexit
.Lv28:
	mov     x0, #168
	b       .Lexit
.Lv29:
	mov     x0, #169
	b       .Lexit
.Lv30:
	mov     x0, #170
	b       .Lexit
.Lv31:
	mov     x0, #171
	b       .Lexit
.Lexit:
	mov     x8, #94                  /* __NR_exit_group */
	svc     #0

	.section .note.GNU-stack,"",%progbits
