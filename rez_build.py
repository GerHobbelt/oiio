import os, subprocess, sys
import shutil

# TODO: We should build glew properly, but ben skinner didn't have the time
# I'm terribly sorry...

if __name__ == "__main__":
    src = os.environ["HOUDINI_ROOT"]
    dst = os.environ["REZ_BUILD_INSTALL_PATH"]
    inc_dst = os.environ["REZ_BUILD_INSTALL_PATH"] + "/include"
    lib_dst = os.environ["REZ_BUILD_INSTALL_PATH"] + "/lib"

    
    if 'win' in str(sys.platform):

        # Remove existing build
        if os.path.exists(dst):
            print(" - Removing existing build")
            shutil.rmtree(dst, ignore_errors=True)


        print ("Starting install")

        shutil.copytree(src + "/toolkit/include/OpenImageIO", inc_dst + "/OpenImageIO")
        
        os.mkdir(lib_dst)

        shutil.copy(src + '/custom/houdini/dsolib/OpenImageIO_sidefx.lib', lib_dst + "/OpenImageIO.lib")
        shutil.copy(src + '/custom/houdini/dsolib/OpenImageIO_Util_sidefx.lib', lib_dst + "/OpenImageIO_Util.lib")
    else:

        # Remove existing build
        if os.path.exists(dst):
            print(" - Removing existing build")
            shutil.rmtree(dst, ignore_errors=True)


        print ("Starting install")

        shutil.copytree(src + "/toolkit/include/OpenImageIO", inc_dst + "/OpenImageIO")
        
        os.mkdir(lib_dst)

        shutil.copy(src + '/dsolib/libOpenImageIO_sidefx.so.2.0.10', lib_dst + "/libOpenImageIO.so")
        shutil.copy(src + '/dsolib/libOpenImageIO_Util_sidefx.so.2.0.10', lib_dst + "/libOpenImageIO_Util.so")



    print ("Completed install")
