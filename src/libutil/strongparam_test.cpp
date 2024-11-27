// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO


#include <OpenImageIO/strongparam.h>
#include <OpenImageIO/unittest.h>

#include <monolithic_examples.h>

using namespace OIIO;

OIIO_STRONG_PARAM_TYPE(Meters, float);
OIIO_STRONG_PARAM_TYPE(Seconds, float);

// Note: if you don't like using those macros, the following achieves roughly
// equivalent declarations:
//
//     using Meters = StrongParam<struct MetersTag, float>;
//     using Seconds = StrongParam<struct SecondsTag, float>;


float
speed(Meters a, Seconds b)
{
    return a / b;
}



#if defined(BUILD_MONOLITHIC)
#    define main oiio_strongparam_test_main
#endif

extern "C"
int
main(void)
{
    float s = speed(Meters(8.0f), Seconds(2.0f));
    OIIO_CHECK_EQUAL(s, 4.0f);

    return unit_test_failures;
}
