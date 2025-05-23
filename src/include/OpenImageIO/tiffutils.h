// Copyright Contributors to the OpenImageIO project.
// SPDX-License-Identifier: Apache-2.0
// https://github.com/AcademySoftwareFoundation/OpenImageIO

// clang-format off

/////////////////////////////////////////////////////////////////////////////
/// \file
///
/// Utilities for dealing with TIFF tags and data structures (common to
/// plugins that have to deal with TIFF itself, Exif data blocks, and other
/// miscellaneous stuff that piggy-backs off TIFF format).
///
/////////////////////////////////////////////////////////////////////////////


#pragma once

extern "C" {
#include <tiff.h>
#include <tiffvers.h>
}

#include <OpenImageIO/imageio.h>


// Decode TIFFLIB_VERSION year to OIIO_LIBTIFF_VERSION number
#if TIFFLIB_VERSION >= 20210416
#    define OIIO_LIBTIFF_VERSION 40300
#elif TIFFLIB_VERSION >= 20201219
#    define OIIO_LIBTIFF_VERSION 40200
#elif TIFFLIB_VERSION >= 20191103
#    define OIIO_LIBTIFF_VERSION 40100
#elif TIFFLIB_VERSION >= 20181110
#    define OIIO_LIBTIFF_VERSION 40010
#elif TIFFLIB_VERSION >= 20171118
#    define OIIO_LIBTIFF_VERSION 40009
#elif TIFFLIB_VERSION >= 20170521
#    define OIIO_LIBTIFF_VERSION 40008
#elif TIFFLIB_VERSION >= 20161119
#    define OIIO_LIBTIFF_VERSION 40007
#elif TIFFLIB_VERSION >= 20150912
#    define OIIO_LIBTIFF_VERSION 40006
#elif TIFFLIB_VERSION >= 20150830
#    define OIIO_LIBTIFF_VERSION 40005
#elif TIFFLIB_VERSION >= 20150621
#    define OIIO_LIBTIFF_VERSION 40004
#elif TIFFLIB_VERSION >= 20120922
#    define OIIO_LIBTIFF_VERSION 40003
#elif TIFFLIB_VERSION >= 20120615
#    define OIIO_LIBTIFF_VERSION 40002
#elif TIFFLIB_VERSION >= 20120218
#    define OIIO_LIBTIFF_VERSION 40001
#elif TIFFLIB_VERSION >= 20111221
#    define OIIO_LIBTIFF_VERSION 40000
#elif TIFFLIB_VERSION >= 20090820
#    define OIIO_LIBTIFF_VERSION 30900
#elif TIFFLIB_VERSION >= 20051230
#    define OIIO_LIBTIFF_VERSION 30800
#elif TIFFLIB_VERSION >= 20041016
#    define OIIO_LIBTIFF_VERSION 30700
#else
#    define OIIO_LIBTIFF_VERSION 0
#endif

#if TIFFLIB_VERSION < 30900
#    error "Libtiff version is too old"
#endif



#ifdef TIFF_VERSION_BIG
// In old versions of TIFF, this was defined in tiff.h.  It's gone from
// "BIG TIFF" (libtiff 4.x), so we just define it here.

struct TIFFHeader {
    uint16_t tiff_magic;  /* magic number (defines byte order) */
    uint16_t tiff_version;/* TIFF version number */
    uint32_t tiff_diroff; /* byte offset to first directory */
};

struct TIFFDirEntry {
    uint16_t tdir_tag;    /* tag ID */
    uint16_t tdir_type;   /* data type -- see TIFFDataType enum */
    uint32_t tdir_count;  /* number of items; length in spec */
    uint32_t tdir_offset; /* byte offset to field data */
};
#endif



OIIO_NAMESPACE_BEGIN

// Define EXIF constants
enum TIFFTAG {
    EXIF_EXPOSURETIME               = 33434,
    EXIF_FNUMBER                    = 33437,
    EXIF_EXPOSUREPROGRAM            = 34850,
    EXIF_SPECTRALSENSITIVITY        = 34852,
    EXIF_PHOTOGRAPHICSENSITIVITY    = 34855,
    EXIF_ISOSPEEDRATINGS            = 34855, // old nme for PHOTOGRAPHICSENSITIVITY
    EXIF_OECF                       = 34856,
    EXIF_SENSITIVITYTYPE            = 34864,
    EXIF_STANDARDOUTPUTSENSITIVITY  = 34865,
    EXIF_RECOMMENDEDEXPOSUREINDEX   = 34866,
    EXIF_ISOSPEED                   = 34867,
    EXIF_ISOSPEEDLATITUDEYYY        = 34868,
    EXIF_ISOSPEEDLATITUDEZZZ        = 34869,
    EXIF_EXIFVERSION                = 36864,
    EXIF_DATETIMEORIGINAL           = 36867,
    EXIF_DATETIMEDIGITIZED          = 36868,
    EXIF_OFFSETTIME                 = 36880,
    EXIF_OFFSETTIMEORIGINAL         = 36881,
    EXIF_OFFSETTIMEDIGITIZED        = 36882,
    EXIF_COMPONENTSCONFIGURATION    = 37121,
    EXIF_COMPRESSEDBITSPERPIXEL     = 37122,
    EXIF_SHUTTERSPEEDVALUE          = 37377,
    EXIF_APERTUREVALUE              = 37378,
    EXIF_BRIGHTNESSVALUE            = 37379,
    EXIF_EXPOSUREBIASVALUE          = 37380,
    EXIF_MAXAPERTUREVALUE           = 37381,
    EXIF_SUBJECTDISTANCE            = 37382,
    EXIF_METERINGMODE               = 37383,
    EXIF_LIGHTSOURCE                = 37384,
    EXIF_FLASH                      = 37385,
    EXIF_FOCALLENGTH                = 37386,
    EXIF_SECURITYCLASSIFICATION     = 37394,
    EXIF_IMAGEHISTORY               = 37395,
    EXIF_SUBJECTAREA                = 37396,
    EXIF_MAKERNOTE                  = 37500,
    EXIF_USERCOMMENT                = 37510,
    EXIF_SUBSECTIME                 = 37520,
    EXIF_SUBSECTIMEORIGINAL         = 37521,
    EXIF_SUBSECTIMEDIGITIZED        = 37522,
    EXIF_TEMPERATURE                = 37888,
    EXIF_HUMIDITY                   = 37889,
    EXIF_PRESSURE                   = 37890,
    EXIF_WATERDEPTH                 = 37891,
    EXIF_ACCELERATION               = 37892,
    EXIF_CAMERAELEVATIONANGLE       = 37893,
    EXIF_FLASHPIXVERSION            = 40960,
    EXIF_COLORSPACE                 = 40961,
    EXIF_PIXELXDIMENSION            = 40962,
    EXIF_PIXELYDIMENSION            = 40963,
    EXIF_RELATEDSOUNDFILE           = 40964,
    EXIF_FLASHENERGY                = 41483,
    EXIF_SPATIALFREQUENCYRESPONSE   = 41484,
    EXIF_FOCALPLANEXRESOLUTION      = 41486,
    EXIF_FOCALPLANEYRESOLUTION      = 41487,
    EXIF_FOCALPLANERESOLUTIONUNIT   = 41488,
    EXIF_SUBJECTLOCATION            = 41492,
    EXIF_EXPOSUREINDEX              = 41493,
    EXIF_SENSINGMETHOD              = 41495,
    EXIF_FILESOURCE                 = 41728,
    EXIF_SCENETYPE                  = 41729,
    EXIF_CFAPATTERN                 = 41730,
    EXIF_CUSTOMRENDERED             = 41985,
    EXIF_EXPOSUREMODE               = 41986,
    EXIF_WHITEBALANCE               = 41987,
    EXIF_DIGITALZOOMRATIO           = 41988,
    EXIF_FOCALLENGTHIN35MMFILM      = 41989,
    EXIF_SCENECAPTURETYPE           = 41990,
    EXIF_GAINCONTROL                = 41991,
    EXIF_CONTRAST                   = 41992,
    EXIF_SATURATION                 = 41993,
    EXIF_SHARPNESS                  = 41994,
    EXIF_DEVICESETTINGDESCRIPTION   = 41995,
    EXIF_SUBJECTDISTANCERANGE       = 41996,
    EXIF_IMAGEUNIQUEID              = 42016,
    EXIF_CAMERAOWNERNAME            = 42032,
    EXIF_BODYSERIALNUMBER           = 42033,
    EXIF_LENSSPECIFICATION          = 42034,
    EXIF_LENSMAKE                   = 42035,
    EXIF_LENSMODEL                  = 42036,
    EXIF_LENSSERIALNUMBER           = 42037,
    EXIF_GAMMA                      = 42240,
    EXIF_COMPOSITEIMAGE             = 42080,
    EXIF_SOURCEIMAGENUMBEROFCOMPOSITEIMAGE   = 42081,
    EXIF_SOURCEEXPOSURETIMESOFCOMPOSITEIMAGE = 42082,
};



/// Given a TIFF data type code (defined in tiff.h) and a count, return the
/// equivalent TypeDesc where one exists. Return TypeUnknown if there is no
/// obvious equivalent.
OIIO_API TypeDesc tiff_datatype_to_typedesc (TIFFDataType tifftype, size_t tiffcount=1);

inline TypeDesc tiff_datatype_to_typedesc (const TIFFDirEntry& dir) {
    return tiff_datatype_to_typedesc (TIFFDataType(dir.tdir_type), dir.tdir_count);
}

/// Return the data size (in bytes) of the TIFF type.
OIIO_API size_t tiff_data_size (TIFFDataType tifftype);

/// Return the data size (in bytes) of the data for the given TIFFDirEntry.
OIIO_API size_t tiff_data_size (const TIFFDirEntry &dir);

/// Given a TIFFDirEntry and a data arena (represented by an array view
/// of unsigned bytes), return a span of where the values for the
/// tiff dir lives. Return an empty span if there is an error, which
/// could include a nonsensical situation where the TIFFDirEntry seems to
/// point outside the data arena.
OIIO_API cspan<uint8_t>
tiff_dir_data (const TIFFDirEntry &td, cspan<uint8_t> data);

/// Decode a raw Exif data block and save all the metadata in an
/// ImageSpec.  Return true if all is ok, false if the exif block was
/// somehow malformed.  The binary data pointed to by 'exif' should
/// start with a TIFF directory header.
OIIO_API bool decode_exif (cspan<uint8_t> exif, ImageSpec &spec);
OIIO_API bool decode_exif (string_view exif, ImageSpec &spec);

/// Construct an Exif data block from the ImageSpec, appending the Exif
/// data as a big blob to the char vector. Endianness can be specified with
/// endianreq, defaulting to the native endianness of the running platform.
OIIO_API void encode_exif (const ImageSpec &spec, std::vector<char> &blob,
                           OIIO::endian endianreq = endian::native);

/// Helper: For the given OIIO metadata attribute name, look up the Exif tag
/// ID, TIFFDataType (expressed as an int), and count. Return true and fill
/// in the fields if found, return false if not found.
OIIO_API bool exif_tag_lookup (string_view name, int &tag,
                               int &tifftype, int &count);

/// Add metadata to spec based on raw IPTC (International Press
/// Telecommunications Council) metadata in the form of an IIM
/// (Information Interchange Model).  Return true if all is ok, false if
/// the iptc block was somehow malformed.  This is a utility function to
/// make it easy for multiple format plugins to support embedding IPTC
/// metadata without having to duplicate functionality within each
/// plugin.  Note that IIM is actually considered obsolete and is
/// replaced by an XML scheme called XMP.
OIIO_API bool decode_iptc_iim (const void *iptc, int length, ImageSpec &spec);

/// Find all the IPTC-amenable metadata in spec and assemble it into an
/// IIM data block in iptc.  This is a utility function to make it easy
/// for multiple format plugins to support embedding IPTC metadata
/// without having to duplicate functionality within each plugin.  Note
/// that IIM is actually considered obsolete and is replaced by an XML
/// scheme called XMP. Return true if it was successful and any items
/// were encoded.
OIIO_API bool encode_iptc_iim (const ImageSpec &spec, std::vector<char> &iptc);

/// Add metadata to spec based on XMP data in an XML block.  Return true
/// if all is ok, false if the xml was somehow malformed.  This is a
/// utility function to make it easy for multiple format plugins to
/// support embedding XMP metadata without having to duplicate
/// functionality within each plugin.
OIIO_API bool decode_xmp (cspan<uint8_t> xml, ImageSpec &spec);
OIIO_API bool decode_xmp (string_view xml, ImageSpec &spec);

/// Find all the relevant metadata (IPTC, Exif, etc.) in spec and
/// assemble it into an XMP XML string.  This is a utility function to
/// make it easy for multiple format plugins to support embedding XMP
/// metadata without having to duplicate functionality within each
/// plugin.  If 'minimal' is true, then don't encode things that would
/// be part of ordinary TIFF or exif tags.
OIIO_API std::string encode_xmp (const ImageSpec &spec, bool minimal=false);


/// Add metadata to spec based on ICC data in a byte array.  Return true if
/// all is ok, false if the ICC was somehow malformed (and in that case, set
/// the contents of `error` to something useful).  This is a utility function
/// to make it easy for multiple format plugins to support embedding ICC
/// metadata without having to duplicate functionality within each plugin.
OIIO_API bool decode_icc_profile(cspan<uint8_t> iccdata, ImageSpec& spec,
                                 std::string& error);



/// Handy structure to hold information mapping TIFF/EXIF tags to their
/// names and actions.
struct TagInfo {
    typedef void (*HandlerFunc)(const TagInfo& taginfo, const TIFFDirEntry& dir,
                                cspan<uint8_t> buf, ImageSpec& spec,
                                bool swapendian, int offset_adjustment);

    TagInfo (int tag, const char *name, TIFFDataType type,
             int count, HandlerFunc handler = nullptr) noexcept
        : tifftag(tag), name(name), tifftype(type), tiffcount(count),
          handler(handler) {}

    int tifftag = -1;                     // TIFF tag used for this info
    const char *name = nullptr;           // OIIO attribute name we use
    TIFFDataType tifftype = TIFF_NOTYPE;  // Data type that TIFF wants
    int tiffcount = 0;                    // Number of items
    HandlerFunc handler = nullptr;        // Special decoding handler
};

/// Return a span of a TagInfo array for the corresponding table.
/// Valid names are "Exif", "GPS", and "TIFF". This can be handy for
/// iterating through all possible tags for each category.
OIIO_API cspan<TagInfo> tag_table (string_view tablename);

/// Look up the TagInfo of a numbered or named tag from a named domain
/// ("TIFF", "Exif", or "GPS"). Return nullptr if it is not known.
OIIO_API const TagInfo* tag_lookup (string_view domain, int tag);
OIIO_API const TagInfo* tag_lookup (string_view domain, string_view tagname);



OIIO_NAMESPACE_END
