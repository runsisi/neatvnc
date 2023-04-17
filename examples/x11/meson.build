x11 = dependency(
	'x11',
	static: true,
)
xcomposite = dependency(
	'xcomposite',
	static: true,
)

executable(
	'x11',
	[
		'x11.c',
		'kmsgrab.c',
		'filter.c'
	],
	dependencies: [
		neatvnc_dep,
		pixman,
		aml,
		libm,
		x11,
		xcomposite,
		libdrm,
		libavcodec,
		libavfilter,
		libavutil,
	]
)
