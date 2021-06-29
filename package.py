name = "oiio"

version = "1.5.24.x.1.0.2"

authors = [
    "Larry Gritz and Co."
]

description = \
    """
    Reading, writing, and processing images in a wide variety of file formats,
    using a format-agnostic API, aimed at VFX applications. 
    https://github.com/OpenImageIO/oiio
    """


requires = [
]

variants = [
    ["nuke-11.3v6"],
    ["nuke-12.2v3"]
]

# this is a pretty ugly way of building all the different variants but it works
# also, for some reason rez expands ths install_path to include the '/mnt/oracle/la', so
# we include some extra nastiness to strip it off so it doesn't get put into the RPATH of
# libraries
build_command = "cd {root};make clean;" \
                "export install_path=`echo {install_path} | sed 's/\/mnt\/oracle\/la//g'`;" \
                "[ {variant_index}==0 ] && make INSTALLDIR=$install_path USE_NUKE=1 NUKE_HOME=/usr/local/Nuke11.3v6 NUKE_VERSION=11.3 " \
                " || make INSTALLDIR=$install_path USE_NUKE=1 NUKE_HOME=/usr/local/Nuke12.2v3 NUKE_VERSION=12.2"


uuid = "oiio"

# this appears to be the recommended way to install internally developed packages
# into a non-default release path
# unclear if support for multiple release paths was actually implemented/released in rez:
#https://github.com/nerdvegas/rez/issues/426
#https://github.com/nerdvegas/rez/pull/402
with scope("config") as c:
#    release_path = str(c.release_packages_path)
#    c.release_packages_path = release_path.replace("external", "internal")
    c.release_packages_path = "/software/packages/internal"

def commands():
    env.NUKE_PATH.prepend("{root}/dist/linux64/lib/nuke/")
