sgxfive_progs = [
	'sgxfive_ctx',
	'sgxfive_dev_query',
	'sgxfive_gate',
	'sgxfive_gem',
	'sgxfive_render',
	'sgxfive_submit',
	'sgxfive_transfer',
	'sgxfive_vm',
]

foreach prog : sgxfive_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : test_deps,
				       install_dir : sgxfivedir,
				       install_rpath : sgxfive_rpathdir,
				       install : true)
	test_list += join_paths('sgxfive', prog)
endforeach
