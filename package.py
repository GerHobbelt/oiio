name = 'oiio'

version = '2.0.10-houdini-18.5.499-ta.1.0.0'

authors = [
    'benjamin.skinner',
]

requires = [
    'libtiff',
    'libpng',
]

variants = [
    ['platform-windows', 'arch-x64', 'os-windows-10'],
    ['platform-linux', 'arch-x86_64', 'os-centos-7'],
]

private_build_requires = [
    'python',
    'houdini-18.5.499',
]

build_command = "python {root}/rez_build.py"

def commands():

    # Split and store version and package version
    split_versions = str(version).split('-')
    env.OIIO_VERSION.set(split_versions[0])
    env.OIIO_PACKAGE_VERSION.set(split_versions[1])

    env.OPENIMAGEIO_ROOT_DIR.set( "{root}" )
    env.OIIO_LOCATION.set( "{root}" )

    env.OIIO_LIB_DIR.set( "{root}/lib" )
    env.OIIO_INCLUDE_DIR.set( "{root}/include" )
    env.LD_LIBRARY_PATH.append( '{root}/lib64' )
    env.PATH.append( "{root}/bin" )
