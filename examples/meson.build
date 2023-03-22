libpng = dependency('libpng', required: false)

add_languages(
    'cpp',
)

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
        'x11.cc',
    ],
    dependencies: [
        neatvnc_dep,
        pixman,
        aml,
        libm,
        x11,
        xcomposite,
    ],
    cpp_args: '-std=c++17',
)

executable(
	'draw',
	[
		'draw.c',
	],
	dependencies: [
		neatvnc_dep,
		pixman,
		aml,
		libm,
	]
)

if libpng.found()
	executable(
		'png-server',
		[
			'png-server.c',
			'../src/pngfb.c',
		],
		dependencies: [
			neatvnc_dep,
			pixman,
			aml,
			libpng,
		]
	)
endif
