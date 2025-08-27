name = "oiio"

version = "2.5.9.0.hh.1.3.0"

authors = [
    "AcademySoftwareFoundation",
]

description = """Library and tools for reading and writing images"""

with scope("config") as c:
    import os

    c.release_packages_path = os.environ["HH_REZ_REPO_RELEASE_EXT"]

requires = [
    "libtiff",
    "libjpeg",
    "libpng",
    "freetype",
    # "pugixml-1.14",  # will use bundled-in pugixml
    "tbb-2020.3",
    "pybind11",
    # "ffmpeg",  # ffmpeg was built statically (because of conflicts with Houdini) and OIIO is complaining
    "openexr-3.1",  # will bring imath
    "openvdb-11",
]

private_build_requires = []

variants = [
    ["python-3.7", "ocio-2.1.3", "numpy-1.21.6"],
    ["python-3.7", "ocio-2.2.1", "numpy-1.21.6"],
    ["python-3.7", "ocio-2.3.2", "numpy-1.21.6"],
    ["python-3.9", "ocio-2.1.3", "numpy-1.26.4"],
    ["python-3.9", "ocio-2.2.1", "numpy-1.26.4"],
    ["python-3.9", "ocio-2.3.2", "numpy-1.26.4"],
    ["python-3.10", "ocio-2.1.3", "numpy-1.26.4"],
    ["python-3.10", "ocio-2.2.1", "numpy-1.26.4"],
    ["python-3.10", "ocio-2.3.2", "numpy-1.26.4"],
    ["python-3.11", "ocio-2.1.3", "numpy-1.26.4"],
    ["python-3.11", "ocio-2.2.1", "numpy-1.26.4"],
    ["python-3.11", "ocio-2.3.2", "numpy-1.26.4"],
    ["python-3.12", "ocio-2.1.3", "numpy-1.26.4"],
    ["python-3.12", "ocio-2.2.1", "numpy-1.26.4"],
    ["python-3.12", "ocio-2.3.2", "numpy-1.26.4"],
]


def commands():
    env.REZ_OIIO_ROOT = "{root}"
    env.OIIO_ROOT = "{root}"
    env.OIIO_LOCATION = "{root}"
    env.OIIO_INCLUDE_DIR = "{root}/include"
    env.OIIO_LIBRARY_DIR = "{root}/lib64"
    env.OPENIMAGEIOHOME = "{root}"  # for OpenShadingLanguage
    env.OPENIMAGEIO_ROOT_DIR = "{root}"  # for OpenColorIO

    env.PATH.append("{root}/bin")
    env.LD_LIBRARY_PATH.append("{root}/lib64")
    env.CMAKE_MODULE_PATH.append("{root}/lib64/cmake/OpenImageIO")

    if "python" in resolve:
        python_ver = resolve["python"].version
        if python_ver.major == 3:
            if python_ver.minor == 7:
                env.PYTHONPATH.append("{root}/lib64/python3.7/site-packages")
            elif python_ver.minor == 9:
                env.PYTHONPATH.append("{root}/lib64/python3.9/site-packages")
            elif python_ver.minor == 10:
                env.PYTHONPATH.append("{root}/lib64/python3.10/site-packages")
            elif python_ver.minor == 11:
                env.PYTHONPATH.append("{root}/lib64/python3.11/site-packages")
            elif python_ver.minor == 12:
                env.PYTHONPATH.append("{root}/lib64/python3.12/site-packages")


uuid = "repository.OpenImageIO"
build_system = "cmake"
