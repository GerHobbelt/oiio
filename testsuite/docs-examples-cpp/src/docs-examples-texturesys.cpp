// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO


///////////////////////////////////////////////////////////////////////////
// This file contains code examples from the TextureSystem chapter of the
// main OpenImageIO documentation.
//
// To add an additional test, replicate the section below. Change
// "example1" to a helpful short name that identifies the example.

// BEGIN-texturesys-example1
#include <OpenImageIO/imageio.h>

#include <monolithic_examples.h>

using namespace OIIO;

static void example1()
{
    //
    // Example code fragment from the docs goes here.
    //
    // It probably should generate either some text output (which will show up
    // in "out.txt" that captures each test's output), or it should produce a
    // (small) image file that can be compared against a reference image that
    // goes in the ref/ subdirectory of this test.
    //
}
// END-texturesys-example1

//
///////////////////////////////////////////////////////////////////////////


#if defined(BUILD_MONOLITHIC)
#    define main oiio_texturesys_main
#endif

extern "C"
int
main(void)
{
    // Each example function needs to get called here, or it won't execute
    // as part of the test.
    example1();
    return 0;
}
