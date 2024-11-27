
#pragma once

#if defined(BUILD_MONOLITHIC)

#ifdef __cplusplus
extern "C" {
#endif

int oiio_iconvert_main(int argc, const char** argv);
int oiio_idiff_main(int argc, const char** argv);
int oiio_igrep_main(int argc, const char** argv);
int oiio_iinfo_main(int argc, const char** argv);
int oiio_color_test_main(int argc, const char** argv);
int oiio_compute_test_main(int argc, const char** argv);
int oiio_imagebufalgo_test_main(int argc, const char** argv);
int oiio_imagebuf_test_main(void);
int oiio_imagecache_test_main(void);
int oiio_imageinout_test_main(int argc, const char** argv);
int oiio_imagespec_test_main(void);
int oiio_imagespeed_test_main(int argc, const char** argv);
int oiio_argparse_test_main(int argc, const char** argv);
int oiio_atomic_test_main(int argc, const char** argv);
int oiio_filesystem_test_main(void);
int oiio_filter_test_main(int argc, const char** argv);
int oiio_fmath_test_main(int argc, const char** argv);
int oiio_hash_test_main(int argc, const char** argv);
int oiio_optparser_test_main(void);
int oiio_parallel_test_main(int argc, const char** argv);
int oiio_paramlist_test_main(int argc, const char** argv);
int oiio_simd_test_main(int argc, const char** argv);
int oiio_span_test_main(int argc, const char** argv);
int oiio_spinlock_test_main(int argc, const char** argv);
int oiio_spin_rw_test_main(int argc, const char** argv);
int oiio_strongparam_test_main(void);
int oiio_strutil_test_main(void);
int oiio_thread_test_main(int argc, const char** argv);
int oiio_timer_test_main(int argc, const char** argv);
int oiio_typedesc_test_main(void);
int oiio_type_traits_test_main(void);
int oiio_ustring_test_main(int argc, const char** argv);
int oiio_maketx_main(int argc, const char** argv);
int oiio_oiiotool_main(int argc, const char** argv);
int oiio_testtex_main(int argc, const char** argv);
int oiio_imagebuf_main(void);
int oiio_imagebufalgo_main(void);
int oiio_imagecache_main(void);
int oiio_imageinput_main(void);
int oiio_imageioapi_main(void);
int oiio_imageoutput_main(void);
int oiio_texturesys_main(void);
int oiio_writingplugins_main(void);

#ifdef __cplusplus
}
#endif

#endif
