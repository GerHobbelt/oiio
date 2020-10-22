name = 'oiio'

version = '2.0.10-houdini-18.0.532-ta.1.0.0'

authors = [
    'benjamin.skinner',
]

requires = [
    'libtiff',
    'libpng',
]

variants = [
    ['platform-windows', 'arch-x64', 'os-windows-10'],
]

private_build_requires = [
    'python-3',
    'houdini-18.0.532',
]

build_command = "python {root}/rez_build.py"

def commands():

    # Split and store version and package version
    split_versions = str(version).split('-')
    env.OIIO_VERSION.set(split_versions[0])
    env.OIIO_PACKAGE_VERSION.set(split_versions[1])

    env.OPENIMAGEIO_ROOT_DIR.set( "{root}" )
    
    env.OIIO_LIB_DIR.set( "{root}/lib" )
    env.OIIO_INCLUDE_DIR.set( "{root}/include" )
    env.PATH.append( "{root}/bin" )
