name = 'oiio'

version = '1.8.9-ta.2.0.0'

authors = [
    'benjamin.skinner',
]

variants = [
    ['platform-windows', 'arch-x64', 'os-windows-10', 'boost-1.69'],
    ['platform-windows', 'arch-x64', 'os-windows-10', 'boost-1.65'],
]

requires = [
    'zlib-1.2.11',
    'libtiff-4.0.9',
    'libpng-1.6.5',
    'openexr-2.4.0',
    'libjpeg_turbo-2.0.5',
    'ocio-1.1.1',
]

@early()
def private_build_requires():
    import sys
    if 'win' in str(sys.platform):
        return ['visual_studio']
    else:
        return ['gcc-7']

build_system = "cmake"

def commands():

    # Split and store version and package version
    split_versions = str(version).split('-')
    env.OIIO_VERSION.set(split_versions[0])
    env.OIIO_PACKAGE_VERSION.set(split_versions[1])

    env.OPENIMAGEIO_ROOT_DIR.set( "{root}" )
    
    env.OIIO_LIB_DIR.set( "{root}/lib" )
    env.OIIO_INCLUDE_DIR.set( "{root}/include" )
    env.PATH.append( "{root}/bin" )
