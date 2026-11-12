#!/usr/bin/env python3
"""Apply a bounded MSL address-space correction to a build-local source copy.
The pinned upstream source and license remain intact. OpenGL plain uniforms
passed as implicit global arguments retain the entry point's constant storage.
Ordinary function parameters and textures/samplers retain upstream behavior.
"""
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
old = '\tstring address_space = get_argument_address_space(var);\n\tbool builtin = has_decoration(var.self, DecorationBuiltIn);'
new = '''\tstring address_space = get_argument_address_space(var);
\t// OpenGL non-block uniforms are constant-buffer references at the entry
\t// point. An implicit global argument must retain that address space;
\t// ordinary value parameters remain thread-local copies.
\tif (arg.alias_global_variable && type_storage == StorageClassUniformConstant && !type_is_image && !type_is_tlas)
\t\taddress_space = "constant";
\tbool builtin = has_decoration(var.self, DecorationBuiltIn);'''
if source.count(old) != 1:
    raise SystemExit('Pinned SPIRV-Cross MSL argument implementation changed; review the address-space patch')
source = source.replace(old, new)
pathlib.Path(sys.argv[2]).write_text(source)
